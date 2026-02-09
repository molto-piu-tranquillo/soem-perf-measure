/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * EtherCAT RAW socket driver.
 *
 * Low level interface functions to send and receive EtherCAT packets.
 * EtherCAT has the property that packets are only send by the master,
 * and the send packets always return in the receive buffer.
 * There can be multiple packets "on the wire" before they return.
 * To combine the received packets with the original send packets a buffer
 * system is installed. The identifier is put in the index item of the
 * EtherCAT header. The index is stored and compared when a frame is received.
 * If there is a match the packet can be combined with the transmit packet
 * and returned to the higher level function.
 *
 * The socket layer can exhibit a reversal in the packet order (rare).
 * If the Tx order is A-B-C the return order could be A-C-B. The indexed buffer
 * will reorder the packets automatically.
 *
 * The "redundant" option will configure two sockets and two NIC interfaces.
 * Slaves are connected to both interfaces, one on the IN port and one on the
 * OUT port. Packets are send via both interfaces. Any one of the connections
 * (also an interconnect) can be removed and the slaves are still serviced with
 * packets. The software layer will detect the possible failure modes and
 * compensate. If needed the packets from interface A are resent through interface B.
 * This layer if fully transparent for the higher layers.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <netpacket/packet.h>
#include <pthread.h>

#ifdef USE_AF_XDP
#include <bpf/xsk.h>
#include <linux/if_link.h>
#include <sys/mman.h>
#include <poll.h>
#endif

#include "oshw.h"
#include "osal.h"
#include "perf_measure.h"

#ifndef PACKET_QDISC_BYPASS
#define PACKET_QDISC_BYPASS 20
#endif

#ifdef USE_AF_XDP
/* AF_XDP configuration constants */
#define XDP_NUM_FRAMES     512
#define XDP_FRAME_SIZE     4096
#define XDP_UMEM_SIZE      (XDP_NUM_FRAMES * XDP_FRAME_SIZE)  /* 2MB */
#define XDP_RX_RING_SIZE   256  /* match NUM_RX_DESC in r8169 */
#define XDP_TX_RING_SIZE   256  /* match NUM_TX_DESC in r8169 */
#define XDP_RX_FRAMES      256
#define XDP_TX_FRAME_START 256

/* Global AF_XDP context — single NIC, no redundancy */
static struct {
   struct xsk_umem *umem;
   struct xsk_socket *xsk;
   struct xsk_ring_prod fq;   /* fill queue (producer) */
   struct xsk_ring_prod tx;   /* TX queue (producer) */
   struct xsk_ring_cons cq;   /* completion queue (consumer) */
   struct xsk_ring_cons rx;   /* RX queue (consumer) */
   void *umem_area;
   int xsk_fd;
   int ifindex;
   __u32 tx_frame_idx;        /* round-robin TX frame index (32..63) */
} xdp_ctx;

/** Reclaim completed TX frames from completion queue */
static void xdp_complete_tx(void)
{
   __u32 idx;
   unsigned int completed;

   completed = xsk_ring_cons__peek(&xdp_ctx.cq, XDP_TX_RING_SIZE, &idx);
   if (completed > 0)
      xsk_ring_cons__release(&xdp_ctx.cq, completed);
}

/** Populate fill queue with RX frames (addresses 0..31 * FRAME_SIZE) */
static void xdp_fill_rx(void)
{
   __u32 idx;
   unsigned int i;

   if (xsk_ring_prod__reserve(&xdp_ctx.fq, XDP_RX_FRAMES, &idx) == XDP_RX_FRAMES)
   {
      for (i = 0; i < XDP_RX_FRAMES; i++)
         *xsk_ring_prod__fill_addr(&xdp_ctx.fq, idx + i) = (__u64)i * XDP_FRAME_SIZE;
      xsk_ring_prod__submit(&xdp_ctx.fq, XDP_RX_FRAMES);
   }
}
#endif /* USE_AF_XDP */

/** Redundancy modes */
enum
{
   /** No redundancy, single NIC mode */
   ECT_RED_NONE,
   /** Double redundant NIC connection */
   ECT_RED_DOUBLE
};

/** Primary source MAC address used for EtherCAT.
 * This address is not the MAC address used from the NIC.
 * EtherCAT does not care about MAC addressing, but it is used here to
 * differentiate the route the packet traverses through the EtherCAT
 * segment. This is needed to find out the packet flow in redundant
 * configurations. */
const uint16 priMAC[3] = {0x0101, 0x0101, 0x0101};
/** Secondary source MAC address used for EtherCAT. */
const uint16 secMAC[3] = {0x0404, 0x0404, 0x0404};

/** second MAC word is used for identification */
#define RX_PRIM priMAC[1]
/** second MAC word is used for identification */
#define RX_SEC secMAC[1]

static void ecx_clear_rxbufstat(int *rxbufstat)
{
   int i;
   for (i = 0; i < EC_MAXBUF; i++)
   {
      rxbufstat[i] = EC_BUF_EMPTY;
   }
}

/** Basic setup to connect NIC to socket.
 * @param[in] port        = port context struct
 * @param[in] ifname      = Name of NIC device, f.e. "eth0"
 * @param[in] secondary   = if >0 then use secondary stack instead of primary
 * @return >0 if succeeded
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
   int i;
   int rval;
   int *psock;
   pthread_mutexattr_t mutexattr;

   rval = 0;
   if (secondary)
   {
      /* secondary port struct available? */
      if (port->redport)
      {
         /* when using secondary socket it is automatically a redundant setup */
         psock = &(port->redport->sockhandle);
         *psock = -1;
         port->redstate = ECT_RED_DOUBLE;
         port->redport->stack.sock = &(port->redport->sockhandle);
         port->redport->stack.txbuf = &(port->txbuf);
         port->redport->stack.txbuflength = &(port->txbuflength);
         port->redport->stack.tempbuf = &(port->redport->tempinbuf);
         port->redport->stack.rxbuf = &(port->redport->rxbuf);
         port->redport->stack.rxbufstat = &(port->redport->rxbufstat);
         port->redport->stack.rxsa = &(port->redport->rxsa);
         ecx_clear_rxbufstat(&(port->redport->rxbufstat[0]));
      }
      else
      {
         /* fail */
         return 0;
      }
   }
   else
   {
      pthread_mutexattr_init(&mutexattr);
      pthread_mutexattr_setprotocol(&mutexattr, PTHREAD_PRIO_INHERIT);
      pthread_mutex_init(&(port->getindex_mutex), &mutexattr);
      pthread_mutex_init(&(port->tx_mutex), &mutexattr);
      pthread_mutex_init(&(port->rx_mutex), &mutexattr);
      port->sockhandle = -1;
      port->lastidx = 0;
      port->redstate = ECT_RED_NONE;
      port->stack.sock = &(port->sockhandle);
      port->stack.txbuf = &(port->txbuf);
      port->stack.txbuflength = &(port->txbuflength);
      port->stack.tempbuf = &(port->tempinbuf);
      port->stack.rxbuf = &(port->rxbuf);
      port->stack.rxbufstat = &(port->rxbufstat);
      port->stack.rxsa = &(port->rxsa);
      ecx_clear_rxbufstat(&(port->rxbufstat[0]));
      psock = &(port->sockhandle);
   }

#ifdef USE_AF_XDP
   if (!secondary)
   {
      int r;
      struct ifreq ifr;
      struct xsk_umem_config umem_cfg;
      struct xsk_socket_config xsk_cfg;
      int tmp_sock;

      /* get interface index */
      xdp_ctx.ifindex = if_nametoindex(ifname);
      if (!xdp_ctx.ifindex)
      {
         printf("AF_XDP: if_nametoindex failed for %s\n", ifname);
         return 0;
      }

      /* set NIC to promiscuous mode via temporary socket */
      tmp_sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));
      if (tmp_sock >= 0)
      {
         strcpy(ifr.ifr_name, ifname);
         ioctl(tmp_sock, SIOCGIFFLAGS, &ifr);
         ifr.ifr_flags |= IFF_PROMISC | IFF_BROADCAST;
         ioctl(tmp_sock, SIOCSIFFLAGS, &ifr);
         close(tmp_sock);
      }

      /* allocate UMEM area — try hugepage first, fallback to normal mmap */
      xdp_ctx.umem_area = mmap(NULL, XDP_UMEM_SIZE,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                               -1, 0);
      if (xdp_ctx.umem_area == MAP_FAILED)
      {
         xdp_ctx.umem_area = mmap(NULL, XDP_UMEM_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS,
                                  -1, 0);
         if (xdp_ctx.umem_area == MAP_FAILED)
         {
            printf("AF_XDP: mmap UMEM failed\n");
            return 0;
         }
      }

      /* create UMEM */
      umem_cfg.fill_size = XDP_RX_RING_SIZE;
      umem_cfg.comp_size = XDP_TX_RING_SIZE;
      umem_cfg.frame_size = XDP_FRAME_SIZE;
      umem_cfg.frame_headroom = 0;
      umem_cfg.flags = 0;

      r = xsk_umem__create(&xdp_ctx.umem, xdp_ctx.umem_area, XDP_UMEM_SIZE,
                           &xdp_ctx.fq, &xdp_ctx.cq, &umem_cfg);
      if (r)
      {
         printf("AF_XDP: xsk_umem__create failed: %d\n", r);
         munmap(xdp_ctx.umem_area, XDP_UMEM_SIZE);
         return 0;
      }

      /* create XSK socket — zero-copy first, copy mode fallback */
      xsk_cfg.rx_size = XDP_RX_RING_SIZE;
      xsk_cfg.tx_size = XDP_TX_RING_SIZE;
      xsk_cfg.libbpf_flags = 0;
      xsk_cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
      xsk_cfg.bind_flags = XDP_ZEROCOPY | XDP_USE_NEED_WAKEUP;

      r = xsk_socket__create(&xdp_ctx.xsk, ifname, 0,
                             xdp_ctx.umem, &xdp_ctx.rx, &xdp_ctx.tx, &xsk_cfg);
      if (r)
      {
         printf("AF_XDP: zero-copy failed (%d), trying copy mode\n", r);
         xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
         xsk_cfg.bind_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;
         r = xsk_socket__create(&xdp_ctx.xsk, ifname, 0,
                                xdp_ctx.umem, &xdp_ctx.rx, &xdp_ctx.tx, &xsk_cfg);
         if (r)
         {
            printf("AF_XDP: xsk_socket__create failed: %d\n", r);
            xsk_umem__delete(xdp_ctx.umem);
            munmap(xdp_ctx.umem_area, XDP_UMEM_SIZE);
            return 0;
         }
      }

      xdp_ctx.xsk_fd = xsk_socket__fd(xdp_ctx.xsk);
      *psock = xdp_ctx.xsk_fd;
      xdp_ctx.tx_frame_idx = XDP_TX_FRAME_START;

      /* populate fill queue with RX frame addresses */
      xdp_fill_rx();

      rval = 1;
   }
   else
   {
      /* secondary: use AF_PACKET (redundancy path, rarely used) */
      int r, ifindex;
      struct timeval timeout;
      struct ifreq ifr;
      struct sockaddr_ll sll;

      *psock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));
      timeout.tv_sec = 0;
      timeout.tv_usec = 1;
      r = setsockopt(*psock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      r = setsockopt(*psock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      i = 1;
      r = setsockopt(*psock, SOL_SOCKET, SO_DONTROUTE, &i, sizeof(i));
      strcpy(ifr.ifr_name, ifname);
      r = ioctl(*psock, SIOCGIFINDEX, &ifr);
      ifindex = ifr.ifr_ifindex;
      strcpy(ifr.ifr_name, ifname);
      ifr.ifr_flags = 0;
      r = ioctl(*psock, SIOCGIFFLAGS, &ifr);
      ifr.ifr_flags = ifr.ifr_flags | IFF_PROMISC | IFF_BROADCAST;
      r = ioctl(*psock, SIOCSIFFLAGS, &ifr);
      sll.sll_family = AF_PACKET;
      sll.sll_ifindex = ifindex;
      sll.sll_protocol = htons(ETH_P_ECAT);
      r = bind(*psock, (struct sockaddr *)&sll, sizeof(sll));
      if (r == 0) rval = 1;
   }
#else
   {
      /* AF_PACKET path (original) */
      int r, ifindex;
      struct timeval timeout;
      struct ifreq ifr;
      struct sockaddr_ll sll;

      /* we use RAW packet socket, with packet type ETH_P_ECAT */
      *psock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));

      /* QDisc Bypass: skip traffic control layer on TX */
      int qdisc_bypass = 1;
      if (setsockopt(*psock, SOL_PACKET, PACKET_QDISC_BYPASS,
                     &qdisc_bypass, sizeof(qdisc_bypass)) < 0)
      {
      }

      /* Socket Priority */
      int prio = 6;
      if (setsockopt(*psock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0)
      {
      }

      timeout.tv_sec = 0;
      timeout.tv_usec = 1;
      r = setsockopt(*psock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      r = setsockopt(*psock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      i = 1;
      r = setsockopt(*psock, SOL_SOCKET, SO_DONTROUTE, &i, sizeof(i));
      /* connect socket to NIC by name */
      strcpy(ifr.ifr_name, ifname);
      r = ioctl(*psock, SIOCGIFINDEX, &ifr);
      ifindex = ifr.ifr_ifindex;
      strcpy(ifr.ifr_name, ifname);
      ifr.ifr_flags = 0;
      /* reset flags of NIC interface */
      r = ioctl(*psock, SIOCGIFFLAGS, &ifr);
      /* set flags of NIC interface, here promiscuous and broadcast */
      ifr.ifr_flags = ifr.ifr_flags | IFF_PROMISC | IFF_BROADCAST;
      r = ioctl(*psock, SIOCSIFFLAGS, &ifr);
      /* bind socket to protocol, in this case RAW EtherCAT */
      sll.sll_family = AF_PACKET;
      sll.sll_ifindex = ifindex;
      sll.sll_protocol = htons(ETH_P_ECAT);
      r = bind(*psock, (struct sockaddr *)&sll, sizeof(sll));
      if (r == 0)
         rval = 1;
   }
#endif

   /* setup ethernet headers in tx buffers so we don't have to repeat it */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));

   return rval;
}

/** Close sockets used
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port)
{
#ifdef USE_AF_XDP
   if (xdp_ctx.xsk)
   {
      xsk_socket__delete(xdp_ctx.xsk);
      xdp_ctx.xsk = NULL;
      port->sockhandle = -1; /* fd closed by xsk_socket__delete */
   }
   if (xdp_ctx.umem)
   {
      xsk_umem__delete(xdp_ctx.umem);
      xdp_ctx.umem = NULL;
   }
   if (xdp_ctx.umem_area)
   {
      munmap(xdp_ctx.umem_area, XDP_UMEM_SIZE);
      xdp_ctx.umem_area = NULL;
   }
#endif
   if (port->sockhandle >= 0)
      close(port->sockhandle);
   if ((port->redport) && (port->redport->sockhandle >= 0))
      close(port->redport->sockhandle);

   return 0;
}

/** Fill buffer with ethernet header structure.
 * Destination MAC is always broadcast.
 * Ethertype is always ETH_P_ECAT.
 * @param[out] p = buffer
 */
void ec_setupheader(void *p)
{
   ec_etherheadert *bp;
   bp = p;
   bp->da0 = htons(0xffff);
   bp->da1 = htons(0xffff);
   bp->da2 = htons(0xffff);
   bp->sa0 = htons(priMAC[0]);
   bp->sa1 = htons(priMAC[1]);
   bp->sa2 = htons(priMAC[2]);
   bp->etype = htons(ETH_P_ECAT);
}

/** Get new frame identifier index and allocate corresponding rx buffer.
 * @param[in] port        = port context struct
 * @return new index.
 */
int ecx_getindex(ecx_portt *port)
{
   int idx;
   int cnt;

   pthread_mutex_lock(&(port->getindex_mutex));

   idx = port->lastidx + 1;
   /* index can't be larger than buffer array */
   if (idx >= EC_MAXBUF)
   {
      idx = 0;
   }
   cnt = 0;
   /* try to find unused index */
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF)
      {
         idx = 0;
      }
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

   pthread_mutex_unlock(&(port->getindex_mutex));

   return idx;
}

/** Set rx buffer status.
 * @param[in] port        = port context struct
 * @param[in] idx      = index in buffer array
 * @param[in] bufstat  = status to set
 */
void ecx_setbufstat(ecx_portt *port, int idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
   if (port->redstate != ECT_RED_NONE)
      port->redport->rxbufstat[idx] = bufstat;
}

/** Transmit buffer over socket (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx         = index in tx buffer array
 * @param[in] stacknumber  = 0=Primary 1=Secondary stack
 * @return socket send result
 */
int ecx_outframe(ecx_portt *port, int idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;

#ifdef USE_AF_XDP
   if (!stacknumber)
   {
      __u32 tx_idx;
      struct xdp_desc *desc;
      void *frame_ptr;
      __u64 frame_addr;

      /* reclaim completed TX frames */
      xdp_complete_tx();

      /* get TX frame address (round-robin frames 32..63) */
      frame_addr = (__u64)xdp_ctx.tx_frame_idx * XDP_FRAME_SIZE;
      xdp_ctx.tx_frame_idx++;
      if (xdp_ctx.tx_frame_idx >= XDP_NUM_FRAMES)
         xdp_ctx.tx_frame_idx = XDP_TX_FRAME_START;

      /* copy frame data to UMEM TX region */
      frame_ptr = xsk_umem__get_data(xdp_ctx.umem_area, frame_addr);
      memcpy(frame_ptr, (*stack->txbuf)[idx], lp);

      /* reserve TX descriptor */
      if (xsk_ring_prod__reserve(&xdp_ctx.tx, 1, &tx_idx) != 1)
      {
         (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
         return -1;
      }

      desc = xsk_ring_prod__tx_desc(&xdp_ctx.tx, tx_idx);
      desc->addr = frame_addr;
      desc->len = lp;

      xsk_ring_prod__submit(&xdp_ctx.tx, 1);

      /* kick kernel to transmit */
      if (xsk_ring_prod__needs_wakeup(&xdp_ctx.tx))
         sendto(xdp_ctx.xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);

      rval = lp;
   }
   else
   {
      /* secondary: AF_PACKET */
      rval = send(*stack->sock, (*stack->txbuf)[idx], lp, 0);
      if (rval == -1)
      {
         (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
      }
   }
#else
   rval = send(*stack->sock, (*stack->txbuf)[idx], lp, 0);
   if (rval == -1)
   {
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
   }
#endif

   return rval;
}

/** Transmit buffer over socket (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx = index in tx buffer array
 * @return socket send result
 */
int ecx_outframe_red(ecx_portt *port, int idx)
{
   ec_comt *datagramP;
   ec_etherheadert *ehp;
   int rval;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   /* rewrite MAC source address 1 to primary */
   ehp->sa1 = htons(priMAC[1]);
   /* transmit over primary socket*/
   rval = ecx_outframe(port, idx, 0);
   if (port->redstate != ECT_RED_NONE)
   {
      pthread_mutex_lock(&(port->tx_mutex));
      ehp = (ec_etherheadert *)&(port->txbuf2);
      /* use dummy frame for secondary socket transmit (BRD) */
      datagramP = (ec_comt *)&(port->txbuf2[ETH_HEADERSIZE]);
      /* write index to frame */
      datagramP->index = idx;
      /* rewrite MAC source address 1 to secondary */
      ehp->sa1 = htons(secMAC[1]);
      /* transmit over secondary socket */
      port->redport->rxbufstat[idx] = EC_BUF_TX;
      if (send(port->redport->sockhandle, &(port->txbuf2), port->txbuflength2, 0) == -1)
      {
         port->redport->rxbufstat[idx] = EC_BUF_EMPTY;
      }
      pthread_mutex_unlock(&(port->tx_mutex));
   }

   return rval;
}

/** Non blocking read of socket. Put frame in temporary buffer.
 * @param[in] port        = port context struct
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return >0 if frame is available and read
 */
static int ecx_recvpkt(ecx_portt *port, int stacknumber)
{
   int lp, bytesrx;
   ec_stackT *stack;

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   lp = sizeof(port->tempinbuf);

#ifdef USE_AF_XDP
   if (!stacknumber)
   {
      __u32 idx;
      const struct xdp_desc *desc;
      __u64 addr;

      /* non-blocking peek for available RX frames */
      if (xsk_ring_cons__peek(&xdp_ctx.rx, 1, &idx) == 0)
      {
         /* no data — poll with 1ms timeout to yield CPU to ksoftirqd.
          * CRITICAL: SOEM(FIFO 99) must sleep so ksoftirqd(FIFO 99)
          * can run NAPI poll and deliver packets to XSK ring. */
         struct pollfd pfd;
         pfd.fd = xdp_ctx.xsk_fd;
         pfd.events = POLLIN;
         poll(&pfd, 1, 1);

         /* retry after poll */
         if (xsk_ring_cons__peek(&xdp_ctx.rx, 1, &idx) == 0)
         {
            port->tempinbufs = 0;
            return 0;
         }
      }

      desc = xsk_ring_cons__rx_desc(&xdp_ctx.rx, idx);
      bytesrx = desc->len;
      addr = desc->addr;
      if (bytesrx > lp)
         bytesrx = lp;

      /* copy from UMEM to tempinbuf */
      memcpy((*stack->tempbuf),
             xsk_umem__get_data(xdp_ctx.umem_area, addr),
             bytesrx);

      /* release RX descriptor */
      xsk_ring_cons__release(&xdp_ctx.rx, 1);

      /* return frame to fill queue for reuse */
      {
         __u32 fq_idx;
         if (xsk_ring_prod__reserve(&xdp_ctx.fq, 1, &fq_idx) == 1)
         {
            *xsk_ring_prod__fill_addr(&xdp_ctx.fq, fq_idx) = addr;
            xsk_ring_prod__submit(&xdp_ctx.fq, 1);
         }
      }
   }
   else
   {
      /* secondary: AF_PACKET */
      bytesrx = recv(*stack->sock, (*stack->tempbuf), lp, 0);
   }
#else
   bytesrx = recv(*stack->sock, (*stack->tempbuf), lp, 0);
#endif

   port->tempinbufs = bytesrx;

   return (bytesrx > 0);
}

/** Non blocking receive frame function. Uses RX buffer and index to combine
 * read frame with transmitted frame. To compensate for received frames that
 * are out-of-order all frames are stored in their respective indexed buffer.
 * If a frame was placed in the buffer previously, the function retrieves it
 * from that buffer index without calling ec_recvpkt. If the requested index
 * is not already in the buffer it calls ec_recvpkt to fetch it. There are
 * three options now, 1 no frame read, so exit. 2 frame read but other
 * than requested index, store in buffer and exit. 3 frame read with matching
 * index, store in buffer, set completed flag in buffer status and exit.
 *
 * @param[in] port        = port context struct
 * @param[in] idx         = requested index of frame
 * @param[in] stacknumber = 0=primary 1=secondary stack
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME or EC_OTHERFRAME.
 */
int ecx_inframe(ecx_portt *port, int idx, int stacknumber)
{
   uint16 l;
   int rval;
   int idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   static PerfMeasure my_timer;
   static int timer_inited = 0;

   if (!timer_inited)
   {
      pm_init(&my_timer, "My Test Timer");
      timer_inited = 1;
   }

   if (!stacknumber)
   {
      stack = &(port->stack);
   }
   else
   {
      stack = &(port->redport->stack);
   }
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];
   /* check if requested index is already in buffer ? */
   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      /* return WKC */
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      /* mark as completed */
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else
   {
//       pm_start(&my_timer); //시작
      pthread_mutex_lock(&(port->rx_mutex));
//       pm_end(&my_timer);   //종료

      /* non blocking call to retrieve frame from socket */
//      pm_start(&my_timer);    //시작
      if (ecx_recvpkt(port, stacknumber))
      {
//         pm_end(&my_timer);   //종료
         rval = EC_OTHERFRAME;
         ehp = (ec_etherheadert *)(stack->tempbuf);
         /* check if it is an EtherCAT frame */
         if (ehp->etype == htons(ETH_P_ECAT))
         {
            ecp = (ec_comt *)(&(*stack->tempbuf)[ETH_HEADERSIZE]);
            l = etohs(ecp->elength) & 0x0fff;
            idxf = ecp->index;
            /* found index equals requested index ? */
            if (idxf == idx)
            {
               /* yes, put it in the buffer array (strip ethernet header) */
               memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
               /* return WKC */
               rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
               /* mark as completed */
               (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
               /* store MAC source word 1 for redundant routing info */
               (*stack->rxsa)[idx] = ntohs(ehp->sa1);
            }
            else
            {
               /* check if index exist and someone is waiting for it */
               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  /* put it in the buffer array (strip ethernet header) */
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  /* mark as received */
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = ntohs(ehp->sa1);
               }
               else
               {
                  /* strange things happened */
               }
            }
         }
      }
      pthread_mutex_unlock(&(port->rx_mutex));
   }

   /* WKC if matching frame found */
   return rval;
}

/** Blocking redundant receive frame function. If redundant mode is not active then
 * it skips the secondary stack and redundancy functions. In redundant mode it waits
 * for both (primary and secondary) frames to come in. The result goes in an decision
 * tree that decides, depending on the route of the packet and its possible missing arrival,
 * how to reroute the original packet to get the data in an other try.
 *
 * @param[in] port        = port context struct
 * @param[in] idx = requested index of frame
 * @param[in] timer = absolute timeout time
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
static int ecx_waitinframe_red(ecx_portt *port, int idx, osal_timert *timer)
{
   osal_timert timer2;
   int wkc = EC_NOFRAME;
   int wkc2 = EC_NOFRAME;
   int primrx, secrx;

   /* if not in redundant mode then always assume secondary is OK */
   if (port->redstate == ECT_RED_NONE)
      wkc2 = 0;
   do
   {
      /* only read frame if not already in */
      if (wkc <= EC_NOFRAME)
      {
         wkc = ecx_inframe(port, idx, 0);
      }
      /* only try secondary if in redundant mode */
      if (port->redstate != ECT_RED_NONE)
      {
         /* only read frame if not already in */
         if (wkc2 <= EC_NOFRAME)
            wkc2 = ecx_inframe(port, idx, 1);
      }
      /* wait for both frames to arrive or timeout */
   } while (((wkc <= EC_NOFRAME) || (wkc2 <= EC_NOFRAME)) && !osal_timer_is_expired(timer));
   /* only do redundant functions when in redundant mode */
   if (port->redstate != ECT_RED_NONE)
   {
      /* primrx if the received MAC source on primary socket */
      primrx = 0;
      if (wkc > EC_NOFRAME)
         primrx = port->rxsa[idx];
      /* secrx if the received MAC source on psecondary socket */
      secrx = 0;
      if (wkc2 > EC_NOFRAME)
         secrx = port->redport->rxsa[idx];

      /* primary socket got secondary frame and secondary socket got primary frame */
      /* normal situation in redundant mode */
      if (((primrx == RX_SEC) && (secrx == RX_PRIM)))
      {
         /* copy secondary buffer to primary */
         memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         wkc = wkc2;
      }
      /* primary socket got nothing or primary frame, and secondary socket got secondary frame */
      /* we need to resend TX packet */
      if (((primrx == 0) && (secrx == RX_SEC)) ||
          ((primrx == RX_PRIM) && (secrx == RX_SEC)))
      {
         /* If both primary and secondary have partial connection retransmit the primary received
          * frame over the secondary socket. The result from the secondary received frame is a combined
          * frame that traversed all slaves in standard order. */
         if ((primrx == RX_PRIM) && (secrx == RX_SEC))
         {
            /* copy primary rx to tx buffer */
            memcpy(&(port->txbuf[idx][ETH_HEADERSIZE]), &(port->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
         }
         osal_timer_start(&timer2, EC_TIMEOUTRET);
         /* resend secondary tx */
         ecx_outframe(port, idx, 1);
         do
         {
            /* retrieve frame */
            wkc2 = ecx_inframe(port, idx, 1);
         } while ((wkc2 <= EC_NOFRAME) && !osal_timer_is_expired(&timer2));
         if (wkc2 > EC_NOFRAME)
         {
            /* copy secondary result to primary rx buffer */
            memcpy(&(port->rxbuf[idx]), &(port->redport->rxbuf[idx]), port->txbuflength[idx] - ETH_HEADERSIZE);
            wkc = wkc2;
         }
      }
   }

   /* return WKC or EC_NOFRAME */
   return wkc;
}

/** Blocking receive frame function. Calls ec_waitinframe_red().
 * @param[in] port        = port context struct
 * @param[in] idx       = requested index of frame
 * @param[in] timeout   = timeout in us
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
int ecx_waitinframe(ecx_portt *port, int idx, int timeout)
{
   int wkc;
   osal_timert timer;

   osal_timer_start(&timer, timeout);
   wkc = ecx_waitinframe_red(port, idx, &timer);

   return wkc;
}

/** Blocking send and receive frame function. Used for non processdata frames.
 * A datagram is build into a frame and transmitted via this function. It waits
 * for an answer and returns the workcounter. The function retries if time is
 * left and the result is WKC=0 or no frame received.
 *
 * The function calls ec_outframe_red() and ec_waitinframe_red().
 *
 * @param[in] port        = port context struct
 * @param[in] idx      = index of frame
 * @param[in] timeout  = timeout in us
 * @return Workcounter or EC_NOFRAME
 */
int ecx_srconfirm(ecx_portt *port, int idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer1, timer2;

   osal_timer_start(&timer1, timeout);
   do
   {
      /* tx frame on primary and if in redundant mode a dummy on secondary */
      ecx_outframe_red(port, idx);
      if (timeout < EC_TIMEOUTRET)
      {
         osal_timer_start(&timer2, timeout);
      }
      else
      {
         /* normally use partial timeout for rx */
         osal_timer_start(&timer2, EC_TIMEOUTRET);
      }
      /* get frame from primary or if in redundant mode possibly from secondary */
      wkc = ecx_waitinframe_red(port, idx, &timer2);
      /* wait for answer with WKC>=0 or otherwise retry until timeout */
   } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(&timer1));

   return wkc;
}

#ifdef EC_VER1
int ec_setupnic(const char *ifname, int secondary)
{
   return ecx_setupnic(&ecx_port, ifname, secondary);
}

int ec_closenic(void)
{
   return ecx_closenic(&ecx_port);
}

int ec_getindex(void)
{
   return ecx_getindex(&ecx_port);
}

void ec_setbufstat(int idx, int bufstat)
{
   ecx_setbufstat(&ecx_port, idx, bufstat);
}

int ec_outframe(int idx, int stacknumber)
{
   return ecx_outframe(&ecx_port, idx, stacknumber);
}

int ec_outframe_red(int idx)
{
   return ecx_outframe_red(&ecx_port, idx);
}

int ec_inframe(int idx, int stacknumber)
{
   return ecx_inframe(&ecx_port, idx, stacknumber);
}

int ec_waitinframe(int idx, int timeout)
{
   return ecx_waitinframe(&ecx_port, idx, timeout);
}

int ec_srconfirm(int idx, int timeout)
{
   return ecx_srconfirm(&ecx_port, idx, timeout);
}
#endif

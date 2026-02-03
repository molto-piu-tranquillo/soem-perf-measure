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
#include <strings.h>
#include <linux/if_packet.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdlib.h>

#include "oshw.h"
#include "osal.h"
#include "perf_measure.h"

#ifndef PACKET_QDISC_BYPASS
#define PACKET_QDISC_BYPASS 20
#endif

#ifndef EC_PACKET_MMAP_ENV
#define EC_PACKET_MMAP_ENV "EC_PACKET_MMAP"
#endif
#ifndef EC_PACKET_MMAP_BLOCKS
#define EC_PACKET_MMAP_BLOCKS 8
#endif
#ifndef EC_PACKET_MMAP_PAGES_PER_BLOCK
#define EC_PACKET_MMAP_PAGES_PER_BLOCK 8
#endif
#ifndef EC_PACKET_MMAP_POLL_NS
#define EC_PACKET_MMAP_POLL_NS 1000
#endif

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

static unsigned int ecx_gcd_u32(unsigned int a, unsigned int b)
{
   while (b != 0)
   {
      unsigned int t = a % b;
      a = b;
      b = t;
   }
   return a;
}

static unsigned int ecx_lcm_u32(unsigned int a, unsigned int b)
{
   unsigned int g;
   unsigned long long lcm;

   if ((a == 0) || (b == 0))
   {
      return 0;
   }
   g = ecx_gcd_u32(a, b);
   lcm = (unsigned long long)(a / g) * (unsigned long long)b;
   if (lcm > 0xFFFFFFFFULL)
   {
      return 0;
   }
   return (unsigned int)lcm;
}

static void ecx_init_packet_mmap(ec_stackT *stack)
{
   stack->use_packet_mmap = 0;
   stack->rx_ring = NULL;
   stack->rx_ring_size = 0;
   stack->rx_frame_size = 0;
   stack->rx_frame_nr = 0;
   stack->rx_frame_idx = 0;
}

static void ecx_packet_mmap_wait(void)
{
   struct timespec ts;

   ts.tv_sec = 0;
   ts.tv_nsec = EC_PACKET_MMAP_POLL_NS;
   (void)nanosleep(&ts, NULL);
}

static int ecx_packet_mmap_enabled(void)
{
   const char *env = getenv(EC_PACKET_MMAP_ENV);
   if ((env == NULL) || (env[0] == '\0'))
   {
      return 1; /* default on */
   }
   if ((strcmp(env, "0") == 0) ||
       (strcasecmp(env, "false") == 0) ||
       (strcasecmp(env, "off") == 0) ||
       (strcasecmp(env, "no") == 0))
   {
      return 0;
   }
   return 1;
}

static int ecx_setup_packet_mmap(ec_stackT *stack, int sock)
{
   struct tpacket_req req;
   unsigned int frame_size;
   unsigned int page_size;
   unsigned int block_size;
   unsigned int frames_per_block;
   unsigned int block_nr;
   unsigned int frame_nr;
   unsigned long long block_size_ll;
   unsigned long long min_block_size_ll;
   size_t ring_size;
   void *ring;
   int version;

   if (!ecx_packet_mmap_enabled())
   {
      return 0;
   }

   version = TPACKET_V1;
   if (setsockopt(sock, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0)
   {
      return 0;
   }

   frame_size = TPACKET_ALIGN(TPACKET_HDRLEN + EC_MAXECATFRAME);
   page_size = (unsigned int)getpagesize();
   block_size = ecx_lcm_u32(page_size, frame_size);
   if (block_size == 0)
   {
      return 0;
   }
   min_block_size_ll = (unsigned long long)page_size * (unsigned long long)EC_PACKET_MMAP_PAGES_PER_BLOCK;
   block_size_ll = (unsigned long long)block_size;
   while (block_size_ll < min_block_size_ll)
   {
      block_size_ll += (unsigned long long)block_size;
      if (block_size_ll > 0xFFFFFFFFULL)
      {
         return 0;
      }
   }
   block_size = (unsigned int)block_size_ll;
   frames_per_block = block_size / frame_size;
   if (frames_per_block == 0)
   {
      return 0;
   }
   block_nr = EC_PACKET_MMAP_BLOCKS;
   frame_nr = frames_per_block * block_nr;

   memset(&req, 0, sizeof(req));
   req.tp_block_size = block_size;
   req.tp_block_nr = block_nr;
   req.tp_frame_size = frame_size;
   req.tp_frame_nr = frame_nr;

   if (setsockopt(sock, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0)
   {
      return 0;
   }

   ring_size = (size_t)req.tp_block_size * req.tp_block_nr;
   ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, sock, 0);
   if (ring == MAP_FAILED)
   {
      struct tpacket_req req_empty;
      memset(&req_empty, 0, sizeof(req_empty));
      setsockopt(sock, SOL_PACKET, PACKET_RX_RING, &req_empty, sizeof(req_empty));
      return 0;
   }

   stack->use_packet_mmap = 1;
   stack->rx_ring = ring;
   stack->rx_ring_size = ring_size;
   stack->rx_frame_size = req.tp_frame_size;
   stack->rx_frame_nr = req.tp_frame_nr;
   stack->rx_frame_idx = 0;

   return 1;
}

static void ecx_teardown_packet_mmap(ec_stackT *stack, int sock)
{
   struct tpacket_req req;

   if (!stack->use_packet_mmap || (stack->rx_ring == NULL))
   {
      return;
   }
   memset(&req, 0, sizeof(req));
   setsockopt(sock, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req));
   munmap(stack->rx_ring, stack->rx_ring_size);
   ecx_init_packet_mmap(stack);
}

static int ecx_recvpkt_mmap(ecx_portt *port, ec_stackT *stack)
{
   struct tpacket_hdr *hdr;
   uint8 *pkt;
   unsigned int snaplen;

   hdr = (struct tpacket_hdr *)((uint8 *)stack->rx_ring + (stack->rx_frame_idx * stack->rx_frame_size));
   if ((hdr->tp_status & TP_STATUS_USER) == 0)
   {
      ecx_packet_mmap_wait();
      if ((hdr->tp_status & TP_STATUS_USER) == 0)
      {
         return 0;
      }
   }
   __sync_synchronize();

   pkt = (uint8 *)hdr + hdr->tp_mac;
   snaplen = hdr->tp_snaplen;
   if (snaplen > sizeof(ec_bufT))
   {
      snaplen = sizeof(ec_bufT);
   }
   memcpy(*stack->tempbuf, pkt, snaplen);
   port->tempinbufs = (int)snaplen;

   __sync_synchronize();
   hdr->tp_status = TP_STATUS_KERNEL;
   stack->rx_frame_idx++;
   if (stack->rx_frame_idx >= stack->rx_frame_nr)
   {
      stack->rx_frame_idx = 0;
   }

   return 1;
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
   int r, rval, ifindex;
   struct timeval timeout;
   struct ifreq ifr;
   struct sockaddr_ll sll;
   int *psock;
   ec_stackT *stack;
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
         stack = &(port->redport->stack);
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
      stack = &(port->stack);
      psock = &(port->sockhandle);
   }
   ecx_init_packet_mmap(stack);
   /* we use RAW packet socket, with packet type ETH_P_ECAT */
   *psock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));

   // =========================================================================
   // [추가된 코드 시작] 리얼타임 성능 최적화 (QDisc Bypass & Busy Poll)
   // =========================================================================

   // 1. QDisc Bypass: 송신 시 트래픽 제어 계층을 무시하고 드라이버로 직행 (Lock 제거)
   int qdisc_bypass = 1;
   if (setsockopt(*psock, SOL_PACKET, PACKET_QDISC_BYPASS, &qdisc_bypass, sizeof(qdisc_bypass)) < 0)
   {
      // 에러 로그를 남기거나, 커널 버전이 낮아서 지원 안 할 경우 무시
      // printf("Warning: PACKET_QDISC_BYPASS failed\n");
   }

   // 2. Busy Poll: AF_PACKET raw socket에서는 동작하지 않음 (검증 완료 2026-01-30)
   // - sk_busy_loop()이 AF_PACKET에서 napi_busy_loop을 호출하지 않음
   // - 설정 시 오히려 send-recv 전 구간 +5μs 악화 (mean 24.8→29.5, max 88→132μs)
   // - TCP/UDP 소켓 전용 기능
//   int busy_poll_us = 50;
//   if (setsockopt(*psock, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us)) < 0)
//   {
//       printf("Warning: SO_BUSY_POLL failed\n");
//   }

   // 3. Socket Priority: 소켓 우선순위를 최고(7)로 설정
   int prio = 6; // TC_PRIO_INTERACTIVE or equivalent
   if (setsockopt(*psock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0)
   {
      // printf("Warning: SO_PRIORITY failed\n");
   }

   // =========================================================================
   // [추가된 코드 끝]
   // =========================================================================

   if (*psock >= 0)
   {
      (void)ecx_setup_packet_mmap(stack, *psock);
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
   /* setup ethernet headers in tx buffers so we don't have to repeat it */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));
   if (r == 0)
      rval = 1;

   return rval;
}

/** Close sockets used
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port)
{
   if (port->sockhandle >= 0)
   {
      ecx_teardown_packet_mmap(&(port->stack), port->sockhandle);
      close(port->sockhandle);
   }
   if ((port->redport) && (port->redport->sockhandle >= 0))
   {
      ecx_teardown_packet_mmap(&(port->redport->stack), port->redport->sockhandle);
      close(port->redport->sockhandle);
   }

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
   rval = send(*stack->sock, (*stack->txbuf)[idx], lp, 0);
   if (rval == -1)
   {
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
   }

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
   if (stack->use_packet_mmap)
   {
      return ecx_recvpkt_mmap(port, stack);
   }
   lp = sizeof(port->tempinbuf);

   // static PerfMeasure my_timer;
   // static int timer_inited = 0;

   // if (!timer_inited)
   // {
   //    pm_init(&my_timer, "My Test Timer");
   //    timer_inited = 1;
   // }

   // pm_start(&my_timer);
   bytesrx = recv(*stack->sock, (*stack->tempbuf), lp, 0);
   // pm_end(&my_timer);

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

//    static PerfMeasure my_timer;
//    static int timer_inited = 0;
//
//    if (!timer_inited)
//    {
//       pm_init(&my_timer, "My Test Timer");
//       timer_inited = 1;
//    }

   /* if not in redundant mode then always assume secondary is OK */
   if (port->redstate == ECT_RED_NONE)
      wkc2 = 0;
   do
   {
      /* only read frame if not already in */
      if (wkc <= EC_NOFRAME)
      {
//                  pm_start(&my_timer); //시작
         wkc = ecx_inframe(port, idx, 0);
//                  pm_end(&my_timer);   //종료
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

   // static PerfMeasure my_timer;
   // static int timer_inited = 0;

   // if (!timer_inited)
   // {
   //    pm_init(&my_timer, "My Test Timer");
   //    timer_inited = 1;
   // }

   // pm_start(&my_timer);
   osal_timer_start(&timer, timeout);
   // pm_end(&my_timer); // 종료

   // pm_start(&my_timer); // 시작
   wkc = ecx_waitinframe_red(port, idx, &timer);
   // pm_end(&my_timer); // 종료

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

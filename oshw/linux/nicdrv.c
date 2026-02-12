/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

/** \file
 * \brief
 * EtherCAT UIO driver for RTL8168H.
 *
 * Drives the NIC directly via UIO (mmap BAR + DMA descriptors).
 * No kernel network stack, no NAPI, no syscalls for TX/RX.
 * Accepts PCI BDF (e.g. "0000:01:00.0") or interface name (e.g. "eth0")
 * as ifname argument. If interface name is given, it is resolved to PCI BDF.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <glob.h>

#include "oshw.h"
#include "osal.h"
#include "r8169_uio.h"

/* Global UIO device context — single NIC, no redundancy */
static struct r8169_uio uio_dev;

/** Redundancy modes */
enum
{
   /** No redundancy, single NIC mode */
   ECT_RED_NONE,
   /** Double redundant NIC connection */
   ECT_RED_DOUBLE
};

const uint16 priMAC[3] = {0x0101, 0x0101, 0x0101};
const uint16 secMAC[3] = {0x0404, 0x0404, 0x0404};

#define RX_PRIM priMAC[1]
#define RX_SEC secMAC[1]

/** Resolve interface name (e.g. "eth0") to PCI BDF (e.g. "0000:01:00.0").
 *  If ifname already looks like a BDF, return it as-is.
 *  @return 0 on success, -1 on failure */
static int resolve_pci_bdf(const char *ifname, char *bdf, size_t bdflen)
{
   /* Check if already a PCI BDF: contains ':' and '.' */
   if (strchr(ifname, ':') && strchr(ifname, '.'))
   {
      snprintf(bdf, bdflen, "%s", ifname);
      return 0;
   }

   /* Resolve via sysfs: /sys/class/net/<ifname>/device -> ../../<BDF> */
   char path[256];
   char link[256];
   snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
   ssize_t len = readlink(path, link, sizeof(link) - 1);
   if (len < 0)
   {
      /* Interface gone (already bound to UIO). Fall back to default BDF. */
      printf("UIO: %s not found, using default %s\n", ifname, RTL8168_PCI_BDF);
      snprintf(bdf, bdflen, "%s", RTL8168_PCI_BDF);
      return 0;
   }
   link[len] = '\0';
   /* link is something like "../../devices/pci0000:00/.../0000:01:00.0" */
   const char *last = strrchr(link, '/');
   if (!last)
      return -1;
   snprintf(bdf, bdflen, "%s", last + 1);
   return 0;
}

static void ecx_clear_rxbufstat(int *rxbufstat)
{
   int i;
   for (i = 0; i < EC_MAXBUF; i++)
   {
      rxbufstat[i] = EC_BUF_EMPTY;
   }
}

/** Basic setup to connect NIC via UIO.
 * @param[in] port        = port context struct
 * @param[in] ifname      = PCI BDF (e.g. "0000:01:00.0") or interface name (e.g. "eth0")
 * @param[in] secondary   = if >0 then use secondary stack (not supported in UIO)
 * @return >0 if succeeded
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
   int i;
   pthread_mutexattr_t mutexattr;

   if (secondary)
   {
      printf("UIO: redundancy not supported\n");
      return 0;
   }

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

   /* Resolve ifname to PCI BDF */
   char pci_bdf[32];
   if (resolve_pci_bdf(ifname, pci_bdf, sizeof(pci_bdf)) < 0)
      return 0;

   if (r8169_uio_init(&uio_dev, pci_bdf) < 0)
   {
      printf("UIO: failed to init %s\n", pci_bdf);
      return 0;
   }
   port->sockhandle = uio_dev.uio_fd;

   /* setup ethernet headers in tx buffers */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
      port->rxbufstat[i] = EC_BUF_EMPTY;
   }
   ec_setupheader(&(port->txbuf2));

   return 1;
}

/** Close UIO device
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port)
{
   (void)port;
   r8169_uio_close(&uio_dev);
   return 0;
}

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

int ecx_getindex(ecx_portt *port)
{
   int idx;
   int cnt;

   pthread_mutex_lock(&(port->getindex_mutex));

   idx = port->lastidx + 1;
   if (idx >= EC_MAXBUF)
      idx = 0;
   cnt = 0;
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF)
         idx = 0;
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

   pthread_mutex_unlock(&(port->getindex_mutex));

   return idx;
}

void ecx_setbufstat(ecx_portt *port, int idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
}

/** Transmit buffer via UIO (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx         = index in tx buffer array
 * @param[in] stacknumber = 0=Primary (only option for UIO)
 * @return bytes sent or -1
 */
int ecx_outframe(ecx_portt *port, int idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;

   (void)stacknumber;
   stack = &(port->stack);
   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;

   rval = r8169_tx(&uio_dev, (*stack->txbuf)[idx], lp);
   if (rval == -1)
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;

   return rval;
}

int ecx_outframe_red(ecx_portt *port, int idx)
{
   ec_etherheadert *ehp;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   ehp->sa1 = htons(priMAC[1]);
   return ecx_outframe(port, idx, 0);
}

/** Non blocking read via UIO. Put frame in temporary buffer.
 * @param[in] port        = port context struct
 * @param[in] stacknumber = ignored (UIO has no secondary)
 * @return >0 if frame is available and read
 */
static int ecx_recvpkt(ecx_portt *port, int stacknumber)
{
   int lp, bytesrx;
   ec_stackT *stack;

   (void)stacknumber;
   stack = &(port->stack);
   lp = sizeof(port->tempinbuf);

   r8169_tx_complete(&uio_dev);
   bytesrx = r8169_rx(&uio_dev, (*stack->tempbuf), lp);
   port->tempinbufs = bytesrx;

   return (bytesrx > 0);
}

int ecx_inframe(ecx_portt *port, int idx, int stacknumber)
{
   uint16 l;
   int rval;
   int idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   (void)stacknumber;
   stack = &(port->stack);
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];

   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else
   {
      pthread_mutex_lock(&(port->rx_mutex));
      if (ecx_recvpkt(port, 0))
      {
         rval = EC_OTHERFRAME;
         ehp = (ec_etherheadert *)(stack->tempbuf);
         if (ehp->etype == htons(ETH_P_ECAT))
         {
            ecp = (ec_comt *)(&(*stack->tempbuf)[ETH_HEADERSIZE]);
            l = etohs(ecp->elength) & 0x0fff;
            idxf = ecp->index;
            if (idxf == idx)
            {
               memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
               rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
               (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
               (*stack->rxsa)[idx] = ntohs(ehp->sa1);
            }
            else
            {
               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = ntohs(ehp->sa1);
               }
            }
         }
      }
      pthread_mutex_unlock(&(port->rx_mutex));
   }

   return rval;
}

static int ecx_waitinframe_red(ecx_portt *port, int idx, osal_timert *timer)
{
   int wkc = EC_NOFRAME;

   do
   {
      if (wkc <= EC_NOFRAME)
         wkc = ecx_inframe(port, idx, 0);
   } while ((wkc <= EC_NOFRAME) && !osal_timer_is_expired(timer));

   return wkc;
}

int ecx_waitinframe(ecx_portt *port, int idx, int timeout)
{
   int wkc;
   osal_timert timer;

   osal_timer_start(&timer, timeout);
   wkc = ecx_waitinframe_red(port, idx, &timer);

   return wkc;
}

int ecx_srconfirm(ecx_portt *port, int idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer1, timer2;

   osal_timer_start(&timer1, timeout);
   do
   {
      ecx_outframe_red(port, idx);
      if (timeout < EC_TIMEOUTRET)
         osal_timer_start(&timer2, timeout);
      else
         osal_timer_start(&timer2, EC_TIMEOUTRET);
      wkc = ecx_waitinframe_red(port, idx, &timer2);
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

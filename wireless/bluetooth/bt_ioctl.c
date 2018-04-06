/****************************************************************************
 * wireless/bluetooth/bt_ioctl.c
 * Bluetooth network IOCTL handler
 *
 *   Copyright (C) 2018 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/wqueue.h>
#include <nuttx/semaphore.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/bluetooth.h>
#include <nuttx/wireless/bt_core.h>
#include <nuttx/wireless/bt_hci.h>
#include <nuttx/wireless/bt_ioctl.h>

#include "bt_hcicore.h"
#include "bt_conn.h"
#include "bt_ioctl.h"

#ifdef CONFIG_NETDEV_IOCTL  /* Not optional! */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* This structure encapsulates all globals used by the IOCTL logic */

struct btnet_scanstate_s
{
  sem_t bs_exclsem;                 /* Manages exclusive access */
  bool bs_scanning;                 /* True:  Scanning in progress */
  uint8_t bs_head;                  /* Head of circular list (for removal) */
  uint8_t bs_tail;                  /* Tail of circular list (for addition) */

  struct bt_scanresponse_s bs_rsp[CONFIG_BLUETOOTH_MAXSCANRESULT];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* At present only a single Bluetooth device is supported.  So we can simply
 * maintain the scan state as a global.
 */

static struct btnet_scanstate_s g_scanstate;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: btnet_scan_callback
 *
 * Description:
 *   This is an HCI callback function.  HCI provides scan result data via
 *   this callback function.  The scan result data will be added to the
 *   cached scan results.
 *
 * Input Parameters:
 *   Scan result data
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void btnet_scan_callback(FAR const bt_addr_le_t *addr, int8_t rssi,
                                uint8_t adv_type, FAR const uint8_t *adv_data,
                                uint8_t len)
{
  FAR struct bt_scanresponse_s *rsp;
  uint8_t nexttail;
  uint8_t head;
  uint8_t tail;
  int ret;

  if (!g_scanstate.bs_scanning)
    {
      wlerr("ERROR:  Results received while not scanning\n");
      return;
    }

   if (len > CONFIG_BLUETOOTH_MAXSCANDATA)
     {
       wlerr("ERROR: Scan result is too big:  %u\n", len);
       return;
     }

  /* Get exclusive access to the scan data */

  while ((ret = nxsem_wait(&g_scanstate.bs_exclsem)) < 0)
    {
      DEBUGASSERT(ret == -EINTR || ret == -ECANCELED);
      if (ret != -EINTR)
        {
          return;
        }
    }

  /* Add the scan data to the cache */

  tail     = g_scanstate.bs_tail;
  nexttail = tail + 1;

  if (nexttail >= CONFIG_BLUETOOTH_MAXSCANRESULT)
    {
      nexttail = 0;
    }

  /* Is the circular buffer full? */

  head = g_scanstate.bs_head;
  if (nexttail == head)
    {
      wlerr("ERROR: Too many scan results\n");

      if (++head >= CONFIG_BLUETOOTH_MAXSCANRESULT)
        {
          head = 0;
        }

      g_scanstate.bs_head = head;
    }

  /* Save the new scan result */

  rsp = &g_scanstate.bs_rsp[tail];
  memcpy(&rsp->sr_addr, addr, sizeof(bt_addr_le_t));
  rsp->sr_rssi = rssi;
  rsp->sr_type = adv_type;
  rsp->sr_len  = len;
  memcpy(&rsp->sr_data, adv_data, len);

  g_scanstate.bs_tail = nexttail;
  nxsem_post(&g_scanstate.bs_exclsem);
}

/****************************************************************************
 * Name: btnet_scan_result
 *
 * Description:
 *   This is an HCI callback function.  HCI provides scan result data via
 *   this callback function.  The scan result data will be added to the
 *   cached scan results.
 *
 * Input Parameters:
 *   result - Location to return the scan result data
 *   maxrsp - The maximum number of responses that can be returned.
 *
 * Returned Value:
 *   On success, the actual number of scan results obtain is returned.  A
 *   negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int btnet_scan_result(FAR struct bt_scanresponse_s *result,
                             uint8_t maxrsp)
{
  uint8_t head;
  uint8_t tail;
  uint8_t nrsp;
  int ret;

  if (!g_scanstate.bs_scanning)
    {
      wlerr("ERROR:  Results received while not scanning\n");
      return -EPIPE;
    }

  /* Get exclusive access to the scan data */

  ret = nxsem_wait(&g_scanstate.bs_exclsem);
  if (ret < 0)
    {
      DEBUGASSERT(ret == -EINTR || ret == -ECANCELED);
      return ret;
    }

  /* Copy all available results */

  head   = g_scanstate.bs_head;
  tail   = g_scanstate.bs_tail;

  for (nrsp = 0; nrsp < maxrsp && head != tail; nrsp++)
    {
      FAR const uint8_t *src;
      FAR uint8_t *dest;

      /* Copy data from the head index into the user buffer */

      src  = (FAR const uint8_t *)&g_scanstate.bs_rsp[head];
      dest = (FAR uint8_t *)&result[nrsp];
      memcpy(dest, src, sizeof(struct bt_scanresponse_s));

      /* Increment the head index */

      if (++head >= CONFIG_BLUETOOTH_MAXSCANRESULT)
        {
          head = 0;
        }
    }

  g_scanstate.bs_head = head;
  nxsem_post(&g_scanstate.bs_exclsem);
  return nrsp;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: btnet_ioctl
 *
 * Description:
 *   Handle network IOCTL commands directed to this device.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *   cmd - The IOCTL command
 *   arg - The argument for the IOCTL command
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 ****************************************************************************/

int btnet_ioctl(FAR struct net_driver_s *dev, int cmd, unsigned long arg)
{
  FAR struct btreq_s *btreq = (FAR struct btreq_s *)((uintptr_t)arg);
  int ret;

  wlinfo("cmd=%04x arg=%ul\n", cmd, arg);
  DEBUGASSERT(dev != NULL && dev->d_private != NULL);

  if (btreq == NULL)
    {
      return -EINVAL;
    }

  wlinfo("ifname: %s\n", btreq->btr_name);

  /* Process the command */

  switch (cmd)
    {
       /* SIOCGBTINFO:  Get Bluetooth device Info.  Given the device name,
        * fill in the btreq_s structure.
        *
        * REVISIT:  Little more than a stub at present.  It does return the
        * device address associated with the device name which in itself is
        * important.
        */

       case SIOCGBTINFO:
         {
           memset(&btreq->btru.btri, 0, sizeof(btreq->btru.btri));
           BLUETOOTH_ADDRCOPY(btreq->btr_bdaddr.val, g_btdev.bdaddr.val);
           btreq->btr_num_cmd = CONFIG_BLUETOOTH_BUFFER_PREALLOC;
           btreq->btr_num_acl = CONFIG_BLUETOOTH_BUFFER_PREALLOC;
           btreq->btr_acl_mtu = BLUETOOTH_MAX_MTU;
           btreq->btr_sco_mtu = BLUETOOTH_MAX_MTU;
           btreq->btr_max_acl = CONFIG_IOB_NBUFFERS;
           ret                = OK;
         }
         break;

      /* SIOCBTADVSTART:  Set advertisement data, scan response data,
       * advertisement parameters and start advertising.
       */

      case SIOCBTADVSTART:
        {
          ret = bt_start_advertising(btreq->btr_advtype,
                                     btreq->btr_advad,
                                     btreq->btr_advad);
          wlinfo("Start advertising: %d\n", ret);
        }
        break;

      /* SIOCBTADVSTOP:   Stop advertising. */

      case SIOCBTADVSTOP:
        {
          wlinfo("Stop advertising\n");
          bt_stop_advertising();
          ret = OK;
        }
        break;

      /* SIOCBTSCANSTART:  Start LE scanning.  Buffered scan results may be
       * obtained via SIOCBTSCANGET
       */

      case SIOCBTSCANSTART:
        {
          /* Are we already scanning? */

          if (g_scanstate.bs_scanning)
            {
              ret = -EBUSY;
            }
          else
            {
              /* Initialize scan state */

              nxsem_init(&g_scanstate.bs_exclsem, 0, 1);
              g_scanstate.bs_scanning = true;
              g_scanstate.bs_head     = 0;
              g_scanstate.bs_tail     = 0;

              ret = bt_start_scanning(btreq->btr_dupenable,
                                      btnet_scan_callback);
              wlinfo("Start scan: %d\n", ret);

              if (ret < 0)
                {
                  nxsem_destroy(&g_scanstate.bs_exclsem);
                  g_scanstate.bs_scanning = false;
                }
            }
        }
        break;

      /* SIOCBTSCANGET:  Return scan results buffered since the call time
       * that the SIOCBTSCANGET command was invoked.
       */

      case SIOCBTSCANGET:
        {
          ret = btnet_scan_result(btreq->btr_rsp, btreq->btr_nrsp);
          wlinfo("Get scan results: %d\n", ret);

          if (ret >= 0)
            {
              btreq->btr_nrsp = ret;
              ret = OK;
            }
        }
        break;

      /* SIOCBTSCANSTOP:  Stop LE scanning and discard any buffered results. */

      case SIOCBTSCANSTOP:
        {
          /* Stop scanning */

          ret = bt_stop_scanning();
          wlinfo("Stop scanning: %d\n", ret);

          nxsem_destroy(&g_scanstate.bs_exclsem);
          g_scanstate.bs_scanning = false;
        }
        break;

      /* SIOCBTSECURITY:  Enable security for a connection. */

      case SIOCBTSECURITY:
        {
          FAR struct bt_conn_s *conn;

          /* Get the connection associated with the provided LE address */

          conn = bt_conn_lookup_addr_le(&btreq->btr_secaddr);
          if (conn == NULL)
            {
              wlwarn("WARNING:  Peer not connected\n");
              ret = -ENOTCONN;
            }
          else
            {
              ret = bt_conn_security(conn, btreq->btr_seclevel);
              if (ret < 0)
                {
                  wlerr("ERROR:  Security setting failed: %d\n", ret);
                }

              bt_conn_release(conn);
            }
        }
        break;

      default:
        wlwarn("WARNING: Unrecognized IOCTL command: %02x\n", cmd);
        ret = -ENOTTY;
        break;
    }

  return ret;
}

#endif /* CONFIG_NETDEV_IOCTL */

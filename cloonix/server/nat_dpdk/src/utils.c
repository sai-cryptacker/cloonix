/*****************************************************************************/
/*    Copyright (C) 2006-2020 clownix@clownix.net License AGPL-3             */
/*                                                                           */
/*  This program is free software: you can redistribute it and/or modify     */
/*  it under the terms of the GNU Affero General Public License as           */
/*  published by the Free Software Foundation, either version 3 of the       */
/*  License, or (at your option) any later version.                          */
/*                                                                           */
/*  This program is distributed in the hope that it will be useful,          */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of           */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            */
/*  GNU Affero General Public License for more details.a                     */
/*                                                                           */
/*  You should have received a copy of the GNU Affero General Public License */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*                                                                           */
/*****************************************************************************/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/un.h>

#define ALLOW_EXPERIMENTAL_API
#include <rte_compat.h>
#include <rte_bus_pci.h>
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_meter.h>
#include <rte_pci.h>
#include <rte_version.h>
#include <rte_vhost.h>
#include <rte_arp.h>

#include "io_clownix.h"
#include "rpc_clownix.h"
#include "utils.h"

static uint32_t g_mac_len;
static uint32_t g_ip_len;
static uint32_t g_udp_len;
static uint32_t g_tcp_len;

/*****************************************************************************/
static uint16_t utils_in_cksum(uint16_t *addr, unsigned int count)
{
  register uint32_t sum = 0;
  while (count > 1)
    {
    sum += * addr++;
    count -= 2;
    }
  if(count > 0)
    {
    sum += ((*addr)&htons(0xFF00));
    }
  while (sum>>16)
    sum = (sum & 0xffff) + (sum >> 16);
  sum = ~sum;
  return ((uint16_t) sum);
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
static uint32_t utils_frame_cksum(int len, uint8_t *message)
{
  int i, j;
  uint32_t crc, mask;
  uint8_t byte;
  crc = 0xFFFFFFFF;
  for (j = 0; j < len; j++)
    {
    byte = message[j];
    crc = crc ^ byte;
    for (i = 7; i >= 0; i--)
      {
      mask = -(crc & 1);
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
      }
    }
  return ~crc;
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
static void utils_fill_mac_header(uint8_t *data, uint8_t *smac, uint8_t *dmac)
{
  int i;
  for (i=0; i<6; i++)
    {
    data[6+i] = smac[i] & 0xFF;
    data[i]   = dmac[i] & 0xFF;
    }
  data[12] = 0x08;
  data[13] = 0x00;
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
void format_cisco_arp_req(uint8_t *data, uint8_t *smac, uint8_t *dmac,
                          uint32_t sip, uint32_t dip)
{
  int i;

  utils_fill_mac_header(data, smac, dmac);

  data[12] = 0x08;
  data[13] = 0x06;
  data[14] = 0x00;
  data[15] = 0x01;
  data[16] = 0x08;
  data[17] = 0x00;
  data[18] = 0x06;
  data[19] = 0x04;
  data[20] = 0x00;
  data[21] = 0x01;

  for (i=0; i<6; i++)
    data[22+i] = smac[i];

  data[28] = (sip >> 24) & 0xFF;
  data[29] = (sip >> 16) & 0xFF;
  data[30] = (sip >> 8)  & 0xFF;
  data[31] = (sip & 0xFF);

  for (i=0; i<6; i++)
    data[32+i] = 0;

  data[38] = (dip >> 24) & 0xFF;
  data[39] = (dip >> 16) & 0xFF;
  data[40] = (dip >> 8)  & 0xFF;
  data[41] = (dip & 0xFF);
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
static void utils_fill_ip_header(int ip_proto, int len, uint8_t *data,
                                 uint32_t sip, uint32_t dip)
{
  unsigned short checksum;
  static unsigned short identification = 0;
  identification += 1;
  data[0] = 0x45;
  data[1] = 0;
  data[2] = (len >> 8) & 0xFF;
  data[3] = len & 0xFF;
  data[4] =  (identification >> 8) & 0xFF;
  data[5] =  identification & 0xFF;
  data[6] = 0x40;
  data[7] = 0x00;
  data[8] = 0x40;
  data[9] = ip_proto & 0xFF;
  data[10] = 0x00;
  data[11] = 0x00;
  data[12] = (sip >> 24) & 0xFF;
  data[13] = (sip >> 16) & 0xFF;
  data[14] = (sip >> 8)  & 0xFF;
  data[15] = sip & 0xFF;
  data[16] = (dip >> 24) & 0xFF;
  data[17] = (dip >> 16) & 0xFF;
  data[18] = (dip >> 8)  & 0xFF;
  data[19] =  dip & 0xFF;
  checksum = utils_in_cksum((u_short *)data, 20);
  data[10] = checksum & 0xFF;
  data[11] = (checksum >> 8) & 0xFF;
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
static void utils_fill_udp_ip_header(int len, uint8_t *data,
                                     uint16_t sport, uint16_t dport)
{
  data[0] = (sport >> 8) & 0xFF;
  data[1] = sport & 0xFF;
  data[2] = (dport >> 8) & 0xFF;
  data[3] = dport & 0xFF;
  data[4] = (len >> 8) & 0xFF;
  data[5] = len & 0xFF;
  data[6] = 0x00;
  data[7] = 0x00;
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
static uint16_t tcp_sum_calc(int len, uint8_t *buff, uint8_t *pseudo_header)
{
  int i, sum = 0;
  for (i=0; i<len; i=i+2)
    {
    if (i+1 == len)
      {
      sum += ((buff[i] << 8) & 0xFF00);
      break;
      }
    else
      sum += ((buff[i] << 8) & 0xFF00) + (buff[i+1] & 0xFF);
    }
  for (i=0; i<12; i=i+2)
    sum += ((pseudo_header[i] << 8) & 0xFF00) + (pseudo_header[i+1] & 0xFF);
  sum = (sum & 0xFFFF) + (sum >> 16);
  sum = (sum & 0xFFFF) + (sum >> 16);
  sum = ~sum;
  return ((uint16_t) sum);
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
static uint16_t tcp_checksum(int len, uint8_t *data, uint8_t *iph)
{
  int i;
  uint16_t chksum;
  uint8_t pseudo_header[12];
  for (i=0; i< 8; i++)
    pseudo_header[i] = iph[12+i];
  pseudo_header[8]  = 0;
  pseudo_header[9]  = IPPROTO_TCP;
  pseudo_header[10] = (len >> 8) & 0xFF;
  pseudo_header[11] = len & 0xFF;
  chksum = tcp_sum_calc(len, data, pseudo_header);
  return chksum;
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
static void utils_fill_tcp_ip_header(uint8_t  *buf_ip,   int data_len,
                                     uint16_t sport,     uint16_t dport,
                                     uint32_t sent_seq,  uint32_t recv_ack,
                                     uint8_t  tcp_flags, uint16_t rx_win)
{
  uint8_t *buf = &(buf_ip[g_ip_len]);
  uint16_t checksum;

  buf[0]  = (sport >> 8) & 0xFF;
  buf[1]  =  sport &  0xFF;
  buf[2]  = (dport >> 8) & 0xFF;
  buf[3]  =  dport &  0xFF;

  buf[4]  = (sent_seq >> 24) & 0xFF;
  buf[5]  = (sent_seq >> 16) & 0xFF;
  buf[6]  = (sent_seq >> 8) & 0xFF;
  buf[7]  =  sent_seq &  0xFF;

  buf[8]  = (recv_ack >> 24) & 0xFF;
  buf[9]  = (recv_ack >> 16) & 0xFF;
  buf[10] = (recv_ack >> 8) & 0xFF;
  buf[11] =  recv_ack &  0xFF;

  buf[12] = (5 << 4) & 0xF0;

  buf[13] = tcp_flags & 0xFF;

  buf[14] = (rx_win >> 8) & 0xFF;
  buf[15] =  rx_win & 0xFF;
  
  buf[16] = 0;
  buf[17] = 0;
  buf[18] = 0;
  buf[19] = 0;
  
  checksum=tcp_checksum(g_tcp_len + data_len, &(buf[0]), buf_ip);
  buf[16] =  (checksum >> 8) & 0xFF;
  buf[17] =  checksum & 0xFF;
}
/*---------------------------------------------------------------------------*/

/****************************************************************************/
uint32_t utils_get_cisco_ip(void)
{
  uint32_t result;
  char *ip = NAT_IP_CISCO;
  if (ip_string_to_int (&result, ip))
    KOUT("%s", ip);
  return (result);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
uint32_t utils_get_dns_ip(void)
{
  uint32_t result;
  char *ip = NAT_IP_DNS;
  if (ip_string_to_int (&result, ip))
    KOUT("%s", ip);
  return (result);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
uint32_t utils_get_gw_ip(void)
{
  uint32_t result;
  char *ip = NAT_IP_GW;
  if (ip_string_to_int (&result, ip))
    KOUT("%s", ip);
  return (result);
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
void utils_get_mac(char *str_mac, uint8_t mc[6])
{
  uint32_t i, mac[6];
  memset(mc, 0, 6);
  if (sscanf(str_mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                       &(mac[0]), &(mac[1]), &(mac[2]),
                       &(mac[3]), &(mac[4]), &(mac[5])) != 6)
    KERR("%s", str_mac);
  else
    {
    for (i=0; i<6; i++)
      mc[i] = mac[i] & 0xFF;
    }
}
/*--------------------------------------------------------------------------*/


/*****************************************************************************/
void utils_fill_udp_packet(uint8_t *buf,    uint32_t data_len,
                           uint8_t *smac,   uint8_t *dmac, 
                           uint32_t sip,    uint32_t dip,
                           uint16_t sport,  uint16_t dport)
{
  uint32_t fcs;
  uint32_t len;
  utils_fill_mac_header(buf, smac, dmac);
  len = g_ip_len + g_udp_len + data_len;
  utils_fill_ip_header(IPPROTO_UDP, len, &(buf[g_mac_len]), sip, dip);
  len = g_udp_len + data_len;
  utils_fill_udp_ip_header(len, &(buf[g_mac_len + g_ip_len]), sport, dport);
  len = g_mac_len + g_ip_len + g_udp_len + data_len;
  fcs = utils_frame_cksum(len, buf);
  buf[len + 3] = (fcs & 0xFF000000) >> 24;
  buf[len + 2] = (fcs & 0xFF0000) >> 16;
  buf[len + 1] = (fcs & 0xFF00) >> 8;
  buf[len + 0] = (fcs & 0xFF);
}
/*---------------------------------------------------------------------------*/

/*****************************************************************************/
void utils_fill_tcp_packet(uint8_t *buf, uint32_t data_len,
                           uint8_t *smac,      uint8_t *dmac,
                           uint32_t sip,       uint32_t dip,
                           uint16_t sport,     uint16_t dport,
                           uint32_t sent_seq,  uint32_t recv_ack,
                           uint8_t  tcp_flags, uint16_t rx_win)
{
  uint32_t fcs;
  uint32_t len;
  utils_fill_mac_header(buf, smac, dmac);
  len = g_ip_len + g_tcp_len + data_len;
  utils_fill_ip_header(IPPROTO_TCP, len, &(buf[g_mac_len]), sip, dip);
  utils_fill_tcp_ip_header(&(buf[g_mac_len]), data_len,
                             sport, dport, sent_seq, recv_ack,
                             tcp_flags, rx_win);
  len = g_mac_len + g_ip_len + g_tcp_len + data_len;
  fcs = utils_frame_cksum(len, buf);
  buf[len + 3] = (fcs & 0xFF000000) >> 24;
  buf[len + 2] = (fcs & 0xFF0000) >> 16;
  buf[len + 1] = (fcs & 0xFF00) >> 8;
  buf[len + 0] = (fcs & 0xFF);
}
/*---------------------------------------------------------------------------*/

/****************************************************************************/
struct rte_mbuf *utils_alloc_and_copy_pktmbuf(int len, uint8_t *resp)
{
  struct rte_mbuf *m = NULL;
  char *data;
  m = rte_pktmbuf_alloc(get_rte_mempool());
  if (m == NULL)
    KOUT(" ");
  data = rte_pktmbuf_append(m, len);
  if (data == NULL)
    KOUT(" ");
  if (rte_pktmbuf_pkt_len(m) != len)
    KOUT("%d %d", rte_pktmbuf_pkt_len(m), len);
  memcpy(data, resp, len);
  return (m);
}
/*--------------------------------------------------------------------------*/

/****************************************************************************/
struct rte_mbuf *utils_alloc_pktmbuf(int len)
{
  struct rte_mbuf *m = NULL;
  char *data;
  m = rte_pktmbuf_alloc(get_rte_mempool());
  if (m == NULL)
    KOUT(" ");
  data = rte_pktmbuf_append(m, len);
  if (data == NULL)
    KOUT("%d", len);
  if (rte_pktmbuf_pkt_len(m) != len)
    KOUT("%d %d", rte_pktmbuf_pkt_len(m), len);
  return (m);
}
/*--------------------------------------------------------------------------*/

/*****************************************************************************/
void utils_init(void)
{
  g_mac_len = sizeof(struct rte_ether_hdr);
  g_ip_len  = sizeof(struct rte_ipv4_hdr);
  g_udp_len = sizeof(struct rte_udp_hdr);
  g_tcp_len = sizeof(struct rte_tcp_hdr);
}
/*---------------------------------------------------------------------------*/

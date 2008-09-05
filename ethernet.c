/* ethernet.c */
/* 
 * Routines for taking ethernet packets apart.
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the MPEG TS, PS and ES tools.
 *
 * The Initial Developer of the Original Code is Amino Communications Ltd.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Richard Watts, Kynesim <rrw@kynesim.co.uk>
 *
 * ***** END LICENSE BLOCK *****
 */

#include "ethernet.h"
#include <string.h>
#include "misc_fns.h"

int ethernet_packet_from_pcap(pcaprec_hdr_t *hdr,
			      const uint8_t *data,
			      const uint32_t len,
			      ethernet_packet_t *pkt,
			      uint32_t *out_st,
			      uint32_t *out_len)
{
  // 14 bytes of src,dest,type .. 
  if (len < 14)
    {
      return ETHERNET_ERR_PKT_TOO_SHORT;
    }

  // PCap doesn't store CRCs - it stores [dst] [src] [type]
  memcpy(pkt->dst_addr, &data[0], 6);
  memcpy(pkt->src_addr, &data[6], 6);

  // Type/Length is big-endian.
  pkt->typeorlen = uint_16_be(&data[12]);

  // 0x5DC is the maximum frame length in IEEE 802.3 - anything
  // above that here is a type.
  // 
  // Length is just the data length.
  if (pkt->typeorlen <= 0x5DC)
    {
      (*out_len) = pkt->typeorlen;
    }
  else
    {
      // pcap doesn't store the checksum or pad .. 
      (*out_len) = len - 14;
    }
	
  (*out_st) = 14;

  return 0;
}

/* End file */

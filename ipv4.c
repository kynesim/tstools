/* ipv4.c */
/* 
 * Routines for dissecting ipv4 
 *
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

#include <stdio.h>
#include <stdlib.h>

#include "ipv4.h"
#include "misc_fns.h"
#include <string.h>

int ipv4_from_payload(const uint8_t *data,
		      const uint32_t len,
		      ipv4_header_t *out_hdr,
		      uint32_t *out_st,
		      uint32_t *out_len)
{
  uint32_t cur_field;

  // Min length of an ipv4 header is 20 bytes (5 words)
  if (len < 20)
    {
      return IPV4_ERR_PKT_TOO_SHORT;
    }

  // Field 0
  cur_field = uint_32_be(&data[0]);

  out_hdr->version = (cur_field >> 28)&0xf;
  out_hdr->hdr_length = (cur_field >> 24)& 0xf;
  out_hdr->serv_type = (cur_field >> 16) & 0xff;
  out_hdr->length = (cur_field) & 0xffff;

  // Field 1
  cur_field = uint_32_be(&data[4]);
  
  out_hdr->ident = (cur_field >> 16) & 0xffff;
  out_hdr->flags = (cur_field >> 13) & 7;
  out_hdr->frag_offset = (cur_field & 0x1fff);
  
  // Field 2
  cur_field = uint_32_be(&data[8]);
  
  out_hdr->ttl = (cur_field >> 24) & 0xff;
  out_hdr->proto = (cur_field >> 16) & 0xff;
  out_hdr->csum = (cur_field) & 0xffff;
  
  // Field 3 - src address.
  out_hdr->src_addr = uint_32_be(&data[12]);
  // Field 4 - dest address.
  out_hdr->dest_addr = uint_32_be(&data[16]);
  
  // Now the data ..
  (*out_st) = (out_hdr->hdr_length << 2);
  (*out_len) = len - (out_hdr->hdr_length << 2);
  
  return 0;
}


int ipv4_udp_from_payload(const uint8_t *data,
			  const uint32_t len,
			  ipv4_udp_header_t *out_hdr,
			  uint32_t *out_st,
			  uint32_t *out_len)
{
  uint32_t cur_field;

  // UDP headers are 8 bytes long.
  if (len < 8)
    {
      return IPV4_ERR_PKT_TOO_SHORT;
    }

  cur_field = uint_32_be(&data[0]);
  out_hdr->source_port = (cur_field >> 16)&0xffff;
  out_hdr->dest_port = (cur_field) & 0xffff;
  
  cur_field = uint_32_be(&data[4]);
  out_hdr->length = (cur_field >> 16) & 0xffff;
  out_hdr->csum = (cur_field) &0xffff;
  
  (*out_st) = 8;
  (*out_len) = len - 8;

  return 0;
}

/* End file */


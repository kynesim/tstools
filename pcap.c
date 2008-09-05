/* pcap.c */
/*
 * Read pcap files.
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


/* Both of these return 1 on success, 0 on EOF,  <0 on error */

#include "pcap.h"
#include "misc_fns.h"

static int pcap_read_header(PCAP_reader_p ctx, pcap_hdr_t *out_hdr);
static int pcap_read_pktheader(PCAP_reader_p ctx, pcaprec_hdr_t *out_hdr);


int pcap_open(PCAP_reader_p *ctx_p, pcap_hdr_t *out_hdr, 
	      const char *filename)
{
  FILE *fptr = (filename ? fopen(filename, "rb") : stdin);
  PCAP_reader_p ctx;
  int rv;

  (*ctx_p) = NULL;

  if (!fptr)
    {
      // Couldn't open the file.
      return -1;
    }
  ctx = (PCAP_reader_p)malloc(SIZEOF_PCAP_READER);
  if (!ctx) 
    {
      fclose(fptr);
      // Out of memory.
      return -2;
    }

  ctx->file = fptr;
  
  rv = pcap_read_header(ctx, out_hdr);
  
  if (rv != 1)
    {
      // Header read failed.
      fclose(ctx->file);
      free(ctx);
      return -4;
    }

  (*ctx_p) = ctx;
  
  return 0;
}

int pcap_read_next(PCAP_reader_p ctx, pcaprec_hdr_t *out_hdr, 
		   uint8_t **out_data, 
		   uint32_t *out_len)
{
  int rv;

  (*out_data) = NULL; (*out_len) = 0;

  rv = pcap_read_pktheader(ctx, out_hdr);
  if (rv != 1) { return rv; }

  // Otherwise we now know how long our packet is .. 
  (*out_data) = (uint8_t *)malloc(out_hdr->incl_len);
  
  if (!(*out_data)) 
    {
      // Out of memory.
      return -3; 
    }

  (*out_len) = out_hdr->incl_len;
  
  rv = fread((*out_data), (*out_len), 1, ctx->file);
  if (rv != 1)
    {
      free(*out_data); (*out_data) = NULL;
      *out_len = 0;

      if (feof(ctx->file))
	{
	  // Ah. EOF.
	  return 0;
	}
      else 
	{
	  // Error. Curses.
	  return rv;
	}
    }
  else if (rv == 1)
    {
      // Gotcha.
      return 1;
    }

  return 0;
}


static int pcap_read_header(PCAP_reader_p ctx, pcap_hdr_t *hdr)
{
  uint8_t hdr_val[SIZEOF_PCAP_HDR_ON_DISC];
  int rv;
  
  rv = fread(&hdr_val[0], SIZEOF_PCAP_HDR_ON_DISC, 1, ctx->file);
  if (rv != 1)
    {
      if (feof(ctx->file)) 
	{
	  return 0;
	}
      else
	{
	  return PCAP_ERR_FILE_READ;
	}
    }


  /* The magic number is 0xa1b2c3d4. If the writing
   * machine was BE, the first byte will be a1 else d4
   */
  if (hdr_val[0] == 0xa1)
    {
      // Big endian.
      ctx->is_be = 1;
    }
  else if (hdr_val[0] == 0xd4)
    {
      // Little endian.
      ctx->is_be = 0;
    }
  else
    {
      return PCAP_ERR_INVALID_MAGIC;
    }

  hdr->magic_number = (ctx->is_be ? uint_32_be(&hdr_val[0]) :
		       uint_32_le(&hdr_val[0]));
  if (hdr->magic_number != 0xa1b2c3d4)
    {
      return PCAP_ERR_INVALID_MAGIC;
    }

  hdr->version_major = (ctx->is_be ? uint_16_be(&hdr_val[4]) : 
			uint_16_le(&hdr_val[4]));
  hdr->version_minor = (ctx->is_be ? uint_16_be(&hdr_val[6]) : 
			uint_16_le(&hdr_val[6]));
  hdr->thiszone = (int32_t)(ctx->is_be ? uint_32_be(&hdr_val[8]) : 
			    uint_32_le(&hdr_val[8]));
  hdr->sigfigs = (ctx->is_be ? uint_32_be(&hdr_val[12]) : 
			uint_32_le(&hdr_val[12]));
  hdr->snaplen = (ctx->is_be ? uint_32_be(&hdr_val[16]) : 
		  uint_32_le(&hdr_val[16]));
  hdr->network = (ctx->is_be ? uint_32_be(&hdr_val[20]) : 
		  uint_32_le(&hdr_val[20]));


  return 1;
}

static int pcap_read_pktheader(PCAP_reader_p ctx, pcaprec_hdr_t *hdr)
{
  uint8_t hdr_val[SIZEOF_PCAPREC_HDR_ON_DISC];
  int rv;

  rv = fread(&hdr_val[0], SIZEOF_PCAPREC_HDR_ON_DISC, 1, ctx->file);
  if (rv != 1)
    {
      if (feof(ctx->file))
	{
	  return 0;
	}
      else
	{
	  return PCAP_ERR_FILE_READ;
	}
    }

  hdr->ts_sec = (ctx->is_be ? uint_32_be(&hdr_val[0]) : 
		 uint_32_le(&hdr_val[0]));
  hdr->ts_usec = (ctx->is_be ? uint_32_be(&hdr_val[4]) : 
		  uint_32_le(&hdr_val[4]));
  hdr->incl_len = (ctx->is_be ? uint_32_be(&hdr_val[8]) : 
		   uint_32_le(&hdr_val[8]));
  hdr->orig_len = (ctx->is_be ? uint_32_be(&hdr_val[12]) : 
		   uint_32_le(&hdr_val[12]));

  return 1;
}

/* End file */


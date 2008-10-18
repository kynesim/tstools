/* 
 * Report on a pcap (.pcap) file.
 *
 * <rrw@kynesim.co.uk> 2008-09-05
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
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif // _WIN32

#include "compat.h"
#include "pcap.h"
#include "ethernet.h"
#include "ipv4.h"
#include "version.h"
#include "misc_fns.h"
#include "ts_fns.h"

typedef struct pcapreport_ctx_struct
{
  int use_stdin;
  char *input_name;
  int had_input_name;
  int dump_data;
  int dump_extra;
  int time_report;
  int verbose;
  PCAP_reader_p pcreader;
  pcap_hdr_t pcap_hdr;
  char *output_name;
  FILE *output_file;
  uint32_t output_dest_addr;
  uint32_t output_dest_port;

  TS_reader_p ts_r;

  // The temporary read buffer used by our ts reader.
  byte    *tmp_buf;
  uint32_t tmp_len;

  // ts packet counter for error reporting.
  uint32_t ts_counter;

  // packet counter.
  uint32_t pkt_counter;

  // Last continuity counter.
  int last_cc;

  // Do we think we currently have cc?
  int have_cc;

  /*! In 90kHz units, what do you reckon you need to add to 
   *  PCR to make it into packet time?
   */
  int64_t pcr_time_offset;

  /*! What was the last difference between pcr_time_offset and
   *   the measured time offset?
   */
  int64_t last_time_offset;

  /*! Do we think pcr_time_offset is valid at the minute? */
  int pcr_time_offset_valid;

  /*! How far do we need to skew (in 90kHz units) to signal a discontinuity? */
  int64_t skew_discontinuity_threshold;

  /*! Time of last discontinuity, in us */
  int64_t last_d_us;

} pcapreport_ctx_t;

// Discontinuity threshold is 6s.
#define SKEW_DISCONTINUITY_THRESHOLD (6*90000)

static int digest_times_read(void *handle, byte *out_buf, size_t len)
{
  pcapreport_ctx_t *ctx = (pcapreport_ctx_t *)handle;
  int nr_bytes = (len < ctx->tmp_len ? len : ctx->tmp_len);
  int new_tmp_len = ctx->tmp_len - nr_bytes;

  memcpy(out_buf, ctx->tmp_buf, nr_bytes);
  memmove(ctx->tmp_buf, &ctx->tmp_buf[nr_bytes], 
          new_tmp_len);
  ctx->tmp_len = new_tmp_len;
  //   printf(">> read %d bytes from intermediate buffer. \n", nr_bytes);

  return nr_bytes;
}


static int digest_times_seek(void *handle, offset_t val)
{
  // Cannot seek in a ts stream.
  return 1;
}

static int digest_times(pcapreport_ctx_t *ctx, 
                        pcaprec_hdr_t *pcap_pkt_hdr,
                        ethernet_packet_t *epkt,
                        ipv4_header_t *ipv4_header, 
                        ipv4_udp_header_t *udp_header,
                        const byte *data,
                        const uint32_t len)
{
  int rv;

  if (!ctx->ts_r)
  {
    rv = build_TS_reader_with_fns(ctx,
                                  digest_times_read, 
                                  digest_times_seek, 
                                  &ctx->ts_r);
    if (rv)
    {
      fprintf(stderr, "### pcapreport: Cannot create ts reader.\n");
      return 1;
    }
  }

  // Add all our data to the pool.

  ctx->tmp_buf = (byte *)realloc(ctx->tmp_buf, ctx->tmp_len + len);
  memcpy(&ctx->tmp_buf[ctx->tmp_len], data, len);
  ctx->tmp_len += len;

  // Now read out all the ts packets we can.
  while (1)
  {
    byte *pkt;
    int rv;

    rv = read_next_TS_packet(ctx->ts_r, &pkt);
    if (rv == EOF)
    {
      // Got to EOF - return for more data
      return 0;
    }


    // Right. Split it ..
    {
      uint32_t pid;
      int pusi;
      byte *adapt;
      int adapt_len;
      byte *payload;
      int payload_len;

      rv = split_TS_packet(pkt, &pid, &pusi, &adapt, &adapt_len,
                           &payload, &payload_len);
      if (rv)
      {
        printf(">> WARNING: TS packet %d [ packet %d @ %d.%d s ] cannot be split.\n",
               ctx->ts_counter, ctx->pkt_counter, 
               pcap_pkt_hdr->ts_sec, pcap_pkt_hdr->ts_usec);
      }
      else
      {
        //int cc;

        // PCR ?
        if (adapt && adapt_len)
        {
          int has_pcr;
          uint64_t pcr;
          uint64_t t_pcr;
          int64_t pcr_time_offset;

          get_PCR_from_adaptation_field(adapt, adapt_len, &has_pcr,
                                        &pcr);
          if (has_pcr)
          {
            printf(">> Found PCR %lld at %d.%d s \n", 
                   pcr, pcap_pkt_hdr->ts_sec, pcap_pkt_hdr->ts_usec);

            // PCR pops out in 27MHz units. Let's do all our comparisons
            // in 90kHz.
            pcr = pcr / 300;
            t_pcr = (((int64_t)pcap_pkt_hdr->ts_usec*9)/100) + 
              ((int64_t)pcap_pkt_hdr->ts_sec * 90000);

            // printf("pcr = %lld t_pcr = %lld diff = %lld\n", 
            // pcr, t_pcr, 
            // t_pcr - pcr);

            pcr_time_offset = ((int64_t)t_pcr - (int64_t)pcr);
            if (ctx->pcr_time_offset_valid)
            {
              int64_t skew = (pcr_time_offset - ctx->pcr_time_offset);

              if (skew > ctx->skew_discontinuity_threshold || 
                  skew < -ctx->skew_discontinuity_threshold)
              {
                printf(">> Skew discontinuity! Skew = %lld (> %lld) at"
                       " ts = %d network = %d (PCR %lld Time %d.%d)\n", 
                       skew, ctx->skew_discontinuity_threshold, 
                       ctx->ts_counter, ctx->pkt_counter,
                       pcr, pcap_pkt_hdr->ts_sec,
                       pcap_pkt_hdr->ts_usec);

                ctx->pcr_time_offset = pcr_time_offset;
              }
              else
              {
                int64_t rel_tim;
                double skew_rate;

                rel_tim = ((int64_t)pcap_pkt_hdr->ts_usec) + 
                  ((int64_t)pcap_pkt_hdr->ts_sec * 1000000);

                rel_tim -= ctx->last_d_us;

                skew_rate = (double)skew / ((double)((double)rel_tim / (60*1000000)));

                printf(">> [ts %d net %d ] PCR %lld Time %d.%d [rel %d.%d]  - skew = %lld (delta = %lld, rate = %.4g PTS/min)\n",
                       ctx->ts_counter, ctx->pkt_counter,
                       pcr, 
                       pcap_pkt_hdr->ts_sec, pcap_pkt_hdr->ts_usec,
                       (int)(rel_tim / (int64_t)1000000), 
                       (int)rel_tim%1000000,
                       skew, pcr_time_offset - ctx->last_time_offset, 
                       skew_rate);
              }
              ctx->last_time_offset = pcr_time_offset;
            }
            else
            {
              ctx->pcr_time_offset = ctx->last_time_offset = 
                pcr_time_offset;
              ctx->last_d_us = 
                ((int64_t)pcap_pkt_hdr->ts_usec) + 
                (((int64_t)pcap_pkt_hdr->ts_sec) * 1000000);

              ctx->pcr_time_offset_valid = 1;
            }
          }
        }


#if 0
        // CC?
        cc = pkt[3]&0x0f;
        //printf("cc = %d \n", cc);
        if (!ctx->have_cc)
        {
          ctx->have_cc = 1; 
        }
        else
        {
          if (cc != ((ctx->last_cc + 1)&0xf))
          {
            printf(">> CC discontinuity! ts = %d network = %d expected %d got %d.\n",
                   ctx->ts_counter, ctx->pkt_counter, 
                   (ctx->last_cc+1)&0xf,
                   cc);
          }
        }
        ctx->last_cc = cc;
#endif
      }


      ++ctx->ts_counter;
    }
  }
}

static int write_out_packet(pcapreport_ctx_t *ctx, 
                            const byte *data, 
                            const uint32_t len)
{
  int rv;

  if (ctx->output_name)
  {
    if (!ctx->output_file)
    {
      ctx->output_file = fopen(ctx->output_name, "wb");
      if (!ctx->output_file)
      {
        fprintf(stderr,"### pcapreport: Cannot open %s .\n", 
                ctx->output_name);
        return 1;
      }
    }

    if (ctx->verbose)
    {
      printf("++   Dumping %d bytes to output file.\n", len);
    }
    rv = fwrite(data, len, 1, ctx->output_file);
    if (rv != 1)
    {
      fprintf(stderr, "### pcapreport: Couldn't write %d bytes"
              " to %s (error = %d).\n", 
              len, ctx->output_name, 
              ferror(ctx->output_file));
      return 1;
    }
  }
  return 0;
}


static void print_usage()
{
  printf("Usage: pcapreport [switches] [<infile>] [switches]\n"
         "\n"
        );
  REPORT_VERSION("pcap");
  printf(
    "\n"
    " Report on a pcap capture file.\n"
    "\n"
    " -o <output file>         Dump selected UDP payloads to the named output file.\n"
    " -d <dest ip>:<port>      Select data with the given destination IP and port.\n"
    " --dump-data | -D         Dump any data in the input file to stdout.\n"
    " --extra-dump | -e        Dump only data which isn't being sent to the -o file.\n"
    " --times | -t             Report on PCR vs PCAP timing for the destination specified in -d.\n"
    " --verbose | -v           Output metadata about every packet.\n"
    " --skew-discontinuity-threshold <number>\n"
    "                          Gives the skew discontinuity threshold in 90kHz units.\n"
    "\n"
    " Specifying 0.0.0.0 for destination IP or 0 for destination port will capture all\n"
    " hosts and ports respectively.\n"
    "\n"
    " Network packet and TS packet numbers start at 0.\n"
    "\n"
    " Positive skew means that we received too low a PCR for this timestamp.\n"
    "\n"
    );
}


int main(int argc, char **argv)
{
  int err = 0;
  int ii = 1;
  pcapreport_ctx_t ctx;

  memset(&ctx, '\0', sizeof(pcapreport_ctx_t));

  ctx.skew_discontinuity_threshold = SKEW_DISCONTINUITY_THRESHOLD;

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help", argv[ii]) || !strcmp("-h", argv[ii]) ||
          !strcmp("-help", argv[ii]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("--o", argv[ii]) || !strcmp("-o", argv[ii]))
      {
        ++ii;
        if (ii < argc)
        {
          ctx.output_name = argv[ii];
        }
        else
        {
          fprintf(stderr, "### pcapreport: -o requires an argument.\n");
          return 1;
        }
      }
      else if (!strcmp("-times", argv[ii]) || 
               !strcmp("--times", argv[ii]) || !strcmp("-t", argv[ii]))
      {
        ++ctx.time_report;
      }
      else if (!strcmp("-verbose", argv[ii]) || 
               !strcmp("--verbose", argv[ii]) || !strcmp("-v", argv[ii]))
      {
        ++ctx.verbose;
      }
      else if (!strcmp("--d", argv[ii]) || !strcmp("-d", argv[ii]))
      {
        char *hostname;
        int port;

        err = host_value("pcapreport", argv[ii], argv[ii+1], &hostname, &port);
        if (err) return 1;
        ++ii;

        ctx.output_dest_port = port;
        if (ipv4_string_to_addr(&ctx.output_dest_addr, hostname))
        {
          fprintf(stderr, "### pcapreport: '%s' is not a host IP address (names are not allowed!)\n",
                  hostname);
          return 1;
        }
      }
      else if (!strcmp("--dump-data", argv[ii]) || !strcmp("-D", argv[ii]))
      {
        ++ctx.dump_data;
      }
      else if (!strcmp("--extra-dump", argv[ii]) || !strcmp("-E", argv[ii]))
      {
        ++ctx.dump_extra;
      }
      else if (!strcmp("--skew-discontinuity-threshold", argv[ii]))
      {
        int val;
        int rv = 
          int_value("pcapreport", argv[ii], argv[ii+1], TRUE, 0,
                    &val); 
        ctx.skew_discontinuity_threshold = val;
        if (rv) 
        {
          return 1;
        }
      }
      else
      {
        fprintf(stderr, "### pcapreport: "
                "Unrecognised command line switch '%s'\n", argv[ii]);
        return 1;
      }
    }
    else
    {
      if (ctx.had_input_name)
      {
        fprintf(stderr, "### pcapreport: Unexpected '%s'\n", 
                argv[ii]);
        return 1;
      }
      else
      {
        ctx.input_name = argv[ii];
        ctx.had_input_name = TRUE;
      }
    }
    ++ii;
  }

  err = pcap_open(&ctx.pcreader, &ctx.pcap_hdr, ctx.input_name);
  if (err)
  {
    fprintf(stderr, 
            "### pcapreport: Unable to open input file %s for reading "
            "PCAP (code %d)\n", 
            ctx.had_input_name ? "<stdin>": ctx.input_name, err);
  }

  if (ctx.output_name)
  {
    printf("pcapreport: Dumping all packets for %s:%d to %s .\n",
           ipv4_addr_to_string(ctx.output_dest_addr),
           ctx.output_dest_port,
           ctx.output_name);
  }

  printf("Capture made by version %u.%u local_tz_correction "
         "%d sigfigs %u snaplen %d network %u\n", 
         ctx.pcap_hdr.version_major, ctx.pcap_hdr.version_minor,
         ctx.pcap_hdr.thiszone,
         ctx.pcap_hdr.sigfigs,
         ctx.pcap_hdr.snaplen,
         ctx.pcap_hdr.network);

  if (ctx.pcap_hdr.snaplen < 65535)
  {
    fprintf(stderr,"### pcapreport: WARNING snaplen is %d, not >= 65535 - "
            "not all data may have been captured.\n", 
            ctx.pcap_hdr.snaplen);
  }


  {
    int done = 0;

    while (!done)
    {
      pcaprec_hdr_t rec_hdr;
      byte *data = NULL;
      uint32_t len = 0;
      int sent_to_output = 0;

      err = pcap_read_next(ctx.pcreader, &rec_hdr, &data, &len);

      switch (err)
      {
      case 0: // EOF.
        ++done;
        break;
      case 1: // Got a packet.
        {
          byte *allocated = data;

          if (ctx.verbose)
          {
            printf("pkt: Time = %d.%d orig_len = %d \n", 
                   rec_hdr.ts_sec, rec_hdr.ts_usec, 
                   rec_hdr.orig_len);
          }

          if (!(ctx.pcap_hdr.network == PCAP_NETWORK_TYPE_ETHERNET))
          {
            goto dump_out;
          }

          {
            ethernet_packet_t epkt;
            uint32_t out_st, out_len;
            int rv;
            ipv4_header_t ipv4_hdr;
            ipv4_udp_header_t udp_hdr;

            rv = ethernet_packet_from_pcap(&rec_hdr, 
                                           data, len, 
                                           &epkt,
                                           &out_st,
                                           &out_len);

            if (rv)
            {
              goto dump_out;
            }


            if (ctx.verbose)
            {
              printf("++ 802.11: src %02x:%02x:%02x:%02x:%02x:%02x "
                     " dst %02x:%02x:%02x:%02x:%02x:%02x "
                     "typeorlen 0x%04x\n",
                     epkt.src_addr[0], epkt.src_addr[1], 
                     epkt.src_addr[2], epkt.src_addr[3], 
                     epkt.src_addr[4], epkt.src_addr[5],
                     epkt.dst_addr[0], epkt.dst_addr[1], 
                     epkt.dst_addr[2], epkt.dst_addr[3],
                     epkt.dst_addr[4], epkt.dst_addr[5], 
                     epkt.typeorlen);
            }

            data = &data[out_st];
            len = out_len;

            // Is it IP?
            if (epkt.typeorlen != 0x800)
            {
              goto dump_out;
            }


            rv = ipv4_from_payload(data, len, 
                                   &ipv4_hdr, 
                                   &out_st, 
                                   &out_len);
            if (rv)
            {
              goto dump_out;
            }

            if (ctx.verbose)
            {		    
              printf("++ IPv4: src = %s", 
                     ipv4_addr_to_string(ipv4_hdr.src_addr));
              printf(" dest = %s \n", 
                     ipv4_addr_to_string(ipv4_hdr.dest_addr));

              printf(
                "++ IPv4: version = 0x%x hdr_length = 0x%x"
                " serv_type = 0x%08x length = 0x%04x\n"
                "++ IPv4: ident = 0x%04x flags = 0x%02x"
                " frag_offset = 0x%04x ttl = %d\n"
                "++ IPv4: proto = %d csum = 0x%04x\n",
                ipv4_hdr.version,
                ipv4_hdr.hdr_length,
                ipv4_hdr.serv_type,
                ipv4_hdr.length,
                ipv4_hdr.ident,
                ipv4_hdr.flags,
                ipv4_hdr.frag_offset,
                ipv4_hdr.ttl,
                ipv4_hdr.proto,
                ipv4_hdr.csum);
            }

            data = &data[out_st];
            len = out_len;

            if (!(IPV4_HDR_IS_UDP(&ipv4_hdr)))
            {
              goto dump_out;
            }


            rv = ipv4_udp_from_payload(data, len,
                                       &udp_hdr,
                                       &out_st,
                                       &out_len);
            if (rv) 
            {
              goto dump_out;
            }

            if (ctx.verbose)
            {
              printf("++ udp: src port = %d "
                     "dest port = %d len = %d \n",
                     udp_hdr.source_port,
                     udp_hdr.dest_port,
                     udp_hdr.length);
            }

            data = &data[out_st];
            len = out_len;

            if (
              (!ctx.output_dest_addr || (ipv4_hdr.dest_addr == ctx.output_dest_addr)) && 
              (!ctx.output_dest_port || (udp_hdr.dest_port == ctx.output_dest_port)))
            {
              ++sent_to_output;

              if (ctx.time_report)
              {
                rv =digest_times(&ctx, 
                                 &rec_hdr,
                                 &epkt,
                                 &ipv4_hdr,
                                 &udp_hdr, 
                                 data, len);
                if (rv) { return rv; }
              }
              if (ctx.output_name)
              {
                rv = write_out_packet(&ctx, data, len);
                if (rv) { return rv; }
              }


            }
          }

          // Adjust 
dump_out:
          if (ctx.dump_data || (ctx.dump_extra && !sent_to_output))
          {
            print_data(stdout, "data", 
                       data, len, len);
          }
          free(allocated); allocated = data = NULL;
        }
        break;
      default:
        // Some other error.
        fprintf(stderr, "### pcapreport: Can't read packet %d - code %d\n",
                ctx.pkt_counter, err);
        ++done;
        break;
      }
      ++ctx.pkt_counter;

    }
  }

  if (ctx.output_file) 
  {
    printf("Closing output file.\n");
    fclose(ctx.output_file); 
  }

  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

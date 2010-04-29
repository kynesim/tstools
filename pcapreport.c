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
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
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
#include "fmtx.h"

typedef struct pcapreport_stream_struct pcapreport_stream_t;

#define JITTER_BUF_SIZE 1024

typedef struct jitter_el_struct {
  uint32_t t;
  int delta;
} jitter_el_t;

typedef struct jitter_env_struct {
  int min_val;
  int max_val;
  int in_n;
  int out_n;
  int len;
  jitter_el_t buf[JITTER_BUF_SIZE];
} jitter_env_t;

typedef struct pcapreport_section_struct pcapreport_section_t;
struct pcapreport_section_struct {
  pcapreport_section_t * next;
  unsigned int section_no;
  unsigned int jitter_max;
  uint32_t pkt_start;
  uint32_t pkt_final;
  uint64_t time_start;  // 90kHz
  uint64_t time_last;   // time @ last PCR
  uint64_t time_final;
  uint64_t pcr_start;   // 90kHz
  uint64_t pcr_last;
  uint64_t ts_byte_start;
  uint64_t ts_byte_final;
};

struct pcapreport_stream_struct {
  pcapreport_stream_t * hash_next;

  char *output_name;
  FILE *output_file;
  uint32_t output_dest_addr;
  uint32_t output_dest_port;

  FILE * csv_file;
  const char * csv_name;

  int stream_no;
  int force;  // We have an explicit filter - try harder
  int ts_good;  // Not a boolean -ve is bad, +ve is good
  int seen_good;  // Includes those seen_dodgy
  int seen_bad;
  int seen_dodgy;  // Count of packets that we aren't completely happy with but have declared good
  int multiple_pcr_pids;

  TS_reader_p ts_r;

  uint32_t pcr_pid;

  // The temporary read buffer used by our ts reader.
  byte    *tmp_buf;
  uint32_t tmp_len;

  // ts packet counter for error reporting.
  uint32_t ts_counter;

  // Count overlength packets
  uint32_t pkts_overlength;

  /*! How far do we need to skew (in 90kHz units) to signal a discontinuity? */
  int64_t skew_discontinuity_threshold;

  int64_t last_time_offset;

  uint64_t ts_bytes;

  pcapreport_section_t * section_first;
  pcapreport_section_t * section_last;

  jitter_env_t jitter;
};


typedef struct pcapreport_fragment_struct
{
  int in_use;
  uint16_t ident;
  uint16_t current_len;
  byte pkt[65536];
} pcapreport_fragment_t;

typedef struct pcapreport_reassembly_struct
{
  pcapreport_fragment_t frag;
} pcapreport_reassembly_t;



typedef struct pcapreport_ctx_struct
{
  int use_stdin;
  char *input_name;
  const char * base_name;
  int had_input_name;
  int extract_data;
  int dump_data;
  int dump_extra;
  int time_report;
  int verbose;
  int analyse;
  int extract;
  int stream_count;
  int csv_gen;
  int good_ts_only;
  PCAP_reader_p pcreader;
  pcap_hdr_t pcap_hdr;

  unsigned int tfmt;

  // packet counter.
  uint32_t pkt_counter;

  uint32_t filter_dest_addr;
  uint32_t filter_dest_port;

  const char * output_name_base;

  int64_t opt_skew_discontinuity_threshold;

  uint64_t time_start; // 90kHz
  uint32_t time_usec;
  time_t time_sec;

  pcapreport_stream_t * stream_hash[256];
  pcapreport_reassembly_t reassembly_env;
} pcapreport_ctx_t;


static unsigned int
jitter_value(const jitter_env_t * const je)
{
  return je->max_val - je->min_val;
}

static unsigned int
jitter_add(jitter_env_t * const je, const int delta, const uint32_t time, const uint32_t range)
{
  jitter_el_t * const eob = je->buf + JITTER_BUF_SIZE;
  jitter_el_t * const in_el = je->buf + je->in_n;
  jitter_el_t * out_el = je->buf + je->out_n;
  jitter_el_t * const next_el = (je->in_n == JITTER_BUF_SIZE - 1) ? je->buf : in_el + 1;
  int needs_scan = FALSE;


  // 1st expire anything we no longer want - in any case expire one if
  // we are about to overflow.
  while (in_el != out_el && (time - in_el->t < range || out_el == next_el))
  {
    if (in_el->delta == je->min_val || in_el->delta == je->max_val)
      needs_scan = TRUE;

    // Inc with wrap
    if (++out_el >= eob)
      out_el = je->buf;
  }

  if (needs_scan || in_el == out_el)
  {
    // Only recalc max & min if we have expired a previous one
    const jitter_el_t * el = out_el;
    int min_val = delta;
    int max_val = delta;

    while (el != in_el)
    {
      if (el->delta > max_val)
        max_val = el->delta;
      if (el->delta < min_val)
        min_val = el->delta;

      if (++el >= eob)
        el = je->buf;
    }

    je->max_val = max_val;
    je->min_val = min_val;
  }
  else
  {
    // If we haven't expired a previous min or max just check to see if this
    // is a new one
    if (delta > je->max_val)
      je->max_val = delta;
    if (delta < je->min_val)
      je->min_val = delta;
  }

  // Now add to the end
  in_el->t = time;
  in_el->delta = delta;

  // and update the environment
  je->in_n = next_el - je->buf;
  je->out_n = out_el - je->buf;

  return jitter_value(je);
}

static void
jitter_clear(jitter_env_t * const je)
{
  je->in_n = 0;
  je->out_n = 0;
  je->max_val = 0;
  je->min_val = 0;
}

static pcapreport_section_t *
section_create(pcapreport_stream_t * const st)
{
  pcapreport_section_t * const tsect = calloc(1, sizeof(*tsect));
  pcapreport_section_t * const last = st->section_last;

  if (tsect == NULL)
    return NULL;

  // Bind into stream

  if (last == NULL)
  {
    // Empty chain - add as first el
    st->section_first = tsect;
  }
  else
  {
    // Add to end
    tsect->section_no = last->section_no + 1;
    last->next = tsect;
  }
  st->section_last = tsect;

  return tsect;
}

// Discontinuity threshold is 6s.
#define SKEW_DISCONTINUITY_THRESHOLD (6*90000)

static int digest_times_read(void *handle, byte *out_buf, size_t len)
{
  pcapreport_stream_t * const st = handle;
  int nr_bytes = (len < st->tmp_len ? len : st->tmp_len);
  int new_tmp_len = st->tmp_len - nr_bytes;

  memcpy(out_buf, st->tmp_buf, nr_bytes);
  memmove(st->tmp_buf, &st->tmp_buf[nr_bytes], 
          new_tmp_len);
  st->tmp_len = new_tmp_len;
  //   fprint_msg(">> read %d bytes from intermediate buffer. \n", nr_bytes);

  return nr_bytes;
}


static int digest_times_seek(void *handle, offset_t val)
{
  // Cannot seek in a ts stream.
  return 1;
}

static uint64_t
pkt_time(const pcaprec_hdr_t * const pcap_pkt_hdr)
{
  return (((int64_t)pcap_pkt_hdr->ts_usec*9)/100) + 
    ((int64_t)pcap_pkt_hdr->ts_sec * 90000);
}


static int digest_times(pcapreport_ctx_t *ctx, 
                        pcapreport_stream_t * const st,
                        const pcaprec_hdr_t *pcap_pkt_hdr,
                        const ethernet_packet_t *epkt,
                        const ipv4_header_t *ipv4_header, 
                        const ipv4_udp_header_t *udp_header,
                        const byte *data,
                        const uint32_t len)
{
  int rv;
  const uint64_t ts_byte_start = st->ts_bytes;

  if (st->ts_r == NULL)
  {
    rv = build_TS_reader_with_fns(st,
                                  digest_times_read, 
                                  digest_times_seek, 
                                  &st->ts_r);
    if (rv)
    {
      print_err( "### pcapreport: Cannot create ts reader.\n");
      return 1;
    }
  }

  // Add all our data to the pool.
  {
    unsigned int pkts = len / 188;
    unsigned int pktlen = pkts * 188;

    if (pktlen != len)
      ++st->pkts_overlength;

    st->tmp_buf = (byte *)realloc(st->tmp_buf, st->tmp_len + pktlen);
    memcpy(&st->tmp_buf[st->tmp_len], data, pktlen);
    st->tmp_len += pktlen;
    st->ts_bytes += pktlen;
  }

  // Now read out all the ts packets we can.
  while (1)
  {
    byte *pkt;
    int rv;

    rv = read_next_TS_packet(st->ts_r, &pkt);
    if (rv == EOF)
    {
      // Got to EOF - return for more data
      return 0;
    }


    // Right. Split it ..
    {
      const uint64_t t_pcr = pkt_time(pcap_pkt_hdr);
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
        fprint_msg(">%d> WARNING: TS packet %d [ packet %d @ %d.%d s ] cannot be split.\n",
                   st->stream_no,
                   st->ts_counter, ctx->pkt_counter, 
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
          int64_t pcr_time_offset;

          get_PCR_from_adaptation_field(adapt, adapt_len, &has_pcr,
                                        &pcr);

          if (has_pcr)
          {
            int64_t skew;

            if (ctx->time_report)
            {
              fprint_msg(">%d> Found PCR %lld at %d.%d s \n", st->stream_no,
                         pcr, pcap_pkt_hdr->ts_sec, pcap_pkt_hdr->ts_usec);
            }

            if (st->pcr_pid == 0)
              st->pcr_pid = pid;

            if (pid != st->pcr_pid)
            {
              // *** If this happens often then fix to track each Pid
              if (!st->multiple_pcr_pids)
              {
                fprint_msg("!%d! Multiple pids detected: pids: %d,%d,...\n",
                  st->stream_no, st->pcr_pid, pid);
              }
              st->multiple_pcr_pids = TRUE;
            }
            else
            {
              // PCR pops out in 27MHz units. Let's do all our comparisons
              // in 90kHz.
              pcr /= 300;

              // fprint_msg("pcr = %lld t_pcr = %lld diff = %lld\n", 
              //            pcr, t_pcr, t_pcr - pcr);

              pcr_time_offset = ((int64_t)t_pcr - (int64_t)pcr);

              skew = st->section_last == NULL ? 0LL :
                pcr_time_offset - (st->section_last->time_start - st->section_last->pcr_start);

              if (st->section_last == NULL ||
                  skew > st->skew_discontinuity_threshold || 
                  skew < -st->skew_discontinuity_threshold)
              {
                pcapreport_section_t * const tsect = section_create(st);

                if (tsect->section_no != 0)
                {
                  fprint_msg(">%d> Skew discontinuity! Skew = %lld (> %lld) at"
                             " ts = %d network = %d (PCR %lld Time %d.%d)\n", 
                             st->stream_no,
                             skew, st->skew_discontinuity_threshold, 
                             st->ts_counter, ctx->pkt_counter,
                             pcr, pcap_pkt_hdr->ts_sec,
                             pcap_pkt_hdr->ts_usec);
                }

                tsect->pkt_final =
                tsect->pkt_start = ctx->pkt_counter;
                tsect->pcr_last =
                tsect->pcr_start = pcr;
                tsect->time_last =
                tsect->time_start = t_pcr;
                tsect->ts_byte_start =
                tsect->ts_byte_final = ts_byte_start;

                jitter_clear(&st->jitter);
                st->last_time_offset = 0;
              }
              else
              {
                pcapreport_section_t * const tsect = st->section_last;

                // Extract jitter over up to the last 10s.  skew will be within
                // an int by now
                unsigned int cur_jitter = jitter_add(&st->jitter, (int)skew,
                  (uint32_t)(t_pcr & 0xffffffffU), 90000 * 10);

                if (tsect->jitter_max < cur_jitter)
                  tsect->jitter_max = cur_jitter;

                if (ctx->time_report)
                {
                  int64_t rel_tim = t_pcr - tsect->time_start; // 90kHz
                  double skew_rate = (double)skew / ((double)((double)rel_tim / (60*90000)));
  
                  fprint_msg(">%d> [ts %d net %d ] PCR %lld Time %d.%d [rel %d.%d]  - skew = %lld (delta = %lld, rate = %.4g PTS/min) - jitter=%u\n",
                             st->stream_no,
                             st->ts_counter, ctx->pkt_counter,
                             pcr, 
                             pcap_pkt_hdr->ts_sec, pcap_pkt_hdr->ts_usec,
                             (int)(rel_tim / (int64_t)1000000), 
                             (int)rel_tim%1000000,
                             skew, pcr_time_offset - st->last_time_offset, 
                             skew_rate, cur_jitter);
                }

                if (st->csv_name != NULL)  // We should be outputting to file
                {
                  if (st->csv_file == NULL)
                  {
                    if ((st->csv_file = fopen(st->csv_name, "wt")) == NULL)
                    {
                      fprint_err("### pcapreport: Cannot open %s .\n", 
                                 st->csv_name);
                      exit(1);
                    }
                    fprintf(st->csv_file, "\"PKT\",\"Time\",\"PCR\",\"Skew\",\"Jitter\"\n");
                  }
                  fprintf(st->csv_file, "%d,%llu,%llu,%lld,%u\n", ctx->pkt_counter, t_pcr - ctx->time_start, pcr, skew, cur_jitter);
                }

                // Remember where we are for posterity
                tsect->pcr_last = pcr;
                tsect->time_last = t_pcr;

                st->last_time_offset = pcr_time_offset;
              }
            }
          }
        }
      }

      {
        pcapreport_section_t * const tsect = st->section_last;
        if (tsect != NULL)
        {
          tsect->time_final = t_pcr;
          tsect->ts_byte_final = st->ts_bytes;
          tsect->pkt_final = ctx->pkt_counter;
        }
      }

      ++st->ts_counter;
    }
  }
}

static int write_out_packet(pcapreport_ctx_t * const ctx,
                            pcapreport_stream_t * const st,
                            const byte *data, 
                            const uint32_t len)
{
  int rv;
  unsigned int pkts = len / 188;

  if (st->output_name)
  {
    if (!st->output_file)
    {
      fprint_msg("pcapreport: Dumping all packets for %s:%d to %s\n",
                 ipv4_addr_to_string(st->output_dest_addr),
                 st->output_dest_port,
                 st->output_name);
      st->output_file = fopen(st->output_name, "wb");
      if (!st->output_file)
      {
        fprint_err("### pcapreport: Cannot open %s .\n", 
                   st->output_name);
        return 1;
      }
    }

    if (ctx->verbose)
    {
      fprint_msg("++   Dumping %d bytes to output file.\n", len);
    }
    rv = fwrite(data, 188, pkts, st->output_file);
    if (rv != pkts)
    {
      fprint_err( "### pcapreport: Couldn't write %d bytes"
                  " to %s (error = %d).\n", 
                  len, st->output_name, 
                  ferror(st->output_file));
      return 1;
    }
  }
  return 0;
}

int
stream_ts_check(pcapreport_ctx_t * const ctx, pcapreport_stream_t * const st,
                            const byte *data, 
                            const uint32_t len)
{
  const byte * ptr;
  int good = 0;
  int bad = 0;

  if (st->force)
    st->ts_good = 10;  

  if (len % 188 != 0)
    ++bad;
  else
    ++good;

  for (ptr = data; ptr < data + len; ptr += 188)
  {
    if (*ptr != 0x47)
      ++bad;
    else
      ++good;
  }

  st->ts_good += good - bad;

  if (st->ts_good > 10)
    st->ts_good = 10;
  if (st->ts_good < -10)
    st->ts_good = -10;

  if (st->ts_good <= 0 || (bad != 0 && ctx->good_ts_only))
  {
    ++st->seen_bad;
    return FALSE;
  }

  if (bad != 0)
    ++st->seen_dodgy;
  ++st->seen_good;
  return TRUE;
}

pcapreport_stream_t *
stream_create(pcapreport_ctx_t * const ctx, uint32_t const dest_addr, const uint32_t dest_port)
{
  pcapreport_stream_t * const st = calloc(1, sizeof(*st));
  st->stream_no = ctx->stream_count++;
  st->output_dest_addr = dest_addr;
  st->output_dest_port = dest_port;

  st->skew_discontinuity_threshold = ctx->opt_skew_discontinuity_threshold;

  // If the dest:port is fully specified then avoid guesswork
  st->force = ctx->filter_dest_addr != 0 && ctx->filter_dest_port != 0;

  if (ctx->extract)
  {
    const char * const base_name = ctx->output_name_base != NULL ? ctx->output_name_base : ctx->base_name;
    size_t len = strlen(base_name);
    st->output_name = malloc(len + 32);
    memcpy(st->output_name, base_name, len + 1);

    // If we have been given a unique filter then assume they actually want
    // that name!
    if (ctx->filter_dest_addr == 0 || ctx->filter_dest_port == 0)
    {
      sprintf(st->output_name + len, "_%u.%u.%u.%u_%u.ts",
        dest_addr >> 24, (dest_addr >> 16) & 0xff,
        (dest_addr >> 8) & 0xff, dest_addr & 0xff,
        dest_port);
    }
  }

  if (ctx->csv_gen)
  {
    const size_t len = strlen(ctx->base_name);
    char * const name = malloc(len + 32);
    memcpy(name, ctx->base_name, len + 1);

    if (ctx->filter_dest_addr == 0 || ctx->filter_dest_port == 0)
    {
      sprintf(name + len, "_%u.%u.%u.%u_%u.csv",
        dest_addr >> 24, (dest_addr >> 16) & 0xff,
        (dest_addr >> 8) & 0xff, dest_addr & 0xff,
        dest_port);
    }
    else
      strcpy(name + len, ".csv");
    st->csv_name = name;
  }

  return st;
}

void
stream_analysis(pcapreport_ctx_t * const ctx, pcapreport_stream_t * const st)
{
  uint32_t dest_addr = st->output_dest_addr;

  if (ctx->verbose < 1 && st->seen_good == 0)
    return;

  fprint_msg("Stream %d: Dest %u.%u.%u.%u:%u\n",
    st->stream_no,
    dest_addr >> 24, (dest_addr >> 16) & 0xff,
    (dest_addr >> 8) & 0xff, dest_addr & 0xff,
    st->output_dest_port);
  if (st->seen_good == 0)
  {
    // Cut the rest of the stats short if they are meaningless
    fprint_msg("  No TS detected: Pkts=%u\n", st->seen_bad);
  }
  else
  {
    const pcapreport_section_t * tsect;

    fprint_msg("  Pkts: Good=%d, Dodgy=%d, Bad=%d, Overlength=%u\n",
      st->seen_good - st->seen_dodgy, st->seen_dodgy, st->seen_bad, st->pkts_overlength);
    fprint_msg("  PCR PID: %d (%#x)%s\n", st->pcr_pid, st->pcr_pid,
      !st->multiple_pcr_pids ? "" : " ### Other PCR PIDs in stream - not tracked");

    for (tsect = st->section_first; tsect != NULL; tsect = tsect->next)
    {
      uint64_t time_offset = ctx->time_start;
      int64_t time_len = tsect->time_last - tsect->time_start;
      int64_t time_len2 = tsect->time_final - tsect->time_start;
      int64_t pcr_len = tsect->pcr_last - tsect->pcr_start;
      int64_t drift = time_len - pcr_len;
      fprint_msg("  Section %d:\n", tsect->section_no);
      fprint_msg("    Pkts: %u->%u\n", tsect->pkt_start, tsect->pkt_final);
      fprint_msg("    Bytes: %llu (%llu bits/sec)\n", tsect->ts_byte_final - tsect->ts_byte_start, 
        time_len2 == 0LL ? 0LL : (tsect->ts_byte_final - tsect->ts_byte_start) * 8ULL * 90000ULL / time_len2);
      fprint_msg("    Time (Total): %s->%s (%s)\n",
        fmtx_timestamp(tsect->time_start - time_offset, ctx->tfmt),
        fmtx_timestamp(tsect->time_final - time_offset, ctx->tfmt),
        fmtx_timestamp(time_len2, ctx->tfmt));
      fprint_msg("    Time (PCRs): %s->%s (%s)\n",
        fmtx_timestamp(tsect->time_start - time_offset, ctx->tfmt),
        fmtx_timestamp(tsect->time_last - time_offset, ctx->tfmt),
        fmtx_timestamp(time_len, ctx->tfmt));
      fprint_msg("    PCR: %s->%s (%s)\n",
        fmtx_timestamp(tsect->pcr_start, ctx->tfmt),
        fmtx_timestamp(tsect->pcr_last, ctx->tfmt),
        fmtx_timestamp(pcr_len, ctx->tfmt));
      fprint_msg("    Drift: diff=%s; rate=%s/min; 1s per %llds\n",
        fmtx_timestamp(time_len - pcr_len, ctx->tfmt),
        fmtx_timestamp(time_len == 0 ? 0LL : drift * 60LL * 90000LL / time_len, ctx->tfmt),
        drift == 0 ? 0LL : time_len / drift);
      fprint_msg("    Max jitter: %s\n", fmtx_timestamp(tsect->jitter_max, ctx->tfmt));
    }
  }

  fprint_msg("\n");
}


unsigned int
stream_hash(uint32_t const dest_addr, const uint32_t dest_port)
{
  uint32_t x = dest_addr ^ dest_port;
  x ^= x >> 16;
  return (x ^ (x >> 8)) & 0xff;
}

pcapreport_stream_t *
stream_find(pcapreport_ctx_t * const ctx, uint32_t const dest_addr, const uint32_t dest_port)
{
  const unsigned int h = stream_hash(dest_addr, dest_port);
  pcapreport_stream_t ** pst = ctx->stream_hash + h;
  pcapreport_stream_t * st;

  while ((st = *pst) != NULL)
  {
    if (st->output_dest_addr == dest_addr && st->output_dest_port == dest_port)
      return st;
    pst = &st->hash_next;
  }

  if ((st = stream_create(ctx, dest_addr, dest_port)) == NULL)
    return NULL;

  *pst = st;
  return st;
}

void
stream_close(pcapreport_ctx_t * const ctx, pcapreport_stream_t ** pst)
{
  pcapreport_stream_t * const st = *pst;
  *pst = NULL;

  {
    // Free off all our section data
    pcapreport_section_t * p = st->section_first;
    while (p != NULL)
    {
      pcapreport_section_t * np = p->next;
      free(p);
      p = np;
    }
  }
  if (st->output_file != NULL)
  {
    if (st->seen_dodgy != 0)
    {
      fprint_msg(">%d> WARNING: %d dodgy packet%s written to: %s\n",
                 st->stream_no,
                 st->seen_dodgy, st->seen_dodgy == 1 ? "" : "s", st->output_name);
    }
    if (st->seen_bad != 0)
    {
      fprint_msg(">%d> WARNING: %d bad packet%s excluded from: %s\n",
                 st->stream_no,
                 st->seen_bad, st->seen_bad == 1 ? "" : "s", st->output_name);
    }
    fclose(st->output_file);
  }
  if (st->csv_file != NULL)
    fclose(st->csv_file);
  if (st->csv_name != NULL)
    free((void *)st->csv_name);
  if (st->output_name != NULL)
    free(st->output_name);
  free(st);
}

static int
ip_reassemble(pcapreport_reassembly_t * const reas, const ipv4_header_t * const ip, void * const in_data,
  void ** const out_pdata, uint32_t * const out_plen)
{
  uint32_t frag_len = ip->length - ip->hdr_length * 4;
  uint32_t frag_offset = ip->frag_offset * 8;  // bytes
  int frag_final = (ip->flags & 1) == 0;

  // Discard unless we succeed
  *out_pdata = NULL;
  *out_plen = 0;

  if (frag_final && frag_offset == 0)
  {
    // Normal case - no fragmentation
    *out_pdata = in_data;
    *out_plen = frag_len;
    return 0;
  }

  if ((frag_len & 7) != 0 && !frag_final)
  {
    // Only final fragment may have length that is not a multiple of 8
    fprint_err("### Non-final fragment with bad length: %d\n", frag_len);
    return -1;
  }

  if (frag_len + frag_offset >= 0x10000)
  {
    // I can't find this explicitly prohibited in RFC791 but it can't be good
    // and the limit should probably be a little less if we were being pedantic
    fprint_err("### Fragment end >= 64k: %d+%d\n", frag_offset, frag_len);
    return -1;
  }

  // Very limited reassembly
  {
    pcapreport_fragment_t * frag = &reas->frag;

    if (frag->in_use && frag->ident != ip->ident)
    {
      fprint_err("### Multi-packet fragment reassembly NIF - previous packet discarded\n");
      frag->in_use = 0;
    }

    // If previously idle then reset stuff
    if (!frag->in_use)
    {
      frag->in_use = 1;
      frag->current_len = 0;
      frag->ident = ip->ident;
    }

    if (frag->current_len != frag_offset)
    {
      fprint_err("### Reordering fragment reassembly NIF - packet discarded\n");
      frag->in_use = 0;
      return -1;
    }

    frag->current_len = frag_offset + frag_len;

    memcpy(frag->pkt + ip->frag_offset * 8, in_data, frag_len);

    if (!frag_final)
      return 1;

    *out_pdata = frag->pkt;
    *out_plen = frag->current_len;
    frag->in_use = 0;
    return 0;
  }
}

static int
ip_reassembly_init(pcapreport_reassembly_t * const reas)
{
  memset(reas, 0, sizeof(*reas));
  return 0;
}


static void print_usage()
{
  print_msg(
    "Usage: pcapreport [switches] <infile>\n"
    "\n"
    );
  REPORT_VERSION("pcapreport");
  print_msg(
    "\n"
    "Report on a pcap capture file.\n"
    "\n"
    "  -h                 This help\n"
    "  -h detail          More detail on what some terms used by pcapreport mean\n"
    "  --name <file>\n"
    "  -n <file>          Set the default base name for output files; by default\n"
    "                     this will be the input name without any .pcap suffix\n"
    "  -x, --extract      Extract TS(s) to files of the default name\n"
    "  -c, --csvgen       Create a .csv file for each stream containing timing info\n"
    "  -output <file>\n"
    "  -o <file>,         Dump selected UDP payloads to output file(s)\n"
    "                     Uses given filename if <ip>:<port> specified,\n"
    "                     otherwise appends <ip>_<port> to filename per TS\n"
    "                     Is much the same as -x -n <name>\n"
    "  -a                 Analyse.  Produces summary info on every TS in the pcap\n"
    "  -d <dest ip>:<port>\n"
    "  -d <dest ip>       Select data with the given destination IP and port.\n"
    "                     If the <port> is not specified, it defaults to 0\n"
    "                     (see below).\n"
    "  -g, --good-ts-only Only extract/analyse packets that seem entirely good.\n"
    "                     By default there is a bit of slack in determining if a\n"
    "                     packet is good and some dodgy packets are let through\n"
    "                     This switch ensures that all packets pass simple testing\n"
    "  -tfmt 32|90|ms|hms Set time format in report [default = 90kHz units]\n"
    "  -dump-data, -D     Dump any data in the input file to stdout.\n"
    "  -extra-dump, -e    Dump only data which isn't being sent to the -o file.\n"
    "  -times,  -t        Report continuously on PCR vs PCAP timing for the\n"
    "                     destination specified in -d.\n"
    "  -verbose, -v       Output metadata about every packet.\n"
    "  -skew-discontinuity-threshold <number>\n"
    "  -skew <number>     Gives the skew discontinuity threshold in 90kHz units.\n"
    "\n"
    "  -err stdout        Write error messages to standard output (the default)\n"
    "  -err stderr        Write error messages to standard error (Unix traditional)\n"
    "\n"
    "Specifying 0.0.0.0 for destination IP will capture all hosts, specifying 0\n"
    "as a destination port will capture all ports on the destination host.\n"
    "\n"
    "Network packet numbers start at 1 (like wireshark)\n"
    "TS packet numbers start at 0.\n"
    "\n"
    "Positive skew means that we received too low a PCR for this timestamp.\n"
    "\n"
    );
}

static char manpage[] =
"Times (packet and PCR)\n"
"----------------------\n"
"\n"
"The times associated with packets and PCR are held internally in 90kHz units\n"
"and are displayed in those units by default\n"
"\n"
"Stream\n"
"------\n"
"\n"
"A set of packets to the same IP & Port.  TS streams are detected by looking\n"
"for 0x47s at appropriate places in the packets\n"
"\n"
"Section\n"
"-------\n"
"A part of a stream which appears to have a continuous TS embedded in it.  If\n"
"the PCR jumps then a new section should be started (though this will not\n"
"generate a separate .ts file if the extraction option is in effect, nor will\n"
"it generate a new .csv file.)\n"
"\n"
"As it stands pcapreport will only report on a single PCR pid within a TS. If\n"
"multiple pids with PCRs are detected then this will be reported but the other\n"
"PCRs will be ignored\n"
"\n"
"Skew\n"
"----\n"
"\n"
"This is the difference between the time in the pcap for a UDP packet and any\n"
"PCR found in the TS contained within that packet.  The accuracy of this figure\n"
"obviously depends on how good the clock was in the capture process.  Skew is\n"
"arbitrarily set to zero at the start of a section.  A skew of >6s is assumed\n"
"to be a discontinuity and will start a new section.\n"
"\n"
"Drift\n"
"-----\n"
"\n"
"This is skew over time and (assuming that the playout process is good)\n"
"represents the difference in speed between the transmitters clock and the\n"
"receivers clock.  The algorithm for determining this isn't very sophisticated\n"
"so if you have a large maximum jitter or a short sample this should be taken\n"
"with a pinch of salt.  Beware also that PC clocks (like the one in the m/c\n"
"doing the tcpdump) are not always amongst the most stable or accurate; however\n"
"they should be good enough to detect gross errors\n"
"\n"
"Jitter\n"
"------\n"
"\n"
"This is measured as the difference between the maximum and minimum skews over\n"
"a 10sec (max 1024 samples) period.  This should be long enough to capture a\n"
"good baseline but short enough that drift has a negligible effect\n"
"\n"
"Max Jitter\n"
"----------\n"
"\n"
"The maximum value of jitter (see above) found in a section\n"
"";


const char *onechararg[26] =
{
  "analyse", // a
  "", // b
  "csvgen", // c
  "destip", // d
  "", // e
  "", // f
  "good-ts-only", // g
  "help", // h
  "", // i
  "", // j
  "", // k
  "", // l
  "", // m
  "name", // n
  "output", // o
  "", // p
  "", // q
  "", // r
  "", // s
  "times", // t
  "", // u
  "verbose", // v
  "", // w
  "extract", // x
  "", // y
  ""  // z
};


int main(int argc, char **argv)
{
  int err = 0;
  int ii = 1;
  pcapreport_ctx_t sctx = {0};
  pcapreport_ctx_t  * const ctx = &sctx;

  ctx->opt_skew_discontinuity_threshold = SKEW_DISCONTINUITY_THRESHOLD;
  ctx->tfmt = FMTX_TS_DISPLAY_90kHz_RAW;

  ip_reassembly_init(&ctx->reassembly_env);

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      // remove double dashes
      const char c = argv[ii][1];
      const char * const arg = c >= 'a' && c <= 'z' && argv[ii][2] == 0 ? onechararg[c - 'a'] :
        argv[ii][1] == '-' ? argv[ii] + 2 : argv[ii] + 1;

      if (strcmp("help", arg) == 0)
      {
        if (ii + 1 < argc && strcmp("detail", argv[ii + 1]) == 0)
        {
          fwrite(manpage, sizeof(manpage), 1, stdout);
          exit(0);
        }
        print_usage();
        return 0;
      }
      else if (!strcmp("err",arg))
      {
        CHECKARG("pcapreport",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### pcapreport: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("output", arg))
      {
        CHECKARG("pcapreport",ii);
        ctx->output_name_base = argv[++ii];
        ctx->extract_data = TRUE;
      }
      else if (!strcmp("times", arg))
      {
        ++ctx->time_report;
      }
      else if (!strcmp("analyse", arg))
      {
        ctx->analyse = TRUE;
      }
      else if (!strcmp("verbose", arg))
      {
        ++ctx->verbose;
      }
      else if (!strcmp("destip", arg))
      {
        char *hostname;
        int port = 0;

        CHECKARG("pcapreport",ii);
        err = host_value("pcapreport", argv[ii], argv[ii+1], &hostname, &port);
        if (err) return 1;
        ++ii;

        ctx->filter_dest_port = port;
        if (ipv4_string_to_addr(&ctx->filter_dest_addr, hostname))
        {
          fprint_err( "### pcapreport: '%s' is not a host IP address (names are not allowed!)\n",
                      hostname);
          return 1;
        }
      }
      else if (!strcmp("dump-data", arg) || !strcmp("D", arg))
      {
        ++ctx->dump_data;
      }
      else if (!strcmp("extra-dump", arg) || !strcmp("E", arg))
      {
        ++ctx->dump_extra;
      }
      else if (!strcmp("skew-discontinuity-threshold", arg) ||
               !strcmp("skew", arg))
      {
        int val;
        CHECKARG("pcapreport",ii);
        err = int_value("pcapreport", argv[ii], argv[ii+1], TRUE, 0, &val); 
        if (err) return 1;
        ctx->opt_skew_discontinuity_threshold = val;
        ++ii;
      }
      else if (strcmp("name", arg) == 0)
      {
        CHECKARG("pcapreport",ii);
        ctx->base_name = strdup(argv[++ii]);  // So we know it is always malloced
      }
      else if (strcmp("extract", arg) == 0)
      {
        ctx->extract = TRUE;
      }
      else if (strcmp("csvgen", arg) == 0)
      {
        ctx->csv_gen = TRUE;
      }
      else if (strcmp("good-ts-only", arg) == 0)
      {
        ctx->good_ts_only = TRUE;
      }
      else if (strcmp("tfmt", arg) == 0)
      {
        int tfmt;
        CHECKARG("pcapreport",ii);
        if ((tfmt = fmtx_str_to_timestamp_flags(argv[ii + 1])) < 0)
        {
          fprint_err("### Bad timeformat: %s\n", argv[ii + 1]);
          exit(1);
        }
        ctx->tfmt = tfmt;
        ++ii;
      }
      else
      {
        fprint_err( "### pcapreport: "
                    "Unrecognised command line switch '%s'\n", argv[ii]);
        return 1;
      }
    }
    else
    {
      if (ctx->had_input_name)
      {
        fprint_err( "### pcapreport: Unexpected '%s'\n", argv[ii]);
        return 1;
      }
      else
      {
        ctx->input_name = argv[ii];
        ctx->had_input_name = TRUE;
      }
    }
    ++ii;
  }

  if (!ctx->had_input_name)
  {
    print_err("### pcapreport: No input file specified\n");
    return 1;
  }

  if (ctx->base_name == NULL)
  {
    // If we have no default name then use the input name as a base after
    // stripping off any .pcap
    const char * input_name = ctx->input_name == NULL ? "pcap" : ctx->input_name;
    char * buf = strdup(ctx->input_name);
    size_t len = strlen(input_name);
    if (len > 5 && strcmp(".pcap", buf + len - 5) == 0)
      buf[len - 5] = 0;
    ctx->base_name = buf;
  }

  fprint_msg("%s\n",ctx->input_name);

  err = pcap_open(&ctx->pcreader, &ctx->pcap_hdr, ctx->input_name);
  if (err)
  {
    fprint_err("### pcapreport: Unable to open input file %s for reading "
               "PCAP (code %d)\n", 
               ctx->had_input_name?ctx->input_name:"<stdin>", err);
    // Just an error code isn't much use - let's look at the source
    // and report something more helpful...
    fprint_err("                %s\n",
               (err==-1?"Unable to open file":
                err==-2?"Unable to allocate PCAP reader datastructure":
                err==-4?"Unable to read PCAP header - is it a PCAP file?":
                "<unrecogised error code>"));
    return 1;
  }

  fprint_msg("Capture made by version %u.%u local_tz_correction "
             "%d sigfigs %u snaplen %d network %u\n", 
             ctx->pcap_hdr.version_major, ctx->pcap_hdr.version_minor,
             ctx->pcap_hdr.thiszone,
             ctx->pcap_hdr.sigfigs,
             ctx->pcap_hdr.snaplen,
             ctx->pcap_hdr.network);

  if (ctx->pcap_hdr.snaplen < 65535)
  {
    fprint_err("### pcapreport: WARNING snaplen is %d, not >= 65535 - "
               "not all data may have been captured.\n", 
               ctx->pcap_hdr.snaplen);
  }


  {
    int done = 0;

    while (!done)
    {
      pcaprec_hdr_t rec_hdr;
      byte *data = NULL;
      uint32_t len = 0;
      int sent_to_output = 0;

      err = pcap_read_next(ctx->pcreader, &rec_hdr, &data, &len);
      switch (err)
      {
      case 0: // EOF.
        ++done;
        break;
      case 1: // Got a packet.
        {
          byte *allocated = data;

          // Wireshark numbers packets from 1 so we shall do the same
          if (ctx->pkt_counter++ == 0)
          {
            // Note time of 1st packet
            ctx->time_usec = rec_hdr.ts_usec;
            ctx->time_sec = rec_hdr.ts_sec;
            ctx->time_start = pkt_time(&rec_hdr);
          }

          if (ctx->verbose)
          {
            fprint_msg("pkt: Time = %d.%d orig_len = %d \n", 
                       rec_hdr.ts_sec, rec_hdr.ts_usec, 
                       rec_hdr.orig_len);
          }

          if (!(ctx->pcap_hdr.network == PCAP_NETWORK_TYPE_ETHERNET))
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


            if (ctx->verbose)
            {
              fprint_msg("++ 802.11: src %02x:%02x:%02x:%02x:%02x:%02x "
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

            if (ctx->verbose)
            {		    
              fprint_msg("++ IPv4: src = %s", 
                         ipv4_addr_to_string(ipv4_hdr.src_addr));
              fprint_msg(" dest = %s \n", 
                         ipv4_addr_to_string(ipv4_hdr.dest_addr));

              fprint_msg("++ IPv4: version = 0x%x hdr_length = 0x%x"
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

            if (ip_reassemble(&ctx->reassembly_env, &ipv4_hdr, data, (void**)&data, &len) != 0)
            {
              goto dump_out;
            }

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

            if (ctx->verbose)
            {
              fprint_msg("++ udp: src port = %d "
                         "dest port = %d len = %d \n",
                         udp_hdr.source_port,
                         udp_hdr.dest_port,
                         udp_hdr.length);
            }

            data = &data[out_st];
            len = out_len;

            if (
              (ctx->filter_dest_addr == 0 || (ipv4_hdr.dest_addr == ctx->filter_dest_addr)) && 
              (ctx->filter_dest_port == 0 || (udp_hdr.dest_port == ctx->filter_dest_port)))
            {
              pcapreport_stream_t * const st = stream_find(ctx, ipv4_hdr.dest_addr, udp_hdr.dest_port);

              if (stream_ts_check(ctx, st, data, len))
              {
                ++sent_to_output;
  
                if (ctx->time_report || ctx->analyse || ctx->csv_gen)
                {
                  rv =digest_times(ctx, 
                                   st,
                                   &rec_hdr,
                                   &epkt,
                                   &ipv4_hdr,
                                   &udp_hdr, 
                                   data, len);
                  if (rv) { return rv; }
                }
                if (ctx->extract)
                {
                  rv = write_out_packet(ctx, st, data, len);
                  if (rv) { return rv; }
                }
              }
            }
          }

          // Adjust 
dump_out:
          if (ctx->dump_data || (ctx->dump_extra && !sent_to_output))
          {
            print_data(TRUE, "data", data, len, len);
          }
          free(allocated); allocated = data = NULL;
        }
        break;
      default:
        // Some other error.
        fprint_err( "### pcapreport: Can't read packet %d - code %d\n",
                    ctx->pkt_counter, err);
        ++done;
        break;
      }
    }
  }

  if (ctx->analyse)
  {
    // Spit out pcap part of the report
    //
    const struct tm * const t = gmtime(&ctx->time_sec);

    fprint_msg("Pcap start time: %llu (%d-%02d-%02d %d:%02d:%02d.%06d)\n", ctx->time_start,
      t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
      t->tm_hour, t->tm_min, t->tm_sec, ctx->time_usec);
    fprint_msg("Pcap pkts: %u\n", ctx->pkt_counter);
    fprint_msg("\n");
  }

  {
    unsigned int i;
    {
      for (i = 0; i != 256; ++i)
      {
        // Kill all on this hash chain - singly linked so slightly dull
        while (ctx->stream_hash[i] != NULL)
        {
          pcapreport_stream_t ** pst = ctx->stream_hash + i;
          // Spin to last el in hash
          while ((*pst)->hash_next != NULL)
            pst = &(*pst)->hash_next;
          // Spit out any remaining info
          if (ctx->analyse)
            stream_analysis(ctx, *pst);
          // Kill it
          stream_close(ctx, pst);
        }
      }
    }
  }

  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

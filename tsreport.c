/*
 * Report on an H.222 transport stream (TS) file.
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
 *   Amino Communications Ltd, Swavesey, Cambridge UK
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
#endif // _WIN32

#include "compat.h"
#include "ts_fns.h"
#include "pes_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "pidint_fns.h"
#include "fmtx.h"
#include "version.h"

#define AV_COUNT 2

// Used to mean "PID unset" for continuity_counter monitoring
#define INVALID_PID     0x2000

static int tfmt_diff = FMTX_TS_DISPLAY_90kHz_RAW;
static int tfmt_abs = FMTX_TS_DISPLAY_90kHz_RAW;

static uint64_t
estimate_pcr(offset_t posn, uint64_t ppcr_pos, uint64_t ppcr_val, double pcr_rate)
{
  return (uint64_t)(ppcr_val + (27000000.0 * (double)(posn - ppcr_pos))/pcr_rate);
}

/* ============================================================================
 * Buffering reporting
 */
struct diff_from_pcr
{
  int64_t         min;          // minimum (absolute) difference
  uint64_t       min_at;       // at what PTS the minimum occurred
  offset_t      min_posn;     // at what position in the file
  int64_t         max;          // and ditto for the maximum (abs) difference
  uint64_t       max_at;
  offset_t      max_posn;
  int64_t         sum;          // the sum of all of the differences
  unsigned int  num;          // the number of TS records compared
};

typedef struct avg_rate_elss
{
  uint64_t time;
  uint64_t bytes;
} avg_rate_el_t;

typedef struct avg_ratess
{
  unsigned int max_els;
  unsigned int in_el;
  unsigned int out_el;
  avg_rate_el_t * els;
  uint64_t max_rate;
} avg_rate_t;

struct stream_data {
  uint32_t       pid;
  int           stream_type;
  int           had_a_pts;
  int           had_a_dts;
  int           first_cc;
  int           last_cc;
  int           cc_dup_count;
  int           discontinuity_flag_count;

  uint64_t       first_pts;
  uint64_t       first_dts;

  // Keep these in our datastructure so we can easily report the last
  // PTS/DTS in the file, when we're finishing up
  uint64_t       pts;
  uint64_t       dts;
  uint64_t       pcr;

  int           err_pts_lt_dts;
  int           err_dts_lt_prev_dts;
  int           err_dts_lt_pcr;
  int           err_cc_error;
  int           err_cc_dup_error;
  int           err_cc_contents;
  int           cc_good;

  struct diff_from_pcr        pcr_pts_diff;
  struct diff_from_pcr        pcr_dts_diff;

  // Inter DTS max/min values
  long          dts_dts_min;
  long          dts_dts_max;

  int           pts_ne_dts;

  int           pcr_seen;
  uint64_t      first_pcr;
  uint64_t      ts_bytes;
  avg_rate_t    rate;

  uint8_t       last_pkt[188];
};

static int pid_index(struct stream_data *data,
                     int                 num_streams,
                     uint32_t             pid)
{
  int ii;
  for (ii=0; ii<num_streams; ii++)
    if (data[ii].pid == pid)
      return ii;
  return -1;
}

unsigned int
avg_rate_inc(avg_rate_t * ar, unsigned int n)
{
  return n + 1 >= ar->max_els ? 0 : n + 1;
}

static void
avg_rate_add(avg_rate_t * ar, uint64_t time, uint64_t bytes)
{
  uint64_t gap = 27000000 / 2;  // 0.5 sec
  uint64_t delta_b;
  uint64_t delta_t;
  uint64_t rate;

  if (ar->els == NULL)
  {
    ar->max_els = 1024;
    ar->els = calloc(ar->max_els, sizeof(ar->els[0]));
  }

  while (ar->in_el != ar->out_el && pcr_unsigned_diff(time, ar->els[ar->out_el].time) > gap)
  {
    ar->out_el = avg_rate_inc(ar, ar->out_el);
  }

  ar->els[ar->in_el].time = time;
  ar->els[ar->in_el].bytes = bytes;

  delta_b = bytes - ar->els[ar->out_el].bytes;
  delta_t = pcr_unsigned_diff(time, ar->els[ar->out_el].time);

  if (delta_t != 0)
  {
    rate = (delta_b * 8LL * 27000000LL) / delta_t;
    if (rate > ar->max_rate)
    {
      ar->max_rate = rate;
    }
  }

  ar->in_el = avg_rate_inc(ar, ar->in_el);
}



/*
 * Report on the given file
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int report_buffering_stats(TS_reader_p  tsreader,
                                  const int    req_prog_no,
                                  int          max,
                                  int          verbose,
                                  int          quiet,
                                  char        *output_name,
                                  uint32_t     continuity_cnt_pid,
                                  uint64_t     report_mask)
{
  pmt_p         pmt = NULL;
  int           err;
  FILE         *file = NULL;
  int           num_streams = 0;
  FILE         *file_cnt = NULL;

  // Define an arbitrary maximum number of streams we support
  // -- this is simpler than coping with changing the array sizes
  // when we find PMTs that indicate we need more streams -- this
  // limit should be big enough (we believe!) for any file we're
  // likely to find.
#define MAX_NUM_STREAMS       100

  struct stream_data stats[MAX_NUM_STREAMS];

  // We want to be able to report on how well a simple linear-prediction
  // model for PCRs would work (i.e., given the last two PCRs, how well
  // can we predict the *actual* PCR value). It's useful to bundle the
  // data for that in one place...
  struct linear_prediction_data {
    int           had_a_pcr;      // Have we had a PCR from a PCR PID TS?
    uint64_t      prev_pcr;       // if we have, what the last one was
    offset_t      prev_pcr_posn;  // and which TS it was from
    double        pcr_rate;
    int           know_pcr_rate;
    int64_t       min_pcr_error;  // 27MHz
    int64_t       max_pcr_error;  // 27MHz
  };
  struct linear_prediction_data predict = {0};

  uint32_t      pcr_pid;
  uint64_t      first_pcr = 0;
  offset_t      first_pcr_posn = 0;
  int           pmt_at = 0;     // in case we don't look for a PMT
  int           index;
  int           ii;

  unsigned int  pcr_count = 0;
  uint64_t      max_pcr_gap = 0;
  unsigned int  bad_pcr_gap_count = 0;

  int           first = TRUE;
  offset_t      posn = 0;
  offset_t      start_posn = 0;
  uint32_t       count = 0;
  uint32_t       start_count = 0;

  memset(stats,0,sizeof(stats));
  for (ii=0; ii<MAX_NUM_STREAMS; ii++)
  {
    stats[ii].pcr_pts_diff.min = LONG_MAX;
    stats[ii].pcr_dts_diff.min = LONG_MAX;
    stats[ii].pcr_pts_diff.max = LONG_MIN;
    stats[ii].pcr_dts_diff.max = LONG_MIN;
    stats[ii].dts_dts_min = LONG_MAX;
    stats[ii].dts_dts_max = LONG_MIN;
    stats[ii].first_pcr = ~(uint64_t)0;
    stats[ii].last_cc = -1;
    stats[ii].first_cc = -1;
  }
  predict.min_pcr_error = LONG_MAX;
  predict.max_pcr_error = LONG_MIN;

  if (output_name)
  {
    file = fopen(output_name,"w");
    if (file == NULL)
    {
      fprint_err("### tsreport: Unable to open file %s: %s\n",
                 output_name,strerror(errno));
      return 1;
    }
    fprint_msg("Writing CSV data to file %s\n",output_name);
    fprintf(file,"#TSoffset,calc|read,PCR/300,stream,audio|video,PTS,DTS,ESCR\n");
  }

  // First we need to determine what we're taking our data from.
  err = find_pmt(tsreader, req_prog_no, max,FALSE,quiet,&pmt_at,&pmt);
  if (err) return 1;

  pcr_pid = pmt->PCR_pid;

  // Tell the buffering mechanism we want to use it
  err = prime_read_buffered_TS_packet(tsreader,pcr_pid);
  if (err) return 1;

  for (ii=0; ii<pmt->num_streams; ii++)
  {
    uint32_t pid = pmt->streams[ii].elementary_PID;
    if (ii >= MAX_NUM_STREAMS)
    {
      fprint_msg("!!! Found more than %d streams -- just reporting on the first %d found\n",
                 MAX_NUM_STREAMS,MAX_NUM_STREAMS);
      break;
    }
    if (pid >= 0x10 && pid <= 0x1FFE)
    {
      stats[num_streams].stream_type = pmt->streams[ii].stream_type;
      stats[num_streams].pid = pid;
      num_streams ++;
    }
  }

  fprint_msg("Looking at PCR PID %04x (%d)\n",pcr_pid,pcr_pid);
  for (ii=0; ii<num_streams; ii++)
    fprint_msg("  Stream %d: PID %04x (%d), %s\n",ii,stats[ii].pid,stats[ii].pid,
               h222_stream_type_str(stats[ii].stream_type));

  // Now do the actual work...
  start_count = count = pmt_at;
  start_posn  = posn  = tsreader->posn - TS_PACKET_SIZE;

  if (continuity_cnt_pid != INVALID_PID)
  {
    file_cnt = fopen("continuity_counter.txt","w");
    if (file_cnt == NULL)
    {
      print_err("### tsreport: Unable to open file continuity_counter.txt\n");
      return 1;
    }
  }
  for (;;)
  {
    uint32_t pid;
    int     payload_unit_start_indicator;
    byte   *packet;
    byte   *adapt, *payload;
    int     adapt_len, payload_len;
    int     got_pcr = FALSE;
    uint64_t acc_pcr = 0;        // The accurate PCR per TS packet

    if (max > 0 && count >= (uint32_t)max)
    {
      fprint_msg("Stopping after %d packets (PMT was at %d)\n",max,pmt_at);
      break;
    }

    // Read the next TS packet, taking advantage of our read-ahead buffering
    // so that we know what its PCR *really* is
    if (first)
    {
      err = read_first_TS_packet_from_buffer(tsreader,pcr_pid,start_count,
                                             &packet,&pid,&acc_pcr,&count);
      posn = start_posn + (count-start_count)*TS_PACKET_SIZE;
      first = FALSE;
    }
    else
    {
      err = read_next_TS_packet_from_buffer(tsreader,&packet,&pid,&acc_pcr);
      count ++;
      posn += TS_PACKET_SIZE;
    }
    if (err == EOF)
      break;
    else if (err)
    {
      fprint_err("### Error reading TS packet %d at " OFFSET_T_FORMAT
                 "\n",count,posn);
      return 1;
    }

    err = split_TS_packet(packet,&pid,
                          &payload_unit_start_indicator,
                          &adapt,&adapt_len,&payload,&payload_len);
    if (err)
    {
      fprint_err("### Error splitting TS packet %d at " OFFSET_T_FORMAT
                 "\n",count,posn);
      return 1;
    }

    // ========================================================================
    // If we actually had a PCR, then we need to remember it
    if (pid == pcr_pid)
    {
      uint64_t   adapt_pcr;
      // Do I need to check that this is the same PCR I got earlier?
      // I certainly hope not...
      get_PCR_from_adaptation_field(adapt,adapt_len,&got_pcr,&adapt_pcr);
      if (got_pcr)
      {
        ++pcr_count;

        if (predict.know_pcr_rate)
        {
          // OK, so what we have predicted this PCR would be,
          // given the previous two PCRs and a linear rate?
          uint64_t guess_pcr = estimate_pcr(posn,predict.prev_pcr_posn,
                                           predict.prev_pcr,predict.pcr_rate);
          int64_t delta = pcr_signed_diff(adapt_pcr, guess_pcr);
          if (delta < predict.min_pcr_error)
            predict.min_pcr_error = delta;
          if (delta > predict.max_pcr_error)
            predict.max_pcr_error = delta;
        }

        if (verbose)
          fprint_msg(OFFSET_T_FORMAT_8 ": read PCR %s\n",
                     posn,
                     fmtx_timestamp(adapt_pcr, tfmt_abs | FMTX_TS_N_27MHz));
        if (file)
          fprintf(file,OFFSET_T_FORMAT ",read," LLU_FORMAT ",,,,\n",
                  posn,(adapt_pcr / (uint64_t)300) & report_mask);

        if (predict.had_a_pcr)
        {
          if (pcr_signed_diff(predict.prev_pcr, adapt_pcr) > 0)
          {
            fprint_err("!!! PCR %s at TS packet "
                       OFFSET_T_FORMAT " is not more than previous PCR %s\n",
                       fmtx_timestamp(adapt_pcr, tfmt_abs | FMTX_TS_N_27MHz),
                       posn,
                       fmtx_timestamp(predict.prev_pcr, tfmt_abs | FMTX_TS_N_27MHz));
          }
          else
          {
            uint64_t delta_pcr = pcr_unsigned_diff(adapt_pcr, predict.prev_pcr);
            int delta_bytes = (int)(posn - predict.prev_pcr_posn);
            predict.pcr_rate = ((double)delta_bytes * 27.0 / (double)delta_pcr) * 1000000.0;
            predict.know_pcr_rate = TRUE;

            if (delta_pcr > max_pcr_gap)
              max_pcr_gap = delta_pcr;

            if (delta_pcr > 27000000 / 10)
            {
              if (bad_pcr_gap_count++ == 0)
                fprint_err("!!! PCR gap of %s @ PCR %s > 0.1sec...\n",
                    fmtx_timestamp(delta_pcr, tfmt_diff | FMTX_TS_N_27MHz),
                    fmtx_timestamp(adapt_pcr, tfmt_abs | FMTX_TS_N_27MHz));
            }

#if 0   // XXX
            fprint_msg("PCR RATE = %f, DELTA_BYTES = %d, DELTA_PCR " LLU_FORMAT
                       ", PCR = " LLU_FORMAT "\n",
                       predict.pcr_rate,delta_bytes,delta_pcr,adapt_pcr);
#endif
          }
        }
        else
        {
          if (!quiet)
            fprint_msg("First PCR at " OFFSET_T_FORMAT "\n",posn);
          first_pcr = adapt_pcr;
          first_pcr_posn = posn;
          predict.had_a_pcr = TRUE;
        }
        predict.prev_pcr = adapt_pcr;
        predict.prev_pcr_posn = posn;
      }

      {
        int i;
        for (i = 0; i != num_streams; ++i)
        {
          stats[i].pcr_seen = TRUE;
        }
      }
    }   // end of working with a PCR PID packet
    // ========================================================================

    index = pid_index(stats,num_streams,pid);

    if (index != -1)
    {
      // Do continuity counter checking
      const int cc = packet[3] & 15;
      const int is_discontinuity = (adapt != NULL && (adapt[0] & 0x80) != 0);
      struct stream_data * const ss = stats + index;

      // Log if required
      if (continuity_cnt_pid == pid)
        fprintf(file_cnt, "%d%c", cc, cc == 15 ? '\n' : ' ');

      // Count flagged discontinuities & note what the first CC in the file is
      ss->discontinuity_flag_count += is_discontinuity;
      if (ss->first_cc < 0)
        ss->first_cc = cc;

      // CC is meant to increment if we have a payload and not if we don't
      // CC may legitimately 'be wrong' if the discontinuity flag is set

      if (ss->last_cc > 0 && !is_discontinuity)
      {
        // We are allowed 1 dup packet
        if (ss->last_cc == cc)
        {
          if (payload)
          {
            if (ss->cc_dup_count++ != 0)
            {
              if (continuity_cnt_pid == pid)
                fprintf(file_cnt, "[Duplicate error] ");
              if (ss->err_cc_dup_error++ == 0)
              {
                fprint_msg("### PID(%d): Continuity Counter >1 duplicate %d at " OFFSET_T_FORMAT "\n",
                           ss->pid, cc, posn);
              }
            }
  
            // Whilst everything else must be identical PCR is expected to
            // change if it is given.  If it exists we know where it is.
            if (!got_pcr ? (memcmp(ss->last_pkt, packet, 188) != 0) :
                (memcmp(ss->last_pkt, packet, 6) != 0 ||
                 memcmp(ss->last_pkt + 12, packet + 12, 188 - 12) != 0))
            {
              if (ss->err_cc_contents++ == 0)
                fprint_msg("### PID(%d): Continuity Counter duplicate %d: non identical contents at " OFFSET_T_FORMAT "\n",
                           ss->pid, cc, posn);
              // Assume that non-identical CC means we had a discontinuity and
              // therefore let this packet through
#if 0
              {
                const uint8_t * a = ss->last_pkt;
                const uint8_t * b = packet;
                int i;

                fprint_msg("CC contents:\n");

                for (i = 0; i < 188; i += 16, a += 16, b += 16)
                {
                  int j;
                  const int n = min(16, 188 - i);
                  for (j = 0; j < n; ++j)
                  {
                    fprint_msg("%c%02x", a[j] == b[j] ? ' ' : '*', a[j]);
                  }
                  fprint_msg("\n");
                  for (j = 0; j < n; ++j)
                  {
                    fprint_msg("%c%02x", a[j] == b[j] ? ' ' : '*', b[j]);
                  }
                  fprint_msg("\n\n");
                }
              }
#endif
            }
            else
            {
              // Real redundant TS packet!
              // Log it and discard
              ++ss->cc_good;
              continue;
            }
          }
        }
        else
        {
          // Otherwise CC must go up by 1 mod 16
          ss->cc_dup_count = 0;
          if (payload)
          {
            if (((ss->last_cc + 1) & 15) != cc)
            {
              if (continuity_cnt_pid == pid)
                fprintf(file_cnt, "[Discontinuity] ");
              if (ss->err_cc_error++ == 0)
              {
                fprint_msg("### PID(%d): Continuity Counter discontinuity %d->%d at " OFFSET_T_FORMAT "\n",
                  ss->pid, ss->last_cc, cc, posn);
              }
            }
          }
          else
          {
            // CC not the same but it should be
            if (continuity_cnt_pid == pid)
              fprintf(file_cnt, "[Discontinuity] ");
            if (ss->err_cc_error++ == 0)
            {
              fprint_msg("### PID(%d): Continuity Counter discontinuity %d->%d (but no payload) at " OFFSET_T_FORMAT "\n",
                ss->pid, ss->last_cc, cc, posn);
            }
          }
        }
      }
      ss->last_cc = cc;
      memcpy(ss->last_pkt, packet, 188);
    }

    if (index != -1)
    {
      if (stats[index].pcr_seen)
      {
        stats[index].pcr_seen = FALSE;
        avg_rate_add(&stats[index].rate, acc_pcr, stats[index].ts_bytes);
      }
      if (stats[index].first_pcr == ~(uint64_t)0)
        stats[index].first_pcr = acc_pcr;
      stats[index].pcr = acc_pcr;
      stats[index].ts_bytes += 188;
    }

    if (index != -1 && payload && payload_unit_start_indicator)
    {
      // We are the start of a PES packet
      // We'll assume "enough" of the PES packet is in this TS
      int   got_pts, got_dts;
      const uint64_t last_dts = stats[index].dts;
      uint64_t pcr_time_now_div300 = 0;
      int64_t   difference;
      err = find_PTS_DTS_in_PES(payload,payload_len,
                                &got_pts,&stats[index].pts,&got_dts,&stats[index].dts);
      if (err)
      {
        fprint_err("### PID(%d): Error looking for PTS/DTS in TS packet at "
                   OFFSET_T_FORMAT "\n", stats[index].pid, posn);
        continue;
      }

      if (got_dts && !got_pts)
      {
        fprint_err("### Got DTS but not PTS, in TS packet at "
                   OFFSET_T_FORMAT "\n",posn);
        return 1;
      }

      if (!got_pts)
        continue;

      pcr_time_now_div300 = acc_pcr/300ULL;

      // Do a few simple checks
      // For the sake of simplicity we ignore 33bit wrap...
      if (pts_signed_diff(stats[index].pts, stats[index].dts) < 0)
      {
        if (stats[index].err_pts_lt_dts++ == 0)
          fprint_msg("### PID(%d): PTS (%s) < DTS (%s)\n",
                     stats[index].pid,
                     fmtx_timestamp(stats[index].pts, tfmt_abs),
                     fmtx_timestamp(stats[index].dts, tfmt_abs));
      }
      if (stats[index].had_a_dts)
      {
        int64_t dts_dts_diff = pts_signed_diff(stats[index].dts, last_dts);
        if (dts_dts_diff < stats[index].dts_dts_min)
          stats[index].dts_dts_min = (long)dts_dts_diff;
        if (dts_dts_diff > stats[index].dts_dts_max)
          stats[index].dts_dts_max = (long)dts_dts_diff;

        if (dts_dts_diff < 0)
        {
          if (stats[index].err_dts_lt_prev_dts++ == 0)
            fprint_msg("### PID(%d): DTS (%s) < previous DTS (%s)\n",
                       stats[index].pid,
                       fmtx_timestamp(stats[index].dts, tfmt_abs),
                       fmtx_timestamp(last_dts, tfmt_abs));
        }
      }
      if (pts_signed_diff(stats[index].dts, pcr_time_now_div300) < 0)
      {
        if (stats[index].err_dts_lt_pcr++ == 0)
          fprint_msg("### PID(%d): DTS (%s) < PCR (%s)\n",
                     stats[index].pid,
                     fmtx_timestamp(stats[index].dts, tfmt_abs),
                     fmtx_timestamp(acc_pcr, tfmt_abs | FMTX_TS_N_27MHz));
      }

      if (!stats[index].had_a_pts)
      {
#if 0  // XXX Sometimes useful to know
        fprint_msg("  First stream %d PTS (after first PCR) at " OFFSET_T_FORMAT "\n",
                   index,posn);
#endif
        stats[index].first_pts = stats[index].pts;
        stats[index].had_a_pts = TRUE;
      }
      if (got_dts && !stats[index].had_a_dts)
      {
#if 0  // XXX Sometimes useful to know
        fprint_msg("  First stream %d DTS (after first PCR) at " OFFSET_T_FORMAT "\n",
                   index,posn);
#endif
        stats[index].first_dts = stats[index].dts;
        stats[index].had_a_dts = TRUE;
      }
      if (got_pts != got_dts || (got_pts && stats[index].pts != stats[index].dts))
        stats[index].pts_ne_dts = TRUE;

      if (file)
      {
        // At the moment, we only report any ESCR to the file
        int       got_escr = FALSE;
        uint64_t   escr;
        (void) find_ESCR_in_PES(payload,payload_len,&got_escr,&escr);

        fprintf(file,OFFSET_T_FORMAT ",%s," LLU_FORMAT ",%d,%s,",
                posn,
                (pcr_pid == pid && got_pcr)?"read":"calc",
                pcr_time_now_div300 & report_mask,
                index,
                IS_AUDIO_STREAM_TYPE(stats[index].stream_type)?"audio":
                IS_VIDEO_STREAM_TYPE(stats[index].stream_type)?"video":"");

        fprintf(file,LLU_FORMAT ",",stats[index].pts & report_mask);
        if (got_dts)
          fprintf(file,LLU_FORMAT,stats[index].dts & report_mask);
        else
          fprintf(file,LLU_FORMAT,stats[index].pts & report_mask);
        fprintf(file,",");
        if (got_escr)
        {
          if (!quiet)
            fprint_msg("Found ESCR " LLU_FORMAT " at " OFFSET_T_FORMAT "\n",
                       escr,posn);
          fprintf(file,LLU_FORMAT,escr & report_mask);
        }
        fprintf(file, ",%u", (payload[4] << 8) | payload[5]);
        fprintf(file,"\n");
      }

      if (verbose)
      {
        fprint_msg(OFFSET_T_FORMAT_8 ": %s PCR " LLU_FORMAT " %d %5s",
                   posn,
                   (pcr_pid == pid && got_pcr)?"    ":"calc",
                   pcr_time_now_div300,
                   index,
                   IS_AUDIO_STREAM_TYPE(stats[index].stream_type)?"audio":
                   IS_VIDEO_STREAM_TYPE(stats[index].stream_type)?"video":"");
      }

      difference = pts_signed_diff(stats[index].pts, pcr_time_now_div300);
      if (verbose)
      {
        fprint_msg(" PTS " LLU_FORMAT,stats[index].pts);
        print_msg(" PTS-PCR ");
        fprint_msg(LLD_FORMAT, difference);
      }
      if (difference > stats[index].pcr_pts_diff.max)
      {
        stats[index].pcr_pts_diff.max = difference;
        stats[index].pcr_pts_diff.max_at = stats[index].pts;
        stats[index].pcr_pts_diff.max_posn = posn;
      }
      if (difference < stats[index].pcr_pts_diff.min)
      {
        stats[index].pcr_pts_diff.min = difference;
        stats[index].pcr_pts_diff.min_at = stats[index].pts;
        stats[index].pcr_pts_diff.min_posn = posn;
      }
      stats[index].pcr_pts_diff.sum += difference;
      stats[index].pcr_pts_diff.num ++;

      if (got_dts)
      {
        difference = pts_signed_diff(stats[index].dts, pcr_time_now_div300);
        if (verbose)
        {
          fprint_msg(" DTS " LLU_FORMAT,stats[index].dts);
          print_msg(" DTS-PCR ");
          fprint_msg(LLD_FORMAT, difference & report_mask);
        }
        if (difference > stats[index].pcr_dts_diff.max)
        {
          stats[index].pcr_dts_diff.max = difference;
          stats[index].pcr_dts_diff.max_at = stats[index].dts;
          stats[index].pcr_dts_diff.max_posn = posn;
        }
        if (difference < stats[index].pcr_dts_diff.min)
        {
          stats[index].pcr_dts_diff.min = difference;
          stats[index].pcr_dts_diff.min_at = stats[index].dts;
          stats[index].pcr_dts_diff.min_posn = posn;
        }
        stats[index].pcr_dts_diff.sum += difference;
        stats[index].pcr_dts_diff.num ++;
      }

      if (verbose)
        print_msg("\n");
    }
  }

  if (continuity_cnt_pid != INVALID_PID)
  {
    fprintf(file_cnt, "\n");
    fclose(file_cnt);
  }

  if (!quiet)
    fprint_msg("Last PCR at " OFFSET_T_FORMAT "\n",predict.prev_pcr_posn);
  fprint_msg("Read %d TS packet%s\n",count,(count==1?"":"s"));
  if (pmt) free_pmt(&pmt);
  if (file) fclose(file);
  if (predict.had_a_pcr && predict.prev_pcr_posn > first_pcr_posn)
  {
    // Multiply by 8 at the end to give us a bit more headroom in file size
    int rate = (int)((predict.prev_pcr_posn - first_pcr_posn) * 27000000LL / pcr_unsigned_diff(predict.prev_pcr, first_pcr)) * 8;
    fprint_msg("Overall stream rate=%d bits/sec\n", rate);
  }

  fprint_msg("PCRs found: %u, Bad (>.1s) gaps: %u, Max gap: %s\n",
      pcr_count, bad_pcr_gap_count,
      fmtx_timestamp(max_pcr_gap, tfmt_diff | FMTX_TS_N_27MHz));
  fprint_msg("Linear PCR prediction errors: min=%s, max=%s\n",
             fmtx_timestamp(predict.min_pcr_error, tfmt_diff | FMTX_TS_N_27MHz),
             fmtx_timestamp(predict.max_pcr_error, tfmt_diff | FMTX_TS_N_27MHz));

  for (ii = 0; ii < num_streams; ii++)
  {
    struct stream_data * const ss = stats + ii;

    fprint_msg("\nStream %d: PID %04x (%d), %s\n",ii,ss->pid,ss->pid,
               h222_stream_type_str(ss->stream_type));
    if (ss->pcr_pts_diff.num > 0)
    {
      fprint_msg("  PCR/%s:\n    Minimum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
                 ss->pts_ne_dts ? "PTS" : "PTS,DTS",
                 fmtx_timestamp(ss->pcr_pts_diff.min, tfmt_diff),
                 fmtx_timestamp(ss->pcr_pts_diff.min_at, tfmt_abs),
                 ss->pcr_pts_diff.min_posn);
      fprint_msg("    Maximum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
                 fmtx_timestamp(ss->pcr_pts_diff.max, tfmt_diff),
                 fmtx_timestamp(ss->pcr_pts_diff.max_at, tfmt_abs),
                 ss->pcr_pts_diff.max_posn);
      fprint_msg("    i.e., a span of %s\n",
                 fmtx_timestamp(ss->pcr_pts_diff.max - ss->pcr_pts_diff.min, tfmt_diff));
      fprint_msg("    Mean difference (of %u) is %s\n",
                 ss->pcr_pts_diff.num,
                 fmtx_timestamp((int64_t)(ss->pcr_pts_diff.sum/(double)ss->pcr_pts_diff.num), tfmt_diff));
    }

    if (ss->pcr_dts_diff.num > 0 && ss->pts_ne_dts)
    {
      fprint_msg("  PCR/DTS:\n    Minimum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
                 fmtx_timestamp(ss->pcr_dts_diff.min, tfmt_diff),
                 fmtx_timestamp(ss->pcr_dts_diff.min_at, tfmt_abs),
                 ss->pcr_dts_diff.min_posn);
      fprint_msg("    Maximum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
                 fmtx_timestamp(ss->pcr_dts_diff.max, tfmt_diff),
                 fmtx_timestamp(ss->pcr_dts_diff.max_at, tfmt_abs),
                 ss->pcr_dts_diff.max_posn);
      fprint_msg("    i.e., a span of %s\n",
                 fmtx_timestamp(ss->pcr_dts_diff.max - ss->pcr_dts_diff.min, tfmt_diff));
      fprint_msg("    Mean difference (of %u) is %s\n",
                 ss->pcr_dts_diff.num,
                 fmtx_timestamp((int64_t)(ss->pcr_dts_diff.sum/(double)ss->pcr_dts_diff.num), tfmt_diff));
    }
    if (ss->had_a_dts)
    {
      fprint_msg("  DTS-last DTS: min=%s, max=%s\n",
        fmtx_timestamp(ss->dts_dts_min, tfmt_diff),
        fmtx_timestamp(ss->dts_dts_max, tfmt_diff));
    }

    fprint_msg("  First PCR %8s, last %8s\n",
               fmtx_timestamp(first_pcr, tfmt_abs | FMTX_TS_N_27MHz),
               fmtx_timestamp(predict.prev_pcr, tfmt_abs | FMTX_TS_N_27MHz));
    if (ss->pcr_pts_diff.num > 0)
      fprint_msg("  First PTS %8s, last %8s\n",
                 fmtx_timestamp(ss->first_pts, tfmt_abs),
                 fmtx_timestamp(ss->pts, tfmt_abs));
    if (ss->pcr_dts_diff.num > 0)
      fprint_msg("  First DTS %8s, last %8s\n",
                 fmtx_timestamp(ss->first_dts, tfmt_abs),
                 fmtx_timestamp(ss->dts, tfmt_abs));

    {
      // Calculate rate over the range of PCRs seen in this stream
      uint64_t avg = ss->pcr == ss->first_pcr ? 0LL :
         ((ss->ts_bytes - 188LL) * 8LL * 27000000LL) / pcr_unsigned_diff(ss->pcr, ss->first_pcr);
      fprint_msg("  Stream: %llu bytes; rate: avg %llu bits/s, max %llu bits/s\n", ss->ts_bytes, avg, ss->rate.max_rate);
    }
    if (ss->discontinuity_flag_count != 0)
      fprint_msg("  Discontinuity flags: *%d", ss->discontinuity_flag_count);
    fprint_msg("  CC: first: %d, last: %d; duplicate packets: %d\n", ss->first_cc, ss->last_cc, ss->cc_good);

    if (ss->err_cc_error != 0)
      fprint_msg("  ### CC error * %d\n", ss->err_cc_error);
    if (ss->err_cc_contents != 0)
      fprint_msg("  ### CC contents error * %d\n", ss->err_cc_contents);
    if (ss->err_cc_dup_error != 0)
      fprint_msg("  ### CC duplicate error * %d\n", ss->err_cc_dup_error);
    if (ss->err_pts_lt_dts != 0)
      fprint_msg("  ### PTS < DTS * %d\n", ss->err_pts_lt_dts);
    if (ss->err_dts_lt_prev_dts != 0)
      fprint_msg("  ### DTS < prev DTS * %d\n", ss->err_dts_lt_prev_dts);
    if (ss->err_dts_lt_pcr != 0)
      fprint_msg("  ### DTS < PCR * %d\n", ss->err_dts_lt_pcr);
  }
  return 0;
}

/*
 * Report on the given file
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int report_ts(TS_reader_p  tsreader,
                     int          max,
                     int          verbose,
                     int          show_data,
                     int          report_timing)
{
  struct timing times = {0};
  pidint_list_p prog_list = NULL;
  pmt_p         pmt = NULL;
  int           err;
  int           count = 0;
  timing_p      time_ptr = NULL;

  byte     *pat_data = NULL;
  int       pat_data_len = 0;
  int       pat_data_used = 0;

  uint32_t   unfinished_pmt_pid = 0;
  byte     *pmt_data = NULL;
  int       pmt_data_len = 0;
  int       pmt_data_used = 0;

  if (report_timing)
    time_ptr = &times;

  for (;;)
  {
    uint32_t pid;
    int     payload_unit_start_indicator;
    byte   *adapt, *payload;
    int     adapt_len, payload_len;

    if (max > 0 && count >= max)
    {
      fprint_msg("Stopping after %d packets\n",max);
      break;
    }

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
      break;
    else if (err)
    {
      fprint_err("### Error reading TS packet %d at " OFFSET_T_FORMAT
                 "\n",count,tsreader->posn - TS_PACKET_SIZE);
      free_pidint_list(&prog_list);
      if (pmt_data) free(pmt_data);
      return 1;
    }

    count ++;

    if (verbose)
      fprint_msg(OFFSET_T_FORMAT_8 ": TS Packet %2d PID %04x%s",
                 tsreader->posn - TS_PACKET_SIZE,count,pid,
                 (payload_unit_start_indicator?" [pusi]":""));

    // Report on what we may
    if (verbose)
    {
      if (pid == 0x1fff)
        print_msg(" PADDING - ignored\n");
      else if (pid == 0x0000)
        print_msg(" PAT\n");
      else if (pid == 0x0001)
        print_msg(" Conditional Access Table - ignored\n");
      else if (pid >= 0x0002 && pid <= 0x000F)
        print_msg(" RESERVED - ignored\n");
      else if (pid_in_pidint_list(prog_list,pid))
        print_msg(" PMT\n");
      else if (pid_in_pmt(pmt,pid))
      {
        pmt_stream_p  stream = pid_stream_in_pmt(pmt,pid);
        if (stream == NULL)
        {
          fprint_err("### Internal error: stream for PID %0x returned NULL"
                     " in PMT\n",pid);
          report_pmt(FALSE,"    ",pmt);
          free_pidint_list(&prog_list);
          free_pmt(&pmt);
          if (pmt_data) free(pmt_data);
          return 1;
        }
        fprint_msg(" stream type %02x (%s)\n",
                   stream->stream_type,h222_stream_type_str(stream->stream_type));
      }
      else
        print_msg(" stream type not identified\n");
    }

    // Ignore padding packets
    if (pid == 0x1fff)
      continue;

    // Conditional Access Tables *might* contain a PCR - do we want
    // to ignore them anyway? Well, since I've never seen one, do so for now
    if (pid == 0x0001)
      continue;

    if (report_timing)
      report_adaptation_timing(time_ptr,adapt,adapt_len,count);
    else if (verbose)
      report_adaptation_field(adapt,adapt_len);

    if (pid == 0)
    {
      if (payload_unit_start_indicator && pat_data)
      {
        // Lose any data we started but didn't complete
        free(pat_data);
        pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pat_data)
      {
        fprint_err("!!! Discarding partial (unstarted) PAT in TS"
                   " packet at " OFFSET_T_FORMAT "\n",
                   tsreader->posn - TS_PACKET_SIZE);
        continue;
      }

      err = build_psi_data(verbose,payload,payload_len,pid,
                           &pat_data,&pat_data_len,&pat_data_used);
      if (err)
      {
        fprint_err("### Error %s PAT in TS packet at " OFFSET_T_FORMAT "\n",
                   (payload_unit_start_indicator?"starting new":"continuing"),
                   tsreader->posn - TS_PACKET_SIZE);
        free_pidint_list(&prog_list);
        if (pat_data) free(pat_data);
        return 1;
      }

      // Still need more data for this PAT
      if (pat_data_len > pat_data_used)
        continue;

      // Free any earlier program list we'd read, now we've got a new one
      free_pidint_list(&prog_list);

      err = extract_prog_list_from_pat(verbose,pat_data,pat_data_len,&prog_list);
      if (err)
      {
        fprint_err("### Error extracting program list from PAT in TS"
                   " packet at " OFFSET_T_FORMAT "\n",
                   tsreader->posn - TS_PACKET_SIZE);
        free_pidint_list(&prog_list);
        if (pat_data) free(pat_data);
        return 1;
      }

      if (pat_data) free(pat_data);
      pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
    }
    else if (pid_in_pidint_list(prog_list,pid))
    {
      // We don't cope with interleaved PMT's with different PIDs
      if (unfinished_pmt_pid != 0 && pid != unfinished_pmt_pid)
      {
        // We're already part way through a PMT packet, but it's not
        // the same PMT as the one in this TS packet
        if (payload_unit_start_indicator)
        {
          // This is the start (and maybe also the end) of a new PMT,
          // so let's read this one
          // - actually, we don't need to do anything here, as our
          // data will get "thrown away" further down
        }
        else
        {
          // This is the continuation of another PMT - let's ignore
          // it for now and hope we'll find the rest of the one we're
          // still waiting to finish
          fprint_err("!!! Discarding partial PMT with PID %04x in TS"
                     " packet at " OFFSET_T_FORMAT ", already building PMT with PID %04x\n",
                     unfinished_pmt_pid,
                     tsreader->posn - TS_PACKET_SIZE,pid);
          continue;
        }
      }

      if (payload_unit_start_indicator && pmt_data)
      {
        // Lose any data we started but didn't complete
        free(pmt_data);
        pmt_data = NULL; pmt_data_len = 0; pmt_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pmt_data)
      {
        fprint_err("!!! Discarding partial (unstarted) PMT in TS"
                   " packet at " OFFSET_T_FORMAT "\n",
                   tsreader->posn - TS_PACKET_SIZE);
        continue;
      }

      err = build_psi_data(verbose,payload,payload_len,pid,
                           &pmt_data,&pmt_data_len,&pmt_data_used);
      if (err)
      {
        fprint_err("### Error %s PMT in TS packet at " OFFSET_T_FORMAT "\n",
                   (payload_unit_start_indicator?"starting new":"continuing"),
                   tsreader->posn - TS_PACKET_SIZE);
        free_pidint_list(&prog_list);
        free_pmt(&pmt);
        if (pmt_data) free(pmt_data);
        return 1;
      }

      // Still need more data for this PMT
      if (pmt_data_len > pmt_data_used)
      {
        unfinished_pmt_pid = pid;
        continue;
      }

      // Free any earlier PMT data we'd read, now we've got a new one
      free_pmt(&pmt);

      // Which isn't unfinished anymore
      unfinished_pmt_pid = 0;

      err = extract_pmt(verbose,pmt_data,pmt_data_len,pid,&pmt);
      if (err)
      {
        fprint_err("### Error extracting stream list from PMT in TS"
                   " packet at " OFFSET_T_FORMAT "\n",
                   tsreader->posn - TS_PACKET_SIZE);
        free_pidint_list(&prog_list);
        free_pmt(&pmt);
        if (pmt_data) free(pmt_data);
        return err;
      }

      if (pmt_data) free(pmt_data);
      pmt_data = NULL; pmt_data_len = 0; pmt_data_used = 0;
#if 0
      print_msg("PMT data read as:\n");
      report_pmt(TRUE,"  ",pmt);
      print_msg("\n");
#endif
    }
    else if (verbose)
    {
      pmt_stream_p  stream = pid_stream_in_pmt(pmt,pid);
      int stream_type;
      if (stream == NULL)
        stream_type = -1;
      else
        stream_type = stream->stream_type;
      report_payload(show_data,stream_type,payload,payload_len,
                     payload_unit_start_indicator);
      if (!show_data && payload_unit_start_indicator)
      {
        print_data(TRUE,"  Data",payload,payload_len,20);
      }
#if 0   // XXX
        print_end_of_data("      ",payload,payload_len,20);
#endif
    }
  }
  fprint_msg("Read %d TS packet%s\n",count,(count==1?"":"s"));
  free_pidint_list(&prog_list);
  free_pmt(&pmt);
  if (pmt_data) free(pmt_data);
  return 0;
}

/*
 * Report on TS packets with a particular PID in the given file
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int report_single_pid(TS_reader_p  tsreader,
                             int          max,
                             int          quiet,
                             uint32_t      just_pid)
{
  int       err;
  int       count = 0;
  int       pid_count = 0;

  for (;;)
  {
    uint32_t pid;
    int     payload_unit_start_indicator;
    byte   *adapt, *payload;
    int     adapt_len, payload_len;

    if (max > 0 && pid_count >= max)
    {
      fprint_msg("Stopping after %d packets with PID %0x\n",max,just_pid);
      break;
    }

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
      break;
    else if (err)
    {
      fprint_err("### Error reading TS packet %d at " OFFSET_T_FORMAT
                 "\n",count,tsreader->posn - TS_PACKET_SIZE);
      return 1;
    }

    count ++;

    if (pid != just_pid)
      continue;

    pid_count ++;

    if (!quiet)
    {
      fprint_msg(OFFSET_T_FORMAT_8 ": TS Packet %2d PID %04x%s\n",
                 tsreader->posn - TS_PACKET_SIZE,count,pid,
                 (payload_unit_start_indicator?" [pusi]":""));

      if (adapt_len > 0)
        print_data(TRUE,"    Adapt",adapt,adapt_len,adapt_len);
      print_data(TRUE,  "  Payload",payload,payload_len,payload_len);
    }
  }
  fprint_msg("Read %d TS packet%s, %d with PID %0x\n",
             count,(count==1?"":"s"),pid_count,just_pid);
  return 0;
}

static void print_usage()
{
  print_msg(
    "Usage: tsreport [switches] [<infile>] [switches]\n"
    "\n"
    );
  REPORT_VERSION("tsreport");
  print_msg(
    "\n"
    "  Report on one of the following for the given Transport Stream:\n"
    "\n"
    "  * The number of TS packets.\n"
    "  * PCR and PTS/DTS differences (-buffering).\n"
    "  * The packets of a single PID (-justpid).\n"
    "\n"
    "  When conflicting switches are specified, the last takes effect.\n"
    "\n"
    "Input:\n"
    "  <infile>          Read data from the named H.222 Transport Stream file\n"
    "  -stdin            Read data from standard input\n"
    "\n"
    "Normal operation:\n"
    "  By default, normal operation just reports the number of TS packets.\n"
    "  -timing, -t       Report timing information based on the PCRs.\n"
    "  -data             Show TS packet/payload data as bytes\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -verbose, -v      Also output (fairly detailed) information on each TS packet.\n"
    "  -quiet, -q        Only output summary information (this is the default)\n"
    "  -max <n>, -m <n>  Maximum number of TS packets to read\n"
    "\n"
    "Buffering information:\n"
    "  -buffering, -b    Report on the differences between PCR and PTS, and\n"
    "                    between PCR and DTS. This is relevant to the size of\n"
    "                    buffers needed in the decoder.  Also reports bitrates;\n"
    "                    the max bitrate is calculated over 0.5sec\n"
    "  -o <file>         Output CSV data for -buffering to the named file.\n"
    "  -32               Truncate 33 bit values in the CSV output to 32 bits\n"
    "                    (losing the top bit).\n"
    "  -verbose, -v      Output PCR/PTS/DTS information as it is found (in a\n"
    "                    format similar to that used for -o)\n"
    "  -quiet, -q        Output less information (notably, not the PMT)\n"
    "  -cnt <pid>,       Check values of continuity_counter in the specified PID.\n"
    "                    Writes all the values of the counter to a file called\n"
    "                    'continuity_counter.txt'. Turns buffering on (-b).\n"
    "  -max <n>, -m <n>  Maximum number of TS packets to read\n"
    "  -prog <n>         Report on program <n> [default = 1]\n"
    "                    (hopefully default will be 'all' in the future)\n"
    "\n"
    "Single PID:\n"
    "  -justpid <pid>    Just show data (file offset, index, adaptation field\n"
    "                    and payload) for TS packets with the given PID.\n"
    "                    PID 0 is allowed (i.e., the PAT).\n"
    "  -verbose, -v      Is ignored\n"
    "  -quiet, -q        Is ignored\n"
    "  -max <n>, -m <n>  Maximum number of TS packets of that PID to read\n"
    "\n"
    "Experimental control of timestamp formats (this doesn't affect the output\n"
    "to the CVS file, produced with -o):\n"
    "  -tfmt <thing>     Specify format of time differences.\n"
    "  -tafmt <thing>    Specify format of absolute times.\n"
    "\n"
    "  <thing> is (currently, but may change) one of:\n"
    "      90            Default -- show as 90KHz timestamps (suffix 't' on\n"
    "                    the values: e.g., 4362599t).\n"
    "      27            Show as 27MHz timestamps (similar, e.g., 25151:000t).\n"
    "      32            Show as 90KHz timestamps, but only the low 32 bits.\n"
    "      ms            Show as milliseconds.\n"
    "      hms           Show as hours/minutes/seconds (H:MM:SS.ssss, the H\n"
    "                    can be more than one digit if necessary)\n"
    );
}

int main(int argc, char **argv)
{
  int    use_stdin = FALSE;
  char  *input_name = NULL;
  int    had_input_name = FALSE;

  TS_reader_p  tsreader = NULL;

  int       max     = 0;     // The maximum number of TS packets to read (or 0)
  int       verbose = FALSE; // True => output diagnostic/progress messages
  int       quiet   = FALSE;
  int       report_timing  = FALSE;
  int       report_buffering = FALSE;
  int       show_data = FALSE;
  char     *output_name = NULL;
  uint32_t  continuity_cnt_pid = INVALID_PID;
  int       req_prog_no = 1;

  uint64_t  report_mask = ~0;   // report as many bits as we get

  int       select_pid = FALSE;
  uint32_t  just_pid = 0;

  int    err = 0;
  int    ii = 1;

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-h",argv[ii]) ||
          !strcmp("-help",argv[ii]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        verbose = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### tsreport: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-timing",argv[ii]) || !strcmp("-t",argv[ii]))
      {
        report_timing = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-buffering",argv[ii]) || !strcmp("-b",argv[ii]))
      {
        report_buffering = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-o",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        output_name = argv[ii+1];
        ii ++;
      }
      else if (!strcmp("-cnt",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        err = unsigned_value("tsreport",argv[ii],argv[ii+1],10,&continuity_cnt_pid);
        if (err) return 1;
        fprint_msg("Reporting on continuity_counter for pid = %04x (%u)\n",
                   continuity_cnt_pid,continuity_cnt_pid);
        report_buffering = TRUE;
        quiet = FALSE;
        ii ++;
      }
      else if (!strcmp("-data",argv[ii]))
      {
        show_data = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-32",argv[ii]))
      {
        report_mask = 0xFFFFFFFF;       // i.e., bottom 32 bits only
      }
      else if (!strcmp("-tfmt",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        if ((tfmt_diff = fmtx_str_to_timestamp_flags(argv[ii + 1])) < 0)
        {
          fprint_msg("### tsreport: Bad timestamp format '%s'\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-tafmt",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        if ((tfmt_abs = fmtx_str_to_timestamp_flags(argv[ii + 1])) < 0)
        {
          fprint_msg("### tsreport: Bad timestamp format '%s'\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-justpid",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        err = unsigned_value("tsreport",argv[ii],argv[ii+1],0,&just_pid);
        if (err) return 1;
        select_pid = TRUE;
        ii++;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        verbose = FALSE;
        quiet = TRUE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        err = int_value("tsreport",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        use_stdin = TRUE;
        had_input_name = TRUE;  // so to speak
      }
      else if (!strcmp("-prog",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        err = int_value("tsreport",argv[ii],argv[ii+1],TRUE,10,&req_prog_no);
        if (err) return 1;
        ii++;
      }
      else
      {
        fprint_err("### tsreport: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### tsreport: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
      else
      {
        input_name = argv[ii];
        had_input_name = TRUE;
      }
    }
    ii++;
  }

  if (!had_input_name)
  {
    print_err("### tsreport: No input file specified\n");
    return 1;
  }

  err = open_file_for_TS_read((use_stdin?NULL:input_name),&tsreader);
  if (err)
  {
    fprint_err("### tsreport: Unable to open input file %s for reading TS\n",
               use_stdin?"<stdin>":input_name);
    return 1;
  }
  fprint_msg("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  if (max)
    fprint_msg("Stopping after %d TS packets\n",max);

  if (select_pid)
    err = report_single_pid(tsreader,max,quiet,just_pid);
  else if (report_buffering)
    err = report_buffering_stats(tsreader,req_prog_no,max,verbose,quiet,
                                 output_name,continuity_cnt_pid,report_mask);
  else
    err = report_ts(tsreader,max,verbose,show_data,report_timing);
  if (err)
  {
    print_err("### tsreport: Error reporting on input stream\n");
    (void) close_TS_reader(&tsreader);
    return 1;
  }
  err = close_TS_reader(&tsreader);
  if (err) return 1;

  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

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

struct stream_data {
  uint32_t       pid;
  int           stream_type;
  int           had_a_pts;
  int           had_a_dts;
  int           last_cc;
  int           cc_dup_count;

  uint64_t       first_pts;
  uint64_t       first_dts;

  // Keep these in our datastructure so we can easily report the last
  // PTS/DTS in the file, when we're finishing up
  uint64_t       pts;
  uint64_t       dts;

  int           err_pts_lt_dts;
  int           err_dts_lt_prev_dts;
  int           err_dts_lt_pcr;
  int           err_cc_error;
  int           err_cc_dup_error;

  struct diff_from_pcr        pcr_pts_diff;
  struct diff_from_pcr        pcr_dts_diff;

  int           pts_ne_dts;
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

/*
 * Report on the given file
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int report_buffering_stats(TS_reader_p  tsreader,
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
  int           pmt_at = 0;     // in case we don't look for a PMT
  int           index;
  int           ii;

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
    stats[ii].last_cc = -1;
  }
  predict.min_pcr_error = LONG_MAX;
  predict.max_pcr_error = LONG_MIN;

  if (output_name)
  {
    file = fopen(output_name,"w");
    if (file == NULL)
    {
      fprintf(stderr,"### tsreport: Unable to open file %s: %s\n",
              output_name,strerror(errno));
      return 1;
    }
    printf("Writing CSV data to file %s\n",output_name);
    fprintf(file,"#TSoffset,calc|read,PCR/300,stream,audio|video,PTS,DTS,ESCR\n");
  }

  // First we need to determine what we're taking our data from.
  err = find_pmt(tsreader,max,FALSE,quiet,&pmt_at,&pmt);
  if (err) return 1;

  pcr_pid = pmt->PCR_pid;

  for (ii=0; ii<pmt->num_streams; ii++)
  {
    uint32_t pid = pmt->streams[ii].elementary_PID;
    if (ii >= MAX_NUM_STREAMS)
    {
      printf("!!! Found more than %d streams -- just reporting on the first %d found\n",
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

  printf("Looking at PCR PID %04x (%d)\n",pcr_pid,pcr_pid);
  for (ii=0; ii<num_streams; ii++)
    printf("  Stream %d: PID %04x (%d), %s\n",ii,stats[ii].pid,stats[ii].pid,
           h222_stream_type_str(stats[ii].stream_type));

  // Now do the actual work...
  start_count = count = pmt_at;
  start_posn  = posn  = tsreader->posn - TS_PACKET_SIZE;

  if (continuity_cnt_pid != INVALID_PID)
  {
    file_cnt = fopen("continuity_counter.txt","w"); //lorenzo
    if (file_cnt == NULL)
    {
      fprintf(stderr,"### tsreport: Unable to open file continuity_counter.txt\n");
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
      printf("Stopping after %d packets (PMT was at %d)\n",max,pmt_at);
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
      fprintf(stderr,"### Error reading TS packet %d at " OFFSET_T_FORMAT
              "\n",count,posn);
      return 1;
    }

    err = split_TS_packet(packet,&pid,
                          &payload_unit_start_indicator,
                          &adapt,&adapt_len,&payload,&payload_len);
    if (err)
    {
      fprintf(stderr,"### Error splitting TS packet %d at " OFFSET_T_FORMAT
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
        if (predict.know_pcr_rate)
        {
          // OK, so what we have predicted this PCR would be,
          // given the previous two PCRs and a linear rate?
          uint64_t guess_pcr = estimate_pcr(posn,predict.prev_pcr_posn,
                                           predict.prev_pcr,predict.pcr_rate);
          int64_t delta = adapt_pcr - guess_pcr;
          if (delta < predict.min_pcr_error)
            predict.min_pcr_error = delta;
          if (delta > predict.max_pcr_error)
            predict.max_pcr_error = delta;
        }

        if (verbose)
          printf(OFFSET_T_FORMAT_8 ": read PCR %s\n",
                 posn,
                 fmtx_timestamp(adapt_pcr, tfmt_abs | FMTX_TS_N_27MHz));
        if (file)
          fprintf(file,LLU_FORMAT ",read," LLU_FORMAT ",,,,\n",
                  posn,(adapt_pcr / (int64_t)300) & report_mask);

        if (predict.had_a_pcr)
        {
          if (predict.prev_pcr > adapt_pcr)
          {
            fprintf(stderr,"!!! PCR %s at TS packet "
                    OFFSET_T_FORMAT " is not more than previous PCR %s\n",
                    fmtx_timestamp(adapt_pcr, tfmt_abs | FMTX_TS_N_27MHz),
                    posn,
                    fmtx_timestamp(predict.prev_pcr, tfmt_abs | FMTX_TS_N_27MHz));
          }
          else
          {
            uint64_t delta_pcr = adapt_pcr - predict.prev_pcr;
            int delta_bytes = (int)(posn - predict.prev_pcr_posn);
            predict.pcr_rate = ((double)delta_bytes * 27.0 / (double)delta_pcr) * 1000000.0;
            predict.know_pcr_rate = TRUE;
#if 0   // XXX
            printf("PCR RATE = %f, DELTA_BYTES = %d, DELTA_PCR " LLU_FORMAT
                   ", PCR = " LLU_FORMAT "\n",
                   predict.pcr_rate,delta_bytes,delta_pcr,adapt_pcr);
#endif
          }
        }
        else
        {
          if (!quiet)
            printf("First PCR at " OFFSET_T_FORMAT "\n",posn);
          first_pcr = adapt_pcr;
          predict.had_a_pcr = TRUE;
        }
        predict.prev_pcr = adapt_pcr;
        predict.prev_pcr_posn = posn;
      }
    }   // end of working with a PCR PID packet
    // ========================================================================

    index = pid_index(stats,num_streams,pid);

    if (index != -1)
    {
      // Do continuity counter checking
      int cc = packet[3] & 15;

      // Log if required
      if (continuity_cnt_pid == pid)
        fprintf(file_cnt, "%d%c", cc, cc == 15 ? '\n' : ' ');

      if (stats[index].last_cc > 0)
      {
        // We are allowed 1 dup packet
        // *** Could check that it actually is a dup...
        if (stats[index].last_cc == cc)
        {
          if (stats[index].cc_dup_count++ != 0)
          {
            if (stats[index].err_cc_dup_error++ == 0)
            {
              if (continuity_cnt_pid == pid)
                fprintf(file_cnt, "[Duplicate error] ");
              printf("### PID(%d): Continuity Counter >1 duplicate %d at " OFFSET_T_FORMAT "\n",
                stats[index].pid, cc, posn);
            }
          }
        }
        else
        {
          // Otherwise CC must go up by 1 mod 16
          stats[index].cc_dup_count = 0;
          if (((stats[index].last_cc + 1) & 15) != cc)
          {
            if (stats[index].err_cc_error++ == 0)
            {
              if (continuity_cnt_pid == pid)
                fprintf(file_cnt, "[Discontinuity] ");
              printf("### PID(%d): Continuity Counter discontinuity %d->%d at " OFFSET_T_FORMAT "\n",
                stats[index].pid, stats[index].last_cc, cc, posn);
            }
          }
        }
      }
      stats[index].last_cc = cc;
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
        fprintf(stderr,"### Error looking for PTS/DTS in TS packet at "
                OFFSET_T_FORMAT "\n",posn);
        return 1;
      }

      if (got_dts && !got_pts)
      {
        fprintf(stderr,"### Got DTS but not PTS, in TS packet at "
                OFFSET_T_FORMAT "\n",posn);
        return 1;
      }

      if (!got_pts)
        continue;

      pcr_time_now_div300 = acc_pcr/300;

      // Do a few simple checks
      // For the sake of simplicity we ignore 33bit wrap...
      if (stats[index].pts < stats[index].dts)
      {
        if (stats[index].err_pts_lt_dts++ == 0)
          printf("### PID(%d): PTS (%s) < DTS (%s)\n",
            stats[index].pid,
            fmtx_timestamp(stats[index].pts, tfmt_abs),
            fmtx_timestamp(stats[index].dts, tfmt_abs));
      }
      if (stats[index].had_a_dts && stats[index].dts < last_dts)
      {
        if (stats[index].err_dts_lt_prev_dts++ == 0)
          printf("### PID(%d): DTS (%s) < previous DTS (%s)\n",
            stats[index].pid,
            fmtx_timestamp(stats[index].dts, tfmt_abs),
            fmtx_timestamp(last_dts, tfmt_abs));
      }
      if (stats[index].dts < pcr_time_now_div300)
      {
        if (stats[index].err_dts_lt_pcr++ == 0)
          printf("### PID(%d): DTS (%s) < PCR (%s)\n",
            stats[index].pid,
            fmtx_timestamp(stats[index].dts, tfmt_abs),
            fmtx_timestamp(acc_pcr, tfmt_abs | FMTX_TS_N_27MHz));
      }

      if (!stats[index].had_a_pts)
      {
#if 0  // XXX Sometimes useful to know
        printf("  First stream %d PTS (after first PCR) at " OFFSET_T_FORMAT "\n",
               index,posn);
#endif
        stats[index].first_pts = stats[index].pts;
        stats[index].had_a_pts = TRUE;
      }
      if (got_dts && !stats[index].had_a_dts)
      {
#if 0  // XXX Sometimes useful to know
        printf("  First stream %d DTS (after first PCR) at " OFFSET_T_FORMAT "\n",
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
            printf("Found ESCR " LLU_FORMAT " at " OFFSET_T_FORMAT "\n",
                   escr,posn);
          fprintf(file,LLU_FORMAT,escr & report_mask);
        }
        fprintf(file,"\n");
      }

      if (verbose)
      {
        printf(OFFSET_T_FORMAT_8 ": %s PCR " LLU_FORMAT " %d %5s",
               posn,
               (pcr_pid == pid && got_pcr)?"    ":"calc",
               pcr_time_now_div300,
               index,
               IS_AUDIO_STREAM_TYPE(stats[index].stream_type)?"audio":
               IS_VIDEO_STREAM_TYPE(stats[index].stream_type)?"video":"");
      }

      difference = stats[index].pts - pcr_time_now_div300;
      if (verbose)
      {
        printf(" PTS " LLU_FORMAT,stats[index].pts);
        printf(" PTS-PCR ");
        printf(LLD_FORMAT, difference);
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
        difference = stats[index].dts - pcr_time_now_div300;
        if (verbose)
        {
          printf(" DTS " LLU_FORMAT,stats[index].dts);
          printf(" DTS-PCR ");
          printf(LLD_FORMAT, difference & report_mask);
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
        printf("\n");
    }
  }

  if (continuity_cnt_pid != INVALID_PID)
  {
    fprintf(file_cnt, "\n");
    fclose(file_cnt); //lorenzo
  }

  if (!quiet)
    printf("Last PCR at " OFFSET_T_FORMAT "\n",predict.prev_pcr_posn);
  printf("Read %d TS packet%s\n",count,(count==1?"":"s"));
  if (pmt) free_pmt(&pmt);
  if (file) fclose(file);

  printf("Linear PCR prediction errors: min=%s, max=%s\n",
         fmtx_timestamp(predict.min_pcr_error, tfmt_diff),
         fmtx_timestamp(predict.max_pcr_error, tfmt_diff));

  if (!stats[0].had_a_pts && !stats[1].had_a_pts &&
      !stats[0].had_a_dts && !stats[1].had_a_dts)
    printf("\n"
           "No PTS or DTS values found\n");

  for (ii = 0; ii < num_streams; ii++)
  {
    printf("\nStream %d: PID %04x (%d), %s\n",ii,stats[ii].pid,stats[ii].pid,
           h222_stream_type_str(stats[ii].stream_type));
    if (stats[ii].pcr_pts_diff.num > 0)
    {
      printf("  PCR/%s:\n    Minimum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
             stats[ii].pts_ne_dts ? "PTS" : "PTS,DTS",
             fmtx_timestamp(stats[ii].pcr_pts_diff.min, tfmt_diff),
             fmtx_timestamp(stats[ii].pcr_pts_diff.min_at, tfmt_abs),
             stats[ii].pcr_pts_diff.min_posn);
      printf("    Maximum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
             fmtx_timestamp(stats[ii].pcr_pts_diff.max, tfmt_diff),
             fmtx_timestamp(stats[ii].pcr_pts_diff.max_at, tfmt_abs),
             stats[ii].pcr_pts_diff.max_posn);
      printf("    i.e., a span of %s\n",
             fmtx_timestamp(stats[ii].pcr_pts_diff.max - stats[ii].pcr_pts_diff.min, tfmt_diff));
      printf("    Mean difference (of %u) is %s\n",
             stats[ii].pcr_pts_diff.num,
             fmtx_timestamp((int64_t)(stats[ii].pcr_pts_diff.sum/(double)stats[ii].pcr_pts_diff.num), tfmt_diff));
    }

    if (stats[ii].pcr_dts_diff.num > 0 && stats[ii].pts_ne_dts)
    {
      printf("  PCR/DTS:\n    Minimum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
             fmtx_timestamp(stats[ii].pcr_dts_diff.min, tfmt_diff),
             fmtx_timestamp(stats[ii].pcr_dts_diff.min_at, tfmt_abs),
             stats[ii].pcr_dts_diff.min_posn);
      printf("    Maximum difference was %6s at DTS %8s, TS packet at " OFFSET_T_FORMAT_8 "\n",
             fmtx_timestamp(stats[ii].pcr_dts_diff.max, tfmt_diff),
             fmtx_timestamp(stats[ii].pcr_dts_diff.max_at, tfmt_abs),
             stats[ii].pcr_dts_diff.max_posn);
      printf("    i.e., a span of %s\n",
             fmtx_timestamp(stats[ii].pcr_dts_diff.max - stats[ii].pcr_dts_diff.min, tfmt_diff));
      printf("    Mean difference (of %u) is %s\n",
             stats[ii].pcr_dts_diff.num,
             fmtx_timestamp((int64_t)(stats[ii].pcr_dts_diff.sum/(double)stats[ii].pcr_dts_diff.num), tfmt_diff));
    }

    printf("  First PCR %8s, last %8s\n",
             fmtx_timestamp(first_pcr, tfmt_abs | FMTX_TS_N_27MHz),
             fmtx_timestamp(predict.prev_pcr, tfmt_abs | FMTX_TS_N_27MHz));
    if (stats[ii].pcr_pts_diff.num > 0)
      printf("  First PTS %8s, last %8s\n",
             fmtx_timestamp(stats[ii].first_pts, tfmt_abs),
             fmtx_timestamp(stats[ii].pts, tfmt_abs));
    if (stats[ii].pcr_dts_diff.num > 0)
      printf("  First DTS %8s, last %8s\n",
             fmtx_timestamp(stats[ii].first_dts, tfmt_abs),
             fmtx_timestamp(stats[ii].dts, tfmt_abs));

    if (stats[ii].err_cc_error != 0)
      printf("  ### CC error * %d\n", stats[ii].err_cc_error);
    if (stats[ii].err_cc_dup_error != 0)
      printf("  ### CC duplicate error * %d\n", stats[ii].err_cc_dup_error);
    if (stats[ii].err_pts_lt_dts != 0)
      printf("  ### PTS < DTS * %d\n", stats[ii].err_pts_lt_dts);
    if (stats[ii].err_dts_lt_prev_dts != 0)
      printf("  ### DTS < prev DTS * %d\n", stats[ii].err_dts_lt_prev_dts);
    if (stats[ii].err_dts_lt_pcr != 0)
      printf("  ### DTS < PCR * %d\n", stats[ii].err_dts_lt_pcr);
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
      printf("Stopping after %d packets\n",max);
      break;
    }

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
      break;
    else if (err)
    {
      fprintf(stderr,"### Error reading TS packet %d at " OFFSET_T_FORMAT
              "\n",count,tsreader->posn - TS_PACKET_SIZE);
      free_pidint_list(&prog_list);
      if (pmt_data) free(pmt_data);
      return 1;
    }

    count ++;

    if (verbose)
      printf(OFFSET_T_FORMAT_8 ": TS Packet %2d PID %04x%s",
             tsreader->posn - TS_PACKET_SIZE,count,pid,
             (payload_unit_start_indicator?" [pusi]":""));

    // Report on what we may
    if (verbose)
    {
      if (pid == 0x1fff)
        printf(" PADDING - ignored\n");
      else if (pid == 0x0000)
        printf(" PAT\n");
      else if (pid == 0x0001)
        printf(" Conditional Access Table - ignored\n");
      else if (pid >= 0x0002 && pid <= 0x000F)
        printf(" RESERVED - ignored\n");
      else if (pid_in_pidint_list(prog_list,pid))
        printf(" PMT\n");
      else if (pid_in_pmt(pmt,pid))
      {
        pmt_stream_p  stream = pid_stream_in_pmt(pmt,pid);
        if (stream == NULL)
        {
          fprintf(stderr,"### Internal error: stream for PID %0x returned NULL"
                  " in PMT\n",pid);
          report_pmt(stderr,"    ",pmt);
          free_pidint_list(&prog_list);
          free_pmt(&pmt);
          if (pmt_data) free(pmt_data);
          return 1;
        }
        printf(" stream type %02x (%s)\n",
               stream->stream_type,h222_stream_type_str(stream->stream_type));
      }
      else
        printf(" stream type not identified\n");
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
        fprintf(stderr,"!!! Discarding partial (unstarted) PAT in TS"
                " packet at " OFFSET_T_FORMAT "\n",
                tsreader->posn - TS_PACKET_SIZE);
        continue;
      }

      err = build_psi_data(verbose,payload,payload_len,pid,
                           &pat_data,&pat_data_len,&pat_data_used);
      if (err)
      {
        fprintf(stderr,"### Error %s PAT in TS packet at " OFFSET_T_FORMAT "\n",
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
        fprintf(stderr,"### Error extracting program list from PAT in TS"
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
          fprintf(stderr,"!!! Discarding partial PMT with PID %04x in TS"
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
        fprintf(stderr,"!!! Discarding partial (unstarted) PMT in TS"
                " packet at " OFFSET_T_FORMAT "\n",
                tsreader->posn - TS_PACKET_SIZE);
        continue;
      }

      err = build_psi_data(verbose,payload,payload_len,pid,
                           &pmt_data,&pmt_data_len,&pmt_data_used);
      if (err)
      {
        fprintf(stderr,"### Error %s PMT in TS packet at " OFFSET_T_FORMAT "\n",
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
        fprintf(stderr,"### Error extracting stream list from PMT in TS"
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
      printf("PMT data read as:\n");
      report_pmt(stdout,"  ",pmt);
      printf("\n");
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
        print_data(stdout,"  Data",payload,payload_len,20);
      }
#if 0   // XXX
        print_end_of_data(stdout,"      ",payload,payload_len,20);
#endif
    }
  }
  printf("Read %d TS packet%s\n",count,(count==1?"":"s"));
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
      printf("Stopping after %d packets with PID %0x\n",max,just_pid);
      break;
    }

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
      break;
    else if (err)
    {
      fprintf(stderr,"### Error reading TS packet %d at " OFFSET_T_FORMAT
              "\n",count,tsreader->posn - TS_PACKET_SIZE);
      return 1;
    }

    count ++;

    if (pid != just_pid)
      continue;

    pid_count ++;

    if (!quiet)
    {
      printf(OFFSET_T_FORMAT_8 ": TS Packet %2d PID %04x%s\n",
             tsreader->posn - TS_PACKET_SIZE,count,pid,
             (payload_unit_start_indicator?" [pusi]":""));

      if (adapt_len > 0)
        print_data(stdout,"    Adapt",adapt,adapt_len,adapt_len);
      print_data(stdout,  "  Payload",payload,payload_len,payload_len);
    }
  }
  printf("Read %d TS packet%s, %d with PID %0x\n",
         count,(count==1?"":"s"),pid_count,just_pid);
  return 0;
}

static void print_usage()
{
  printf(
    "Usage: tsreport [switches] [<infile>] [switches]\n"
    "\n"
    );
  REPORT_VERSION("tsreport");
  printf(
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
    "  -verbose, -v      Also output (fairly detailed) information on each TS packet.\n"
    "  -quiet, -q        Only output summary information (this is the default)\n"
    "  -max <n>, -m <n>  Maximum number of TS packets to read\n"
    "\n"
    "Buffering information:\n"
    "  -buffering, -b    Report on the differences between PCR and PTS, and\n"
    "                    between PCR and DTS. This is relevant to the size of\n"
    "                    buffers needed in the decoder.\n"
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
        output_name = argv[ii+1];
        ii ++;
      }
      else if (!strcmp("-cnt",argv[ii]))
      {
        err = unsigned_value("tsreport",argv[ii],argv[ii+1],10,&continuity_cnt_pid);
        if (err) return 1;
        printf("Reporting on continuity_counter for pid = %04x (%u)\n",
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
          printf("### tsreport: Bad timestamp format '%s'\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-tafmt",argv[ii]))
      {
        CHECKARG("tsreport",ii);
        if ((tfmt_abs = fmtx_str_to_timestamp_flags(argv[ii + 1])) < 0)
        {
          printf("### tsreport: Bad timestamp format '%s'\n",argv[ii+1]);
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
      else
      {
        fprintf(stderr,"### tsreport: "
                "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprintf(stderr,"### tsreport: Unexpected '%s'\n",argv[ii]);
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
    fprintf(stderr,"### tsreport: No input file specified\n");
    return 1;
  }

  err = open_file_for_TS_read((use_stdin?NULL:input_name),&tsreader);
  if (err)
  {
    fprintf(stderr,
            "### tsreport: Unable to open input file %s for reading TS\n",
            use_stdin?"<stdin>":input_name);
    return 1;
  }
  printf("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  if (max)
    printf("Stopping after %d TS packets\n",max);

  if (select_pid)
    err = report_single_pid(tsreader,max,quiet,just_pid);
  else if (report_buffering)
    err = report_buffering_stats(tsreader,max,verbose,quiet,
                                 output_name,continuity_cnt_pid,report_mask);
  else
    err = report_ts(tsreader,max,verbose,show_data,report_timing);
  if (err)
  {
    fprintf(stderr,"### tsreport: Error reporting on input stream\n");
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

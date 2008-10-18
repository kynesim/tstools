/*
 * Play (stream) TS packets.
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
#include <math.h>

#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include <time.h>       // Sleeping and timing

#include "compat.h"
#include "ts_fns.h"
#include "ps_fns.h"
#include "pes_fns.h"
#include "misc_fns.h"
#include "tswrite_fns.h"
#include "pidint_fns.h"
#include "version.h"

// If not being quiet, report progress every REPORT_EVERY packets read
#define REPORT_EVERY 10000


// ============================================================
// Common TS packet reading code
// ============================================================
/*
 * Read the next TS packet, coping with looping, etc.
 *
 * - `tsreader` is the TS reader context
 * - `count` is a running count of TS packets read from this input
 * - `data` is a pointer to the data for the packet
 * - `pid` is the PID of the TS packet
 * - `got_PCR` is TRUE if the adaptation field of this packet contains a PCR
 * - `pcr` is then the PCR value itself
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable) - i.e., rewind to `start_posn` and start again if
 *   `count` reaches `max` (obviously only if `max` is greater than zero).
 * - `start_count` is the value `count` should have after we've looped back
 *   to `start_posn`
 * - if `quiet` is true, then only error messages should be written out
 *
 * Returns 0 if all went well, 1 if something went wrong, EOF if `loop` is
 * false and either EOF was read, or `max` TS packets were read.
 */
static int read_TS_packet(TS_reader_p  tsreader,
                          uint32_t    *count,
                          byte        *data[TS_PACKET_SIZE],
                          uint32_t    *pid,
                          int         *got_pcr,
                          uint64_t    *pcr,
                          int          max,
                          int          loop,
                          offset_t     start_posn,
                          uint32_t     start_count,
                          int          quiet)
{
  int     err;
  int     payload_unit_start_indicator;
  byte   *adapt;
  int     adapt_len;
  byte   *payload;
  int     payload_len;

  if (max > 0 && (*count) >= (uint32_t)max)
  {
    if (loop)
    {
      if (!quiet)
        printf("Read %d packets, rewinding and continuing\n",max);
      err = seek_using_TS_reader(tsreader,start_posn);
      if (err) return 1;
      *count = start_count;
    }
    else
    {
      if (!quiet) printf("Stopping after %d TS packets\n",max);
      return EOF;
    }
  }

  // Read the next packet
  err = read_next_TS_packet(tsreader,data);
  if (err)
  {
    if (err == EOF)
    {
      if (!loop)
        return EOF;
      if (!quiet)
        printf("EOF (after %d TS packets), rewinding and continuing\n",
               *count);
    }
    else
    {
      fprintf(stderr,"### Error reading TS packet %d\n",*count);
      if (!loop)
        return 1;
      if (!quiet)
        printf("!!! Rewinding and continuing anyway\n");
    }
    err = seek_using_TS_reader(tsreader,start_posn);
    if (err) return 1;
    *count = start_count;
  }

  err = split_TS_packet(*data,pid,&payload_unit_start_indicator,
                        &adapt,&adapt_len,&payload,&payload_len);
  if (err)
  {
    fprintf(stderr,"### Error splitting TS packet %d\n",*count);
    return 1;
  }

  get_PCR_from_adaptation_field(adapt,adapt_len,got_pcr,pcr);

  (*count) ++;
  return 0;
}

/*
 * Read TS packets until we have found the PCR PID for our program stream,
 * outputting packets (without using their PCR) as we go.
 *
 * - `tsreader` is the TS reader context
 * - `tswriter` is our (buffered) writer
 * - `pcr_pid` is the PID containing PCRs as indicated by the PMT
 * - `num_read` is how many TS packets we read
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `quiet` is true, then only error messages should be written out
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int find_PCR_PID(TS_reader_p  tsreader,
                        TS_writer_p  tswriter,
                        uint32_t    *pcr_pid,
                        uint32_t    *num_read,
                        int          max,
                        int          quiet)
{
  int     err;
  int     count = 0;
  byte   *data;
  uint32_t pid;
  int     payload_unit_start_indicator;
  byte   *adapt;
  int     adapt_len;
  byte   *payload;
  int     payload_len;
  int     got_PAT = FALSE;

  pidint_list_p  prog_list = NULL;
  pmt_p          pmt = NULL;
  uint32_t       pmt_pid = 0;  // safe initial value

  byte  *pat_data = NULL;
  int    pat_data_len = 0;
  int    pat_data_used = 0;

  byte  *pmt_data = NULL;
  int    pmt_data_len = 0;
  int    pmt_data_used = 0;

  for (;;)
  {
    err = read_next_TS_packet(tsreader,&data);
    if (err == EOF)
    {
      fprintf(stderr,"### EOF (after %d TS packets), before finding program"
              " information\n",count);
      if (pmt_data) free(pmt_data);
      return 1;
    }
    else if (err)
    {
      fprintf(stderr,"### Error reading TS packet %d\n",count+1);
      if (pmt_data) free(pmt_data);
      return 1;
    }
    count++;

    err = split_TS_packet(data,&pid,&payload_unit_start_indicator,
                          &adapt,&adapt_len,&payload,&payload_len);
    if (err)
    {
      fprintf(stderr,"### Error splitting TS packet %d\n",count);
      if (pmt_data) free(pmt_data);
      return 1;
    }

    // Whatever we've found, don't forget to write it out via the
    // circular buffer (and we *know* it doesn't have a PCR that is
    // useful to us, as yet)
    err = tswrite_write(tswriter,data,pid,FALSE,0);
    if (err)
    {
      fprintf(stderr,"### Error writing TS packet %d to circular buffer\n",
              count);
      if (pmt_data) free(pmt_data);
      return 1;
    }

    if (pid == 0x0000)
    {
      if (!quiet) printf("Packet %d is PAT\n",count);
      if (payload_unit_start_indicator && pat_data)
      {
        // This is the start of a new PAT packet, but we'd already
        // started one, so throw its data away
        fprintf(stderr,"!!! Discarding previous (uncompleted) PAT data\n");
        free(pat_data);
        pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pat_data)
      {
        // This is the continuation of a PAT packet, but we hadn't
        // started one yet
        fprintf(stderr,"!!! Discarding PAT continuation, no PAT started\n");
        continue;
      }

      err = build_psi_data(FALSE,payload,payload_len,pid,
                           &pat_data,&pat_data_len,&pat_data_used);
      if (err)
      {
        fprintf(stderr,"### Error %s PAT\n",
                (payload_unit_start_indicator?"starting new":"continuing"));
        if (pat_data) free(pat_data);
        return 1;
      }

      // Do we need more data to complete this PAT?
      if (pat_data_len > pat_data_used)
        continue;

      err = extract_prog_list_from_pat(FALSE,pat_data,pat_data_len,&prog_list);
      if (err != 0)
      {
        free(pat_data);
        return err;
      }
      if (!quiet)
        report_pidint_list(prog_list,"Program list","Program",FALSE);

      if (prog_list->length > 1 && !quiet)
        printf("Multiple programs in PAT - using the first\n\n");

      pmt_pid = prog_list->pid[0];
      got_PAT = TRUE;
      free_pidint_list(&prog_list);
      free(pat_data);
      pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
    }
    else if (got_PAT && pid == pmt_pid)
    {
      if (!quiet)
        printf("Packet %d %s PMT with PID %04x\n",
               count, payload_unit_start_indicator?"starts":"continues",
               pmt_pid);

      if (payload_unit_start_indicator && pmt_data)
      {
        // This is the start of a new PMT packet, but we'd already
        // started one, so throw its data away
        fprintf(stderr,"!!! Discarding previous (uncompleted) PMT data\n");
        free(pmt_data);
        pmt_data = NULL; pmt_data_len = 0; pmt_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pmt_data)
      {
        // This is the continuation of a PMT packet, but we hadn't
        // started one yet
        fprintf(stderr,"!!! Discarding PMT continuation, no PMT started\n");
        continue;
      }

      err = build_psi_data(FALSE,payload,payload_len,pid,
                           &pmt_data,&pmt_data_len,&pmt_data_used);
      if (err)
      {
        fprintf(stderr,"### Error %s PMT\n",
                (payload_unit_start_indicator?"starting new":"continuing"));
        if (pmt_data) free(pmt_data);
        return 1;
      }

      // Do we need more data to complete this PMT?
      if (pmt_data_len > pmt_data_used)
        continue;

      err = extract_pmt(FALSE,pmt_data,pmt_data_len,pmt_pid,&pmt);
      free(pmt_data);
      if (err) return err;

      if (!quiet)
        report_pmt(stdout,"  ",pmt);
      *pcr_pid = pmt->PCR_pid;
      free_pmt(&pmt);
      if (!quiet)
        printf("Taking timing information from PID 0x%03x\n",*pcr_pid);
      *num_read = count;
      return 0;
    }

    if (max > 0 && count >= max)
    {
      fprintf(stderr,"### Stopping after %d TS packets, before finding program"
              " information\n",max);
      if (pmt_data) free(pmt_data);
      return 1;
    }
  }
}

// ============================================================
// Play the TS data
// ============================================================

/*
 * Read TS packets and then output them, using the buffered approach
 * so that we read-ahead to get the next PCR, and thus have reliable
 * timing information.
 *
 * Assumes (strongly) that it is starting from the start of the file.
 *
 * - `tsreader` is the TS reader context
 * - `tswriter` is our (maybe buffered) writer
 * - if `pid_to_ignore` is non-zero, then any TS packets with that PID
 *   will not be written out (note: any PCR information in them may still
 *   be used)
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `quiet` is true, then only error messages should be written out
 * - if `verbose` is true, then give extra progress messages
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int play_buffered_TS_packets(TS_reader_p  tsreader,
                                    TS_writer_p  tswriter,
                                    uint32_t     pid_to_ignore,
                                    int          max,
                                    int          loop,
                                    int          quiet,
                                    int          verbose)
{
  int  err;
  int  total = 0;
  uint32_t count = 0;
  uint32_t pcr_pid;
  uint32_t   start_count = 0;  // which TS packet to loop from
  offset_t   start_posn = 0;

  // These are only used in the loop below, but the compiler grumbles if
  // they're uninitialised (it isn't sure if they're being set by the call
  // to read_buffered_TS_packet() or not). I don't want to have to keep
  // thinking about the compiler warning, but I also know that these values
  // *will* be set by the function, so I don't want them reinitialised
  // every time round the loop. So hoist them back up to here...
  byte    *data = NULL;
  uint32_t pid = 0;
  uint64_t pcr = 0;

  // Before we can use PCRs for timing, we need to read a PMT which tells us
  // what our video stream is (so we can get our PCRs therefrom).
  err = find_PCR_PID(tsreader,tswriter,&pcr_pid,&start_count,max,quiet);
  if (err)
  {
    fprintf(stderr,
            "### Unable to find PCR PID for timing information\n"
            "    Looked in first %d TS packets\n",max);
    return 1;
  }

  // Once we've found that, we're ready to play our data
  prime_read_buffered_TS_packet(pcr_pid);

  // If we're looping, remember the location of the first packet of (probable)
  // data - there's not much point rewinding before that point
  if (loop)
    start_posn = start_count * TS_PACKET_SIZE;

  count = start_count;
  for (;;)
  {
    err = read_buffered_TS_packet(tsreader,&count,&data,&pid,&pcr,
                                  max,loop,start_posn,start_count,quiet);
    if (err == EOF)  // shouldn't occur if `loop`
      break;
    else if (err)
    {
      if (tsreader->file != STDIN_FILENO)
      {
        fprintf(stderr,"### Last TS packet read was at " LLU_FORMAT "\n",
                (uint64_t)count * TS_PACKET_SIZE);
      }
      return 1;
    }
    total ++;

    // If we've been asked to ignore this packet, we should be able to
    // just ignore it -- since all TS packets have their time associated
    // with them, we shouldn't need to send a "dummy" packet, just in
    // case it had time on it.
    if (pid_to_ignore != 0 && pid == pid_to_ignore)
      continue;

    // And write it out via the circular buffer
    err = tswrite_write(tswriter,data,pid,TRUE,pcr);
    if (err)
    {
      fprintf(stderr,"### Error writing TS packet %d to circular buffer\n",
              count);
      return 1;
    }

    if (!quiet && verbose && total%REPORT_EVERY == 0)
      printf("Transferred %d TS packets\n",total);
  }

  if (!quiet)
    printf("Transferred %d TS packet%s in total\n",total,(total==1?"":"s"));
  return 0;
}

/*
 * Read TS packets and then output them.
 *
 * Assumes (strongly) that it is starting from the start of the file.
 *
 * - `tsreader` is the TS reader context
 * - `tswriter` is our (maybe buffered) writer
 * - if `pid_to_ignore` is non-zero, then any TS packets with that PID
 *   will not be written out (note: any PCR information in them may still
 *   be used)
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `quiet` is true, then only error messages should be written out
 * - if `verbose` is true, then give extra progress messages
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int play_TS_packets(TS_reader_p  tsreader,
                           TS_writer_p  tswriter,
                           uint32_t     pid_to_ignore,
                           int          max,
                           int          loop,
                           int          quiet,
                           int          verbose)
{
  int  err;
  int  total = 0;
  uint32_t count = 0;
  int  pcrs_used = 0;
  int  pcrs_ignored = 0;
  uint32_t pcr_pid;
  uint32_t   start_count = 0;  // which TS packet to loop from
  offset_t   start_posn = 0;

  // Before we can use PCRs for timing, we need to read a PMT which tells us
  // what our video stream is (so we can get our PCRs therefrom).
  err = find_PCR_PID(tsreader,tswriter,&pcr_pid,&start_count,max,quiet);
  if (err)
  {
    fprintf(stderr,
            "### Unable to find PCR PID for timing information\n"
            "    Looked in first %d TS packets\n",max);
    return 1;
  }

  // Once we've found that, we're ready to play our data

  // If we're looping, remember the location of the first packet of (probable)
  // data - there's not much point rewinding before that point
  if (loop)
    start_posn = start_count * TS_PACKET_SIZE;

  count = start_count;
  for (;;)
  {
    byte    *data;
    uint32_t pid;
    int      got_pcr;
    uint64_t pcr;

    err = read_TS_packet(tsreader,&count,&data,&pid,&got_pcr,&pcr,
                         max,loop,start_posn,start_count,quiet);
    if (err == EOF)  // shouldn't occur if `loop`
      break;
    else if (err)
    {
      if (tsreader->file != STDIN_FILENO)
      {
        fprintf(stderr,"### Last TS packet read was at " LLU_FORMAT "\n",
                (uint64_t)count * TS_PACKET_SIZE);
      }
      return 1;
    }

    total ++;

    // We are only interested in timing information from our PCR PID stream
    if (got_pcr)
    {
      if (pid == pcr_pid)
        pcrs_used ++;
      else
      {
        pcrs_ignored ++;
        got_pcr = FALSE;
      }
    }

    if (pid_to_ignore != 0 && pid == pid_to_ignore)
    {
      // We want to "transmit" this packet, since that's the simplest
      // way of sending its timing information (if any) to the writer.
      // However, we don't want to *actually* send meaningful data.
      // The simplest thing is to ignore it if it doesn't have a PCR:
      // and otherwise, change it to a null packet, by resetting its PID.
      if (!got_pcr)
        continue;
      else
      {
        data[2]  = 0xFF;
        data[1] |= 0x1F;
      }
    }

    // And write it out via the circular buffer
    err = tswrite_write(tswriter,data,pid,got_pcr,pcr);
    if (err)
    {
      fprintf(stderr,"### Error writing TS packet %d to circular buffer\n",
              count);
      return 1;
    }

    if (!quiet && verbose && total%REPORT_EVERY == 0)
      printf("Transferred %d TS packets\n",total);
  }

  if (!quiet)
  {
    printf("Transferred %d TS packet%s in total\n",total,(total==1?"":"s"));
    printf("Used PCRs from %d packets, ignored PCRs from %d packets\n",
           pcrs_used,pcrs_ignored);
  }
  return 0;
}

/*
 * Read TS packets and then output them.
 *
 * Assumes (strongly) that it is starting from the start of the file.
 *
 * - `input` is the input stream (descriptor) to read
 * - `tswriter` is our (maybe buffered) writer
 * - if `pid_to_ignore` is non-zero, then any TS packets with that PID
 *   will not be written out (note: any PCR information in them may still
 *   be used)
 * - if `scan_for_PCRs`, use a read-ahead buffer to find the *next* PCR,
 *   and thus allow exact timing of packets.
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `quiet` is true, then only error messages should be written out
 * - if `verbose` is true, then give extra progress messages
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int play_TS_stream(int         input,
                          TS_writer_p tswriter,
                          uint32_t    pid_to_ignore,
                          int         scan_for_PCRs,
                          int         max,
                          int         loop,
                          int         quiet,
                          int         verbose)
{
  int  err;
  TS_reader_p  tsreader;

  err = build_TS_reader(input,&tsreader);
  if (err) return 1;

  if (scan_for_PCRs)
    err = play_buffered_TS_packets(tsreader,tswriter,pid_to_ignore,
                                   max,loop,quiet,verbose);
  else
    err = play_TS_packets(tsreader,tswriter,pid_to_ignore,
                          max,loop,quiet,verbose);
  if (err)
  {
    free_TS_reader(&tsreader);
    return 1;
  }

  free_TS_reader(&tsreader);
  return 0;
}

// ============================================================
// Play the PS data
// ============================================================
/*
 * Read PS packets and then output them as TS.
 *
 * - `input` is the program stream
 * - `output` is the transport stream
 * - `pad_start` is the number of filler TS packets to start the output
 *   with.
 * - `program_repeat` is how often (after how many PS packs) to repeat
 *   the program information (PAT/PMT)
 * - `want_h264` should be true to indicate that the video stream is H.264
 *   (ISO/IEC 14496-2, MPEG-4/AVC), false if it is H.262 (ISO/IEC 13818-3,
 *   MPEG-2, or indeed 11172-3, MPEG-1)
 * - `input_is_dvd` indicates if the PS data came from a DVD, and thus follows
 *   its conventions for private_stream_1 and AC-3/DTS/etc. substreams
 * - `video_stream` indicates which video stream we want - i.e., the stream
 *   with id 0xE0 + <video_stream> - and -1 means the first video stream found.
 * - `audio_stream` indicates which audio stream we want. If `want_ac3_audio`
 *   is false, then this will be the stream with id 0xC0 + <audio_stream>, or,
 *   if it is -1, the first audio stream found.
 * - if `want_ac3_audio` is true, then if `is_dvd` is true, then we want
 *   audio from private_stream_1 (0xBD) with substream id <audio_stream>,
 *   otherwise we ignore `audio_stream` and assume that all data in
 *   private_stream_1 is the audio we want.
 * - `want_dolby_as_dvb` indicates if any Dolby (AC-3) audio data should be output
 *   with DVB or ATSC stream type
 * - `pmt_pid` is the PID of the PMT to write
 * - `pcr_pid` is the PID of the TS unit containing the PCR
 * - `video_pid` is the PID for the video we write
 * - `keep_audio` is true if the audio stream should be output, false if
 *   it should be ignored
 * - `audio_pid` is the PID for the audio we write
 * - if `max` is non-zero, then we want to stop reading after we've read
 *   `max` packets
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `verbose` then we want to output diagnostic information
 *   (nb: only applies to first time if looping is enabled)
 * - if `quiet` then we want to be as quiet as we can
 *   (nb: only applies to first time if looping is enabled)
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int play_PS_stream(int          input,
                          TS_writer_p  output,
                          int          pad_start,
                          int          program_repeat,
                          int          force_stream_type,
                          int          want_h262,
                          int          input_is_dvd,
                          int          video_stream,
                          int          audio_stream,
                          int          want_ac3_audio,
                          int          want_dolby_as_dvb,
                          uint32_t     pmt_pid,
                          uint32_t     pcr_pid,
                          uint32_t     video_pid,
                          int          keep_audio,
                          uint32_t     audio_pid,
                          int          max,
                          int          loop,
                          int          verbose,
                          int          quiet)
{
  int  err;
  int  is_h264;
  PS_reader_p   ps;

  err = build_PS_reader(input,quiet,&ps);
  if (err)
  {
    fprintf(stderr,"### Error building PS reader for input\n");
    return 1;
  }

  if (force_stream_type)
  {
    is_h264 = !want_h262;
    if (!quiet)
      printf("Reading input as %s\n",(want_h262?"MPEG-2 (H.262)":
                             "MPEG-4/AVC (H.264)"));
  }
  else
  {
    err = determine_if_PS_is_h264(ps,&is_h264);
    if (err) return 1;

    if (!quiet)
      printf("Video appears to be %s\n",
             (is_h264?"MPEG-4/AVC (H.264)":"MPEG-2 (H.262)"));
  }

  err = ps_to_ts(ps,output,pad_start,program_repeat,
                 is_h264,input_is_dvd,
                 video_stream,audio_stream,want_ac3_audio,
                 want_dolby_as_dvb,pmt_pid,pcr_pid,
                 video_pid,keep_audio,audio_pid,max,verbose,quiet);

  if (err)
  {
    if (loop)
      fprintf(stderr,"!!! Ignoring error and looping\n");
    else
    {
      free_PS_reader(&ps);
      return 1;
    }
  }

  if (loop)
  {
    for (;;)
    {
      if (!quiet) printf("Rewinding and continuing\n");
      err = rewind_program_stream(ps);
      if (err)
      {
        fprintf(stderr,"### Error rewinding\n");
        free_PS_reader(&ps);
        return 1;
      }
      err = ps_to_ts(ps,output,pad_start,program_repeat,
                     is_h264,input_is_dvd,
                     video_stream,audio_stream,want_ac3_audio,
                     want_dolby_as_dvb,pmt_pid,pcr_pid,
                     video_pid,keep_audio,audio_pid,max,FALSE,TRUE);
      if (err)
      {
        if (loop)
          fprintf(stderr,"!!! Ignoring error and looping\n");
        else
        {
          free_PS_reader(&ps);
          return 1;
        }
      }
    }
  }
  return 0;
}

static void print_usage(int summary)
{
  printf(
    "Basic usage: tsplay  <infile>  <host>[:<port>]\n"
    "\n"
    );
  REPORT_VERSION("tsplay");
  if (summary)
    printf(
      "\n"
      "  Play the given file (containing Transport Stream or Program Stream\n"
      "  data) 'at' the nominated host, or to an output file. The output\n"
      "  is always Transport Stream.\n"
      );
  else
    printf(
      "\n"
      "  Reads from a file containing H.222.0 (ISO/IEC 13818-1) Transport\n"
      "  Stream or Program Stream data (converting PS to TS as it goes),\n"
      "  and 'plays' the Transport Stream 'at' the nominated host, or to an\n"
      "  output file.\n"
      "\n"
      "  Assumes a single program in the file, and for PS assumes that the\n"
      "  program stream is well formed - i.e., that it starts with a pack\n"
      "  header. A PS stream that ends after a PES packet, but without an\n"
      "  MPEG_program_end_code will cause a warning message, but will not\n"
      "  be treated as an error.\n"
      "\n"
      "  Output to a file is only provided for testing purposes, and does\n"
      "  not use the buffering/child process mechanisms (although this may\n"
      "  not be clear from the informative messages output).\n"
      "\n"
      "  Note that most switches can be placed anywhere on the command line.\n"
      );
  printf(
    "\n"
    "Input:\n"
    "  <infile>          Input is from the named H.222 TS file.\n"
    "\n"
    "Output:\n"
    "  <host>\n"
    "  <host>:<port>     Normally, output is to a named host.\n"
    "                    If <port> is not specified, it defaults to 88.\n"
    "                    Output defaults to UDP.\n"
    "  -output <name>\n"
    "  -o <name>         Output is to file <name>.\n"
    "\n"
    "  -tcp              Output to the host is via TCP.\n"
    "  -udp              Output to the host is via UDP (the default).\n"
    );
  if (summary)
    printf(
      "  -stdout           Output is to standard output. Forces -quiet.\n"
      );
  else
    printf(
      "  -stdout           Output is to standard output. This does not\n"
      "                    make sense with -tcp or -udp. This forces -quiet.\n"
      );
  printf(
    "\n"
    "  -mcastif <ipaddr>\n"
    "  -i <ipaddr>       If output is via UDP, and <host> is a multicast\n"
    "                    address, then <ipaddr> is the IP address of the\n"
    "                    network interface to use. This may not be supported\n"
    "                    on some versions of Windows.\n"
    "\n"
    "General Switches:\n"
    "  -quiet, -q        Only output error messages\n"
    "  -verbose, -v      Output progress messages\n"
    "  -help <subject>   Show help on a particular subject\n"
    "  -help             Summarise the <subject>s that can be specified\n"
    "\n"
    );
  if (summary)
    printf(
      "  -max <n>, -m <n>  Maximum number of TS/PS packets to read.\n"
      "                    See -details for more information.\n"
      "  -loop             Play the input file repeatedly. Can be combined\n"
      "                    with -max.\n"
      );
  else
    printf(
      "Normal operation outputs some messages summarising the command line\n"
      "choices, information about the circular buffer filling, and\n"
      "confirmation when the program is ending.\n"
      "Quiet operation endeavours only to output error messages.\n"
      "Verbose operation outputs a progress message every %d TS packets,\n"
      "and a note whenever the input file is rewound for -loop.\n"
      "\n"
      "  -loop             Play the input file repeatedly.\n"
      "\n"
      "This assumes that the file ends neatly - i.e., not with a partial\n"
      " TS packet. On the other hand, it *can* be combined with -max to\n"
      "allow for short video sequences to be repeated.\n"
      "\n"
      "If PS data is being read, and looping has been selected, then the\n"
      "verbosity flags only apply to the first time through the data, and\n"
      "thereafter it is as if -quiet had been specified.\n"
      "\n"
      "  -max <n>, -m <n>  Maximum number of TS packets to read\n"
      "                    (or PS packets if the input data is PS)\n"
      "\n"
      "Note that the -max value must be at least 7 * <buffer size>, since\n"
      "the parent process adds TS packets to the circular buffer 7 at a\n"
      "time, and the child process waits until the buffer has filled up\n"
      "before doing anything. In recognition of this, if the user specifies\n"
      "a smaller value, the software will exit with a complaint.\n"
      "\n"
      "(Strictly, if the input is PS, this restriction is not correct, as\n"
      "one PS packet generates multiple TS packets, and thus lower values\n"
      "for -max could be allowed. For simplicity, this is not considered.)\n"
      "",
      REPORT_EVERY);
}

static void print_help_help()
{
  printf(
    "With no switches, tsplay will give a brief summary of its basic usage.\n"
    "Otherwise:\n"
    "\n"
    "  -help detail[s]   Show an expanded version of the help you get if you\n"
    "                    run tsplay with no arguments\n"
    "  -help ts          Show help specific to playing TS data\n"
    "  -help ps          Show help specific to playing PS data\n"
    "  -help tuning      Show help about how to tune the program's operation\n"
    "  -help test[ing]   Show help on switches that can be useful in testing\n"
    "                    the target application (the video player)\n"
    "  -help debug[ging] Show help on debugging this application.\n"
    "  -help             Show this message\n"
    "  -help all         Show all available help (equivalent to each of the\n"
    "                    above specific helps, in order)\n"
    );
}

static void print_help_ts()
{
  printf(
    "Transport Stream Switches:\n"
    "The following switches are only applicable if the input data is TS.\n"
    "\n"
    "  -ignore <pid>     Any TS packets with this PID will not be output\n"
    "                    (more accurately, any TS packets with this PID and with\n"
    "                    PCR information will be transmitted with PID 0x1FFF, and\n"
    "                    any other packets with this PID will be ignored).\n"
    "                    The <pid> may not be 0, and should not be the PAT or PMT.\n"
    "\n"
    "Normally, the TS reading process remembers the last PCR, and also scans ahead\n"
    "for the next PCR. This allows it to calculate an accurate timestamp for each TS\n"
    "packet, at the cost of ignoring any packets before the first PCR, and also\n"
    "(at least in the currrent implementation), ignoring any TS packets after the\n"
    "last PCR, and also at the cost of some extra copying of data.\n"
    "\n"
    "However, sometimes it is more appropriate to revert to the mechanism used by\n"
    "earlier versions of tsplay:\n"
    "\n"
    "  -oldpace          Switch off scanning ahead for the next PCR.\n"
    "\n"
    "which attempts to predict an approximate PCR for each TS packet, based on an\n"
    "initial speed (see '-bitrate'/'-byterate' in '-help tuning') and the PCRs found\n"
    "earlier in the data stream. This works reasonably well for streams with a\n"
    "constant bitrate, but does not cope well if the bitrate varies greatly.\n"
    "\n"
    "Note that '-nopcrs' (see '-help tuning') also implies '-oldpace'.\n"
    );
}

static void print_help_ps()
{
  printf(
    "Program Stream Switches:\n"
    "The following switches are only applicable if the input data is PS.\n"
    "\n"
    "If input is from a file, then the program will look at the start of\n"
    "the file to determine if the stream is H.264 or H.262 data. This\n"
    "process may occasionally come to the wrong conclusion, in which case\n"
    "the user can override the choice:\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    "\n"
    "Program stream data from a DVD stores its audio differently\n"
    "(specifically, the DVD specification states that AC-3 (Dolby), DTS and\n"
    "other audio are stored in a specialised manner in private_stream_1):\n"
    "\n"
    "  -dvd              The PS data is from a DVD. This is the default.\n"
    "                    This switch has no effect on MPEG-1 PS data.\n"
    "  -notdvd, -nodvd   The PS data is not from a DVD.\n"
    "\n"
    "Specifying audio/video streams:\n"
    "\n"
    "  -vstream <n>      Take video from video stream <n> (0..7).\n"
    "                    The default is the first video stream found.\n"
    "  -astream <n>      Take audio from audio stream <n> (0..31).\n"
    "                    The default is the first audio stream found\n"
    "                    (this includes private_stream_1 on non-DVD streams).\n"
    "  -ac3stream <n>    Take audio from AC3 substream <n> (0..7), from\n"
    "                    private_stream_1. This implies -dvd.\n"
    "                    (If audio is being taken from a substream, the user\n"
    "                    is assumed to have determined which one is wanted,\n"
    "                    e.g., using psreport)\n"
    "\n"
    "  -vpid <pid>       <pid> is the video PID to use for the data.\n"
    "                    Use '-vpid 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x68.\n"
    "  -apid <pid>       <pid> is the audio PID to use for the data.\n"
    "                    Use '-apid 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x67.\n"
    "  -pmt <pid>        <pid> is the PMT PID to use.\n"
    "                    Use '-pmt 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x66\n"
    "\n"
    "The default values for the various PIDs should suffice in most cases\n"
    "\n"
    "If the audio stream being output is Dolby (AC-3), then the stream type\n"
    "used to output it differs for DVB (European) and ATSC (USA) data. It\n"
    "may be specified as follows:\n"
    "\n"
    "  -dolby dvb       Use stream type 0x06 (the default)\n"
    "  -dolby atsc      Use stream type 0x81\n"
    "\n"
    "  -prepeat <n>      Output the program data (PAT/PMT) after every <n>\n"
    "                    PS packs, to allow a TS reader to resynchronise\n"
    "                    if it starts reading part way through the stream.\n"
    "                    Defaults to 100.\n"
    "  -pad <n>          Pad the start of the output stream with <n> filler\n"
    "                    TS packets, to allow a TS reader time to byte align\n"
    "                    with the datastream before any significant data\n"
    "                    occurs. Defaults to 8.\n"
    "\n"
    );
}

static void print_help_tuning()
{
  tswrite_help_tuning();
}

static void print_help_testing()
{
  tswrite_help_testing();
  printf(
    "\n"
    "  -drop <k> <d>     As TS packets are output, for every <k>+<d> packets,\n"
    "                    keep <k> and then drop (throw away) <d>.\n"
    "                    This can be useful when testing other applications.\n"
    );
}

static void print_help_debugging()
{
  tswrite_help_debug();
}

int main(int argc, char **argv)
{
  TS_writer_p       tswriter;
  struct TS_context context;
  char  *input_name = NULL;
  int    had_input_name = FALSE;
  int    had_output_name = FALSE;
  int    input   = -1;
  int    max     = 0;     // The maximum number of TS packets to read (or 0)
  int    quiet   = FALSE;
  int    verbose = FALSE;
  int    err = 0;
  int    ii = 1;
  int    loop = FALSE;
  time_t start,end;
  int    is_TS;   // Does it appear to be TS or PS?

  // Values relevent to "opening" the output file/socket
  enum  TS_writer_type  how = TS_W_UNDEFINED;  // how to output our TS data
  char                 *output_name = NULL;    // the output filename/host
  int                   port = 88;             // the port to connect to
  int    use_network = FALSE;
  char *multicast_if = NULL;                   // IP address of multicast i/f

  int       scan_for_PCRs = TRUE;
  uint32_t  pid_to_ignore = 0;

  // Program Stream specific options
  uint32_t pmt_pid = 0x66;
  uint32_t video_pid = 0x68;
  uint32_t pcr_pid = video_pid;  // Use PCRs from the video stream
  uint32_t audio_pid = 0x67;
  int     repeat_program_every = 100;
  int     pad_start = 8;
  int     input_is_dvd = TRUE;

  int     video_stream = -1;
  int     audio_stream = -1;
  int     want_ac3_audio = FALSE;

  int     want_h262 = TRUE;
  int     force_stream_type = FALSE;
  int     want_dolby_as_dvb = TRUE;
  int     drop_packets = 0;
  int     drop_number  = 0;

  if (argc < 2)
  {
    print_usage(TRUE);
    return 0;
  }

  // Process the standard tswrite switches/arguments
  err = tswrite_process_args("tsplay",argc,argv,&context);
  if (err) return 1;

  // And process any remaining arguments...
  while (ii < argc)
  {
    // Ignore any arguments that have already been "eaten"
    if (!strcmp(argv[ii],TSWRITE_PROCESSED))
    {
      ii++;
      continue;
    }

    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-h",argv[ii]) ||
          !strcmp("-help",argv[ii]))
      {
        if (argc == (ii+1))
          print_help_help();
        else
        {
          if (!strcmp(argv[ii+1],"ps"))
            print_help_ps();
          else if (!strcmp(argv[ii+1],"ts"))
            print_help_ts();
          else if (!strcmp(argv[ii+1],"tuning"))
            print_help_tuning();
          else if (!strcmp(argv[ii+1],"test") || !strcmp(argv[ii+1],"testing"))
            print_help_testing();
          else if (!strcmp(argv[ii+1],"debug") || !strcmp(argv[ii+1],"debugging"))
            print_help_debugging();
          else if (!strcmp(argv[ii+1],"detail") || !strcmp(argv[ii+1],"details"))
            print_usage(FALSE);
          else if (!strcmp(argv[ii+1],"all"))
          {
            print_usage(FALSE);
            printf("\n");
            print_help_ts();
            printf("\n");
            print_help_ps();
            printf("\n");
            print_help_tuning();
            printf("\n");
            print_help_testing();
            printf("\n");
            print_help_debugging();
            printf("\n");
            print_help_help();
          }
          else
          {
            fprintf(stderr,"### tsplay: "
                    "Unrecognised command line switch '%s %s' -- try '-help'\n",
                    argv[ii],argv[ii+1]);
            return 1;
          }
        }
        return 0;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        quiet = TRUE;
        verbose = FALSE;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        quiet = FALSE;
        verbose = TRUE;
      }
      else if (!strcmp("-output",argv[ii]) || !strcmp("-o",argv[ii]))
      {
        had_output_name = TRUE;
        use_network = FALSE;
        how = TS_W_FILE;
        output_name = argv[ii+1];
        ii++;
      }
      else if (!strcmp("-mcastif",argv[ii]) || !strcmp("-i",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        multicast_if = argv[ii+1];
        ii++;
      }
      else if (!strcmp("-stdout",argv[ii]))
      {
        had_output_name = TRUE;  // more or less
        use_network = FALSE;
        how = TS_W_STDOUT;
        output_name = NULL;
        ii++;
      }
      else if (!strcmp("-tcp",argv[ii]))
      {
        if (how == TS_W_STDOUT || how == TS_W_FILE)
        {
          fprintf(stderr,
                  "### tsplay: -tcp does not make sense with file output\n");
          return 1;
        }
        use_network = TRUE;
        how = TS_W_TCP;
      }
      else if (!strcmp("-udp",argv[ii]))
      {
        if (how == TS_W_STDOUT || how == TS_W_FILE)
        {
          fprintf(stderr,
                  "### tsplay: -udp does not make sense with file output\n");
          return 1;
        }
        use_network = TRUE;
        how = TS_W_UDP;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-oldpace",argv[ii]))
      {
        scan_for_PCRs = FALSE;
      }
      else if (!strcmp("-loop",argv[ii]))
      {
        loop = TRUE;
      }
      else if (!strcmp("-avc",argv[ii]) || !strcmp("-h264",argv[ii]))
      {
        force_stream_type = TRUE;
        want_h262 = FALSE;
      }
      else if (!strcmp("-h262",argv[ii]))
      {
        force_stream_type = TRUE;
        want_h262 = TRUE;
      }
      else if (!strcmp("-dvd",argv[ii]))
      {
        input_is_dvd = TRUE;
      }
      else if (!strcmp("-notdvd",argv[ii]) || !strcmp("-nodvd",argv[ii]))
      {
        input_is_dvd = FALSE;
      }
      else if (!strcmp("-vstream",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value_in_range("ps2ts",argv[ii],argv[ii+1],0,0xF,0,
                                 &video_stream);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-astream",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value_in_range("ps2ts",argv[ii],argv[ii+1],0,0x1F,0,
                                 &audio_stream);
        if (err) return 1;
        want_ac3_audio = FALSE;
        ii++;
      }
      else if (!strcmp("-ac3stream",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value_in_range("ps2ts",argv[ii],argv[ii+1],0,0x7,0,
                                 &audio_stream);
        if (err) return 1;
        want_ac3_audio = TRUE;
        input_is_dvd = TRUE;
        ii++;
      }
      else if (!strcmp("-dolby",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        if (!strcmp("dvb",argv[ii+1]))
          want_dolby_as_dvb = TRUE;
        else if (!strcmp("atsc",argv[ii+1]))
          want_dolby_as_dvb = FALSE;
        else
        {
          fprintf(stderr,"### tsplay: -dolby must be followed by dvb or atsc\n");
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-prepeat",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,10,
                        &repeat_program_every);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pad",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,10,&pad_start);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-ignore",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&pid_to_ignore);
        if (err) return 1;
        if (pid_to_ignore == 0)
        {
          fprintf(stderr,"### tsplay: -ignore 0 is not allowed\n");
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-vpid",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&video_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-apid",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&audio_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pmt",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&pmt_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-drop",argv[ii]))
      {
        if (ii+2 >= argc)
        {
          fprintf(stderr,"### tsplay: missing argument(s) to -drop\n");
          return 1;
        }
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,0,&drop_packets);
        if (err) return 1;
        err = int_value("tsplay",argv[ii],argv[ii+2],TRUE,0,&drop_number);
        if (err) return 1;
        ii += 2;
      }
      else
      {
        fprintf(stderr,"### tsplay: "
                "Unrecognised command line switch '%s' -- try '-help'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (!had_input_name)
      {
        input_name = argv[ii];
        had_input_name = TRUE;
      }
      else if (!had_output_name)
      {
        // This is presumably the host to write to
        err = host_value("tsplay",NULL,argv[ii],&output_name,&port);
        if (err) return 1;
        had_output_name = TRUE;
        use_network = TRUE;
        if (how == TS_W_UNDEFINED)
          how = TS_W_UDP;
      }
      else
      {
        fprintf(stderr,"### tsplay: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
    }
    ii++;
  }

  if (!had_input_name)
  {
    fprintf(stderr,"### tsplay: No input file specified\n");
    return 1;
  }

  // We *need* some output...
  if (!had_output_name)
  {
    fprintf(stderr,"### tsplay: No output file or host specified\n");
    return 1;
  }

  // On the other hand, it can be nice to have a *string* for <stdout>
  if (how == TS_W_STDOUT)
    output_name = "<stdout>";

  // Try to stop extraneous data ending up in our output stream
  if (how == TS_W_STDOUT)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  // This is an important check
  if (max > 0 && how == TS_W_UDP && (max / 7) < context.circ_buf_size)
  {
    fprintf(stderr,"### tsplay: -max %d cannot work with -buffer %d"
            " - max must be at least %d",max,
            context.circ_buf_size,context.circ_buf_size*7);
    if (max/7 > 0)
      fprintf(stderr,",\n            or buffer size reduced to %d",max/7);
    fprintf(stderr,"\n");
    return 1;
  }

  // If tswrite found '-nopcrs' in the switches, make sure that we've
  // switched PCR lookahead off.
  if (!context.use_pcrs)
    scan_for_PCRs = FALSE;

  input = open_binary_file(input_name,FALSE);
  if (input == -1)
  {
    fprintf(stderr,"### tsplay: Unable to open input file %s\n",input_name);
    return 1;
  }
  if (!quiet)
    printf("Reading from  %s%s\n",input_name,(loop?" (and looping)":""));

  err = determine_if_TS_file(input,&is_TS);
  if (err)
  {
    fprintf(stderr,"### tsplay: Cannot play file %s\n",output_name);
    (void) close_file(input);
    return 1;
  }

  err = tswrite_open(how,output_name,multicast_if,port,quiet,
                     &tswriter);
  if (err)
  {
    fprintf(stderr,"### tsplay: Cannot open/connect to %s\n",output_name);
    (void) close_file(input);
    return 1;
  }

  if (!quiet)
  {
    if (is_TS)
    {
      printf("Input appears to be Transport Stream\n");
      if (scan_for_PCRs)
        printf("Using 'exact' TS packet timing (by looking-ahead to the next PCR)\n");
      else
        printf("Approximating/predicting intermediate PCRs\n");
      if (pid_to_ignore)
        printf("Ignoring PID %04x (%d)\n",pid_to_ignore,pid_to_ignore);
    }
    else
    {
      printf("Input appears to be Program Stream\n");
      if (input_is_dvd)
        printf("Treating input as from DVD\n");
      else
        printf("Treating input as NOT from DVD\n");
    }
    if (max)
      printf("Stopping after at most %d packets\n",max);

    if (how == TS_W_UDP)
      tswrite_report_args(&context);
  }

  if (drop_packets)
  {
    if (!quiet) printf("DROPPING: Keeping %d TS packet%s, then dropping (throwing away) %d\n",
                       drop_packets,(drop_packets==1?"":"s"),drop_number);
    tswriter->drop_packets = drop_packets;
    tswriter->drop_number = drop_number;
  }

  if (!quiet)
    start = time(NULL);

  // We can only use buffered output for TCP/IP and UDP
  // (it doesn't make much sense for output to a file)
  if (how == TS_W_UDP)
  {
    err = tswrite_start_buffering_from_context(tswriter,&context);
    if (err)
    {
      fprintf(stderr,"### tsplay: Error setting up buffering\n");
      (void) close_file(input);
      (void) tswrite_close(tswriter,TRUE);
      return 1;
    }
  }

  if (is_TS)
  {
    err = play_TS_stream(input,tswriter,pid_to_ignore,scan_for_PCRs,
                         max,loop,quiet,verbose);
  }
  else
    err = play_PS_stream(input,tswriter,pad_start,
                         repeat_program_every,force_stream_type,
                         want_h262,input_is_dvd,
                         video_stream,audio_stream,want_ac3_audio,
                         want_dolby_as_dvb,pmt_pid,pcr_pid,
                         video_pid,TRUE,audio_pid,max,loop,
                         verbose,quiet);
  if (err)
  {
    fprintf(stderr,"### tsplay: Error playing stream\n");
    (void) close_file(input);
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }

  if (!quiet)
  {
    end = time(NULL);
    printf("Started  output at %s",ctime(&start));
    printf("Finished output at %s",ctime(&end));
    printf("Elapsed time %.1fs\n",difftime(end,start));
  }
  
  err = close_file(input);
  if (err)
  {
    fprintf(stderr,"### tsplay: Error closing input file %s\n",input_name);
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }
  err = tswrite_close(tswriter,quiet);
  if (err)
  {
    fprintf(stderr,"### tsplay: Error closing output to %s\n",output_name);
    return 1;
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

/*
 * This is the core functionality used by tsplay to play (stream) TS packets.
 *
 * It is abstracted here so that it can be used in other contexts.
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
#include "printing_fns.h"
#include "ts_fns.h"
#include "ps_fns.h"
#include "pes_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "tsplay_fns.h"
#include "tswrite_fns.h"
#include "pidint_fns.h"


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
        fprint_msg("Read %d packets, rewinding and continuing\n",max);
      err = seek_using_TS_reader(tsreader,start_posn);
      if (err) return 1;
      *count = start_count;
    }
    else
    {
      if (!quiet) fprint_msg("Stopping after %d TS packets\n",max);
      return EOF;
    }
  }

  // Read the next packet
  while ((err = read_next_TS_packet(tsreader,data)) != 0)
  {
    if (err == EOF)
    {
      if (!loop)
        return EOF;
      if (!quiet)
        fprint_msg("EOF (after %d TS packets), rewinding and continuing\n",
                   *count);
    }
    else
    {
      fprint_err("### Error reading TS packet %d\n",*count);
      if (!loop)
        return 1;
      if (!quiet)
        print_msg("!!! Rewinding and continuing anyway\n");
    }
    err = seek_using_TS_reader(tsreader,start_posn);
    if (err) return 1;
    *count = start_count;
  }

  err = split_TS_packet(*data,pid,&payload_unit_start_indicator,
                        &adapt,&adapt_len,&payload,&payload_len);
  if (err)
  {
    fprint_err("### Error splitting TS packet %d\n",*count);
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
  int    pmt_program_number = -1;

  for (;;)
  {
    err = read_next_TS_packet(tsreader,&data);
    if (err == EOF)
    {
      fprint_err("### EOF (after %d TS packets), before finding program"
                 " information\n",count);
      if (pmt_data) free(pmt_data);
      return 1;
    }
    else if (err)
    {
      fprint_err("### Error reading TS packet %d\n",count+1);
      if (pmt_data) free(pmt_data);
      return 1;
    }
    count++;

    err = split_TS_packet(data,&pid,&payload_unit_start_indicator,
                          &adapt,&adapt_len,&payload,&payload_len);
    if (err)
    {
      fprint_err("### Error splitting TS packet %d\n",count);
      if (pmt_data) free(pmt_data);
      return 1;
    }

    // Whatever we've found, don't forget to write it out via the
    // circular buffer (and we *know* it doesn't have a PCR that is
    // useful to us, as yet)
    err = tswrite_write(tswriter,data,pid,FALSE,0);
    if (err)
    {
      fprint_err("### Error writing TS packet %d to circular buffer\n",
                 count);
      if (pmt_data) free(pmt_data);
      return 1;
    }

    if (pid == 0x0000 && !got_PAT)
    {
      if (!quiet) fprint_msg("Packet %d is PAT\n",count);
      if (payload_unit_start_indicator && pat_data)
      {
        // This is the start of a new PAT packet, but we'd already
        // started one, so throw its data away
        print_err("!!! Discarding previous (uncompleted) PAT data\n");
        free(pat_data);
        pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pat_data)
      {
        // This is the continuation of a PAT packet, but we hadn't
        // started one yet
        print_err("!!! Discarding PAT continuation, no PAT started\n");
        continue;
      }

      err = build_psi_data(FALSE,payload,payload_len,pid,
                           &pat_data,&pat_data_len,&pat_data_used);
      if (err)
      {
        fprint_err("### Error %s PAT\n",
                   (payload_unit_start_indicator?"starting new":"continuing"));
        if (pat_data) free(pat_data);
        continue;
      }

      // Do we need more data to complete this PAT?
      if (pat_data_len > pat_data_used)
        continue;

      err = extract_prog_list_from_pat(FALSE,pat_data,pat_data_len,&prog_list);
      if (err != 0)
      {
        free(pat_data);
        continue;
      }
      if (!quiet)
        report_pidint_list(prog_list,"Program list","Program",FALSE);

      if (prog_list->length > 1 && !quiet)
        print_msg("Multiple programs in PAT - using the first\n\n");

      pmt_pid = prog_list->pid[0];
      pmt_program_number = prog_list->number[0];

      got_PAT = TRUE;
      free_pidint_list(&prog_list);
      free(pat_data);
      pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
    }
    else if (got_PAT && pid == pmt_pid)
    {
      if (!quiet)
        fprint_msg("Packet %d %s PMT with PID %04x\n",
                   count, payload_unit_start_indicator?"starts":"continues",
                   pmt_pid);

      if (payload_unit_start_indicator && pmt_data)
      {
        // This is the start of a new PMT packet, but we'd already
        // started one, so throw its data away
        print_err("!!! Discarding previous (uncompleted) PMT data\n");
        free(pmt_data);
        pmt_data = NULL; pmt_data_len = 0; pmt_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pmt_data)
      {
        // This is the continuation of a PMT packet, but we hadn't
        // started one yet
        print_err("!!! Discarding PMT continuation, no PMT started\n");
        continue;
      }

      err = build_psi_data(FALSE,payload,payload_len,pid,
                           &pmt_data,&pmt_data_len,&pmt_data_used);
      if (err)
      {
        fprint_err("### Error %s PMT\n",
                   (payload_unit_start_indicator?"starting new":"continuing"));
        if (pmt_data) free(pmt_data);
        return 1;
      }

      // Do we need more data to complete this PMT?
      if (pmt_data_len > pmt_data_used)
        continue;

      err = extract_pmt(FALSE,pmt_data,pmt_data_len,pmt_pid,&pmt);
      free(pmt_data);
      pmt_data = NULL;
      if (err) return err;

      if (pmt->program_number != pmt_program_number)
      {
        if (!quiet)
          fprint_msg("Discarding PMT program %d - looking for %d\n",
                     pmt->program_number, pmt_program_number);
        free_pmt(&pmt);
        continue;
      }

      if (!quiet)
        report_pmt(TRUE,"  ",pmt);
      *pcr_pid = pmt->PCR_pid;
      free_pmt(&pmt);
      if (!quiet)
        fprint_msg("Taking timing information from PID 0x%03x\n",*pcr_pid);
      *num_read = count;
      return 0;
    }

    if (max > 0 && count >= max)
    {
      fprint_err("### Stopping after %d TS packets, before finding program"
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
 * - if `override_pcr_pid` is non-zero, then it is the PID to use for PCRs,
 *   ignoring any value found in a PMT
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
                                    uint32_t     override_pcr_pid,
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
  if (override_pcr_pid)
  {
    pcr_pid = override_pcr_pid;
    if (!quiet)
      fprint_msg("Forcing use of PCR PID 0x%03x (%d)\n",pcr_pid,pcr_pid);
  }
  else
  {
    err = find_PCR_PID(tsreader,tswriter,&pcr_pid,&start_count,max,quiet);
    if (err)
    {
      fprint_err("### Unable to find PCR PID for timing information\n"
                 "    Looked in first %d TS packets\n",max);
      return 1;
    }
  }

  // Once we've found that, we're ready to play our data
  err = prime_read_buffered_TS_packet(tsreader,pcr_pid);
  if (err) return 1;

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
        fprint_err("### Last TS packet read was at " LLU_FORMAT "\n",
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
      fprint_err("### Error writing TS packet %d to circular buffer\n",
                 count);
      return 1;
    }

    if (!quiet && verbose && total%TSPLAY_REPORT_EVERY == 0)
      fprint_msg("Transferred %d TS packets\n",total);
  }

  if (!quiet)
    fprint_msg("Transferred %d TS packet%s in total\n",total,(total==1?"":"s"));
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
                           const tsplay_output_pace_mode pace_mode,
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
  uint32_t pcr_pid = ~0U;
  uint32_t   start_count = 0;  // which TS packet to loop from
  offset_t   start_posn = 0;

  if (pace_mode == TSPLAY_OUTPUT_PACE_PCR2_PMT)
  {
    // Before we can use PCRs for timing, we need to read a PMT which tells us
    // what our video stream is (so we can get our PCRs therefrom).
    err = find_PCR_PID(tsreader,tswriter,&pcr_pid,&start_count,max,quiet);
    if (err)
    {
      fprint_err("### Unable to find PCR PID for timing information\n"
                 "    Looked in first %d TS packets\n",max);
      return 1;
    }

    // Once we've found that, we're ready to play our data

    // If we're looping, remember the location of the first packet of (probable)
    // data - there's not much point rewinding before that point
    if (loop)
      start_posn = start_count * TS_PACKET_SIZE;
  }

  count = start_count;
  for (;;)
  {
    byte    *data;
    uint32_t pid;
    int      got_pcr;
    uint64_t pcr = 0;

    err = read_TS_packet(tsreader,&count,&data,&pid,&got_pcr,&pcr,
                         max,loop,start_posn,start_count,quiet);

    if (err == EOF)  // shouldn't occur if `loop`
      break;
    else if (err)
    {
      if (tsreader->file != STDIN_FILENO)
      {
        fprint_err("### Last TS packet read was at " LLU_FORMAT "\n",
                   (uint64_t)count * TS_PACKET_SIZE);
      }
      return 1;
    }

    if (count == start_count + 1)
      tswrite_discontinuity(tswriter);

    total ++;

    // We are only interested in timing information from our PCR PID stream
    if (got_pcr)
    {
      // If 1st PCR we see then remember its pid
      if (pcr_pid == ~0U)
      {
        fprint_msg("PCR PID set to 1st seen: %#x (%d)\n", pid, pid);
        pcr_pid = pid;
      }

      if (pid == pcr_pid)
        pcrs_used ++;
      else
      {
        if (pcrs_ignored == 0)
        {
          fprint_msg("Other PCR PIDs seen: %#x (%d)...\n", pid, pid);
        }
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
      fprint_err("### Error writing TS packet %d to circular buffer\n",
                 count);
      return 1;
    }

    if (!quiet && verbose && total%TSPLAY_REPORT_EVERY == 0)
      fprint_msg("Transferred %d TS packets\n",total);
  }

  if (!quiet)
  {
    fprint_msg("Transferred %d TS packet%s in total\n",total,(total==1?"":"s"));
    fprint_msg("Used PCRs from %d packets, ignored PCRs from %d packets\n",
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
 * - if we are using the PCR read-ahead buffer, and `override_pcr_pid` is
 *   non-zero, then it is the PID to use for PCRs, ignoring any value found in
 *   a PMT
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `quiet` is true, then only error messages should be written out
 * - if `verbose` is true, then give extra progress messages
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int play_TS_stream(int         input,
                          TS_writer_p tswriter,
                          const tsplay_output_pace_mode pace_mode,
                          uint32_t    pid_to_ignore,
                          uint32_t    override_pcr_pid,
                          int         max,
                          int         loop,
                          int         quiet,
                          int         verbose)
{
  int  err;
  TS_reader_p  tsreader;

  err = build_TS_reader(input,&tsreader);
  if (err) return 1;

  fprint_msg("pace_mode=%d\n", pace_mode);

  if (pace_mode == TSPLAY_OUTPUT_PACE_PCR1)
    err = play_buffered_TS_packets(tsreader,tswriter,pid_to_ignore,
                                   override_pcr_pid,max,loop,quiet,verbose);
  else
    err = play_TS_packets(tsreader, tswriter, pace_mode, pid_to_ignore,
                          max,loop,quiet,verbose);
  if (err)
  {
    free_TS_reader(&tsreader);
    return 1;
  }

  free_TS_reader(&tsreader);
  return 0;
}

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
extern int play_PS_stream(int          input,
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
    print_err("### Error building PS reader for input\n");
    return 1;
  }

  if (force_stream_type)
  {
    is_h264 = !want_h262;
    if (!quiet)
      fprint_msg("Reading input as %s\n",(want_h262?"MPEG-2 (H.262)":
                                          "MPEG-4/AVC (H.264)"));
  }
  else
  {
    err = determine_if_PS_is_h264(ps,&is_h264);
    if (err) return 1;

    if (!quiet)
      fprint_msg("Video appears to be %s\n",
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
      print_err("!!! Ignoring error and looping\n");
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
      if (!quiet) print_msg("Rewinding and continuing\n");
      err = rewind_program_stream(ps);
      if (err)
      {
        print_err("### Error rewinding\n");
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
          print_err("!!! Ignoring error and looping\n");
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

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

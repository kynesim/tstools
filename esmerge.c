/*
 * Merge a video ES and an audio ES to produce TS.
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

#include "compat.h"
#include "es_fns.h"
#include "accessunit_fns.h"
#include "avs_fns.h"
#include "audio_fns.h"
#include "ts_fns.h"
#include "tswrite_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"

// Default audio rates, in Hertz
#define CD_RATE  44100
#define DAT_RATE 48000

// Video frame rate (frames per second)
#define DEFAULT_VIDEO_FRAME_RATE  25

// Number of audio samples per frame
// For ADTS this will either be 1024 or 960. It's believed that it will
// actually, in practice, be 1024, and in fact the difference may not be
// significant enough to worry about for the moment.
#define ADTS_SAMPLES_PER_FRAME  1024
// For MPEG-1 audio layer 2, this is 1152
#define L2_SAMPLES_PER_FRAME    1152
// For AC-3 this is 256 * 6
#define AC3_SAMPLES_PER_FRAME   (256 * 6)

// ------------------------------------------------------------
#define TEST_PTS_DTS 0

#if TEST_PTS_DTS
#include "pes_fns.h"

static int check(uint64_t value)
{
  int      err;
  byte     data[5];
  uint64_t result;

  encode_pts_dts(data,2,value);
  err = decode_pts_dts(data,2,&result);
  if (err) return 1;

  if (value == result)
    fprint_msg("Value " LLU_FORMAT " OK\n",value);
  else
  {
    fprint_msg("Input " LLU_FORMAT ", output " LLU_FORMAT "\n",value,result);
    return 1;
  }

  return 0;

}

static int test_pts()
{
  if (check(0)) return 1;
  if (check(1)) return 1;
  if (check(2)) return 1;
  if (check(3)) return 1;
  if (check(4)) return 1;
  if (check(5)) return 1;
  if (check(6)) return 1;
  if (check(7)) return 1;
  if (check(8)) return 1;
  if (check(100)) return 1;
  if (check(10000)) return 1;
  if (check(1000000)) return 1;
  if (check(100000000)) return 1;
  if (check(10000000000LL)) return 1;
  if (check(1000000000000LL)) return 1;

  return 0;
}
#endif  // TEST_PTS_DTS

static int is_avs_I_frame(avs_frame_p  frame)
{
  return (frame->is_frame && frame->start_code == 0xB3);
}

static int is_I_or_IDR_frame(access_unit_p  frame)
{
  return (frame->primary_start != NULL &&
          frame->primary_start->nal_ref_idc != 0 &&
          (frame->primary_start->nal_unit_type == NAL_IDR ||
           all_slices_I(frame)));
}

/*
 * Merge the given elementary streams to the given output.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int merge_with_avs(avs_context_p  video_context,
                          int            audio_file,
                          TS_writer_p    output,
                          int            audio_type,
                          int            audio_samples_per_frame,
                          int            audio_sample_rate,
                          double         video_frame_rate,
                          int            pat_pmt_freq,
                          int            quiet,
                          int            verbose,
                          int            debugging)
{
  int  ii;
  int  err;
  uint32_t prog_pids[2];
  byte     prog_type[2];

  int video_frame_count = 0;
  int audio_frame_count = 0;

  uint32_t video_pts_increment = (uint32_t)(90000.0 / video_frame_rate);
  uint32_t audio_pts_increment = (90000 * audio_samples_per_frame) / audio_sample_rate;
  uint64_t video_pts = 0;
  uint64_t audio_pts = 0;

  // The "actual" times are just for information, so we aren't too worried
  // about accuracy - thus floating point should be OK.
  double audio_time = 0.0;
  double video_time = 0.0;

  int got_video = TRUE;
  int got_audio = TRUE;

  if (verbose)
    fprint_msg("Video PTS increment %u\n"
               "Audio PTS increment %u\n",video_pts_increment,audio_pts_increment);

  // Start off our output with some null packets - this is in case the
  // reader needs some time to work out its byte alignment before it starts
  // looking for 0x47 bytes
  for (ii=0; ii<8; ii++)
  {
    err = write_TS_null_packet(output);
    if (err) return 1;
  }

  // Then write some program data
  // @@@ later on we might want to repeat this every so often
  prog_pids[0] = DEFAULT_VIDEO_PID;
  prog_pids[1] = DEFAULT_AUDIO_PID;
  prog_type[0] = AVS_VIDEO_STREAM_TYPE;

  switch (audio_type)
  {
  case AUDIO_ADTS:
  case AUDIO_ADTS_MPEG2:
  case AUDIO_ADTS_MPEG4:
    prog_type[1] = ADTS_AUDIO_STREAM_TYPE;
    break;
  case AUDIO_L2:
    prog_type[1] = MPEG2_AUDIO_STREAM_TYPE;
    break;
  case AUDIO_AC3:
    prog_type[1] = ATSC_DOLBY_AUDIO_STREAM_TYPE;
    break;
  default:              // what else can we do?
    prog_type[1] = ADTS_AUDIO_STREAM_TYPE;
    break;
  }
  err = write_TS_program_data2(output,
                               1, // transport stream id
                               1, // program number
                               DEFAULT_PMT_PID,
                               DEFAULT_VIDEO_PID,  // PCR pid
                               2,prog_pids,prog_type);
  if (err)
  {
    print_err("### Error writing out TS program data\n");
    return 1;
  }

  while (got_video || got_audio)
  {
    avs_frame_p    avs_frame;
    audio_frame_p  aframe;

    // Start with a video frame
    if (got_video)
    {
      err = get_next_avs_frame(video_context,quiet,debugging,&avs_frame);
      if (err == EOF)
      {
        if (verbose)
          print_msg("EOF: no more video data\n");
        got_video = FALSE;
      }
      else if (err)
        return 1;

      if (!avs_frame->is_frame)
      {
        // It's not actually a *picture*
        // If we can, update the video frame rate to what we're told
        if (avs_frame->is_sequence_header)
          video_frame_rate = avs_frame_rate(avs_frame->frame_rate_code);
        // And output the data right away
        err = write_avs_frame_as_TS(output,avs_frame,DEFAULT_VIDEO_PID);
        if (err)
        {
          free_avs_frame(&avs_frame);
          print_err("### Error writing AVS frame (sequence header/end)\n");
          return 1;
        }
        continue;               // look for a "proper" frame
      }
    }

    if (got_video)
    {
      video_time = video_frame_count / video_frame_rate;
      video_pts += video_pts_increment;
      video_frame_count ++;
      if (verbose)
        fprint_msg("\n%s video frame %5d (@ %.2fs, " LLU_FORMAT ")\n",
                   (is_avs_I_frame(avs_frame)?"**":"++"),
                   video_frame_count,video_time,video_pts);

      if (pat_pmt_freq && !(video_frame_count % pat_pmt_freq))
        {
          if (verbose)
            {
              fprint_msg("\nwriting PAT and PMT (frame = %d, freq = %d).. ", 
                         video_frame_count, pat_pmt_freq);
            }

          err = write_TS_program_data2(output, 
                                       1, // tsid
                                       1, // Program number
                                       DEFAULT_PMT_PID,
                                       DEFAULT_VIDEO_PID, // PCR pid
                                       2, prog_pids, prog_type);
        }

      // PCR counts frames as seen in the stream, so is easy
      // The presentation and decoding time for B frames (if we ever get any)
      // could reasonably be the same as the PCR.
      // The presentation and decoding time for I and IDR frames is unlikely to
      // be the same as the PCR (since frames come out later...), but it may
      // work to pretend the PTS is the PCR plus a delay time (for decoding)...

      // We could output the timing information every video frame,
      // but might as well only do it on index frames.

      // (Actually, we *could* work out the proper PTS for I frames, but it's
      // easier just to add a delay to allow for progress through the decoder)
      if (is_avs_I_frame(avs_frame))
        err = write_avs_frame_as_TS_with_pts_dts(avs_frame,
                                                 output,DEFAULT_VIDEO_PID,
                                                 TRUE,video_pts + 30000,
                                                 TRUE,video_pts);
      else
        err = write_avs_frame_as_TS_with_PCR(avs_frame,
                                             output,DEFAULT_VIDEO_PID,
                                             video_pts,0);
      if (err)
      {
        free_avs_frame(&avs_frame);
        print_err("### Error writing AVS frame\n");
        return 1;
      }
      free_avs_frame(&avs_frame);
    }

    if (!got_audio)
      continue;

    // Then output enough audio frames to make up to a similar time
    while (audio_pts < video_pts || !got_video)
    {
      err = read_next_audio_frame(audio_file,audio_type,&aframe);
      if (err == EOF)
      {
        if (verbose)
          print_msg("EOF: no more audio data\n");
        got_audio = FALSE;
        break;
      }
      else if (err)
        return 1;

      audio_time = audio_frame_count *
        audio_samples_per_frame / (double)audio_sample_rate;
      audio_pts += audio_pts_increment;
      audio_frame_count ++;
      if (verbose)
        fprint_msg("** audio frame %5d (@ %.2fs, " LLU_FORMAT ")\n",
                   audio_frame_count,audio_time,audio_pts);

      err = write_ES_as_TS_PES_packet_with_pts_dts(output,aframe->data,
                                                   aframe->data_len,
                                                   DEFAULT_AUDIO_PID,
                                                   DEFAULT_AUDIO_STREAM_ID,
                                                   TRUE,audio_pts,
                                                   TRUE,audio_pts);
      if (err)
      {
        free_audio_frame(&aframe);
        print_err("### Error writing audio frame\n");
        return 1;
      }
      free_audio_frame(&aframe);
    }    
  }

  if (!quiet)
  {
    uint32_t video_elapsed = (uint32_t)((double)(100*video_frame_count)/video_frame_rate);
    uint32_t audio_elapsed = 100*audio_frame_count*
      audio_samples_per_frame/audio_sample_rate;
    fprint_msg("Read %d video frame%s, %.2fs elapsed (%dm %.2fs)\n",
               video_frame_count,(video_frame_count==1?"":"s"),
               video_elapsed/100.0,video_elapsed/6000,(video_elapsed%6000)/100.0);
    fprint_msg("Read %d audio frame%s, %.2fs elapsed (%dm %.2fs)\n",
               audio_frame_count,(audio_frame_count==1?"":"s"),
               audio_elapsed/100.0,audio_elapsed/6000,(audio_elapsed%6000)/100.0);
  }

  return 0;

}

/*
 * Merge the given elementary streams to the given output.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int merge_with_h264(access_unit_context_p  video_context,
                           int                    audio_file,
                           TS_writer_p            output,
                           int                    audio_type,
                           int                    audio_samples_per_frame,
                           int                    audio_sample_rate,
                           int                    video_frame_rate,
                           int                    pat_pmt_freq,
                           int                    quiet,
                           int                    verbose,
                           int                    debugging)
{
  int  ii;
  int  err;
  uint32_t prog_pids[2];
  byte     prog_type[2];

  int video_frame_count = 0;
  int audio_frame_count = 0;

  uint32_t video_pts_increment = 90000 / video_frame_rate;
  uint32_t audio_pts_increment = (90000 * audio_samples_per_frame) / audio_sample_rate;
  uint64_t video_pts = 0;
  uint64_t audio_pts = 0;

  // The "actual" times are just for information, so we aren't too worried
  // about accuracy - thus floating point should be OK.
  double audio_time = 0.0;
  double video_time = 0.0;

  int got_video = TRUE;
  int got_audio = TRUE;

  if (verbose)
    fprint_msg("Video PTS increment %u\n"
               "Audio PTS increment %u\n",video_pts_increment,audio_pts_increment);

  // Start off our output with some null packets - this is in case the
  // reader needs some time to work out its byte alignment before it starts
  // looking for 0x47 bytes
  for (ii=0; ii<8; ii++)
  {
    err = write_TS_null_packet(output);
    if (err) return 1;
  }

  // Then write some program data
  // @@@ later on we might want to repeat this every so often
  prog_pids[0] = DEFAULT_VIDEO_PID;
  prog_pids[1] = DEFAULT_AUDIO_PID;
  prog_type[0] = AVC_VIDEO_STREAM_TYPE;

  switch (audio_type)
  {
  case AUDIO_ADTS:
  case AUDIO_ADTS_MPEG2:
  case AUDIO_ADTS_MPEG4:
    prog_type[1] = ADTS_AUDIO_STREAM_TYPE;
    break;
  case AUDIO_L2:
    prog_type[1] = MPEG2_AUDIO_STREAM_TYPE;
    break;
  case AUDIO_AC3:
    prog_type[1] = ATSC_DOLBY_AUDIO_STREAM_TYPE;
    break;
  default:              // what else can we do?
    prog_type[1] = ADTS_AUDIO_STREAM_TYPE;
    break;
  }
  err = write_TS_program_data2(output,
                               1, // transport stream id
                               1, // program number
                               DEFAULT_PMT_PID,
                               DEFAULT_VIDEO_PID,  // PCR pid
                               2,prog_pids,prog_type);
  if (err)
  {
    print_err("### Error writing out TS program data\n");
    return 1;
  }

  while (got_video || got_audio)
  {
    access_unit_p  access_unit;
    audio_frame_p  aframe;

    // Start with a video frame
    if (got_video)
    {
      err = get_next_h264_frame(video_context,quiet,debugging,&access_unit);
      if (err == EOF)
      {
        if (verbose)
          print_msg("EOF: no more video data\n");
        got_video = FALSE;
      }
      else if (err)
        return 1;
    }

    if (got_video)
    {
      video_time = video_frame_count / (double) video_frame_rate;
      video_pts += video_pts_increment;
      video_frame_count ++;
      if (verbose)
        fprint_msg("\n%s video frame %5d (@ %.2fs, " LLU_FORMAT ")\n",
                   (is_I_or_IDR_frame(access_unit)?"**":"++"),
                   video_frame_count,video_time,video_pts);

      if (pat_pmt_freq && !(video_frame_count % pat_pmt_freq))
        {
          if (verbose)
            {
              fprint_msg("\nwriting PAT and PMT (frame = %d, freq = %d).. ", 
                         video_frame_count, pat_pmt_freq);
            }
          err = write_TS_program_data2(output, 
                                       1, // tsid
                                       1, // Program number
                                       DEFAULT_PMT_PID,
                                       DEFAULT_VIDEO_PID, // PCR pid
                                       2, prog_pids, prog_type);
        }


      // PCR counts frames as seen in the stream, so is easy
      // The presentation and decoding time for B frames (if we ever get any)
      // could reasonably be the same as the PCR.
      // The presentation and decoding time for I and IDR frames is unlikely to
      // be the same as the PCR (since frames come out later...), but it may
      // work to pretend the PTS is the PCR plus a delay time (for decoding)...

      // We could output the timing information every video frame,
      // but might as well only do it on index frames.
      if (is_I_or_IDR_frame(access_unit))
        err = write_access_unit_as_TS_with_pts_dts(access_unit,video_context,
                                                   output,DEFAULT_VIDEO_PID,
                                                   TRUE,video_pts+45000,
                                                   TRUE,video_pts);
      else
        err = write_access_unit_as_TS_with_PCR(access_unit,video_context,
                                               output,DEFAULT_VIDEO_PID,
                                               video_pts,0);
      if (err)
      {
        free_access_unit(&access_unit);
        print_err("### Error writing access unit (frame)\n");
        return 1;
      }
      free_access_unit(&access_unit);

      // Did the logical video stream end after the last access unit?
      if (video_context->end_of_stream)
      {
        if (verbose)
          print_msg("Found End-of-stream NAL unit\n");
        got_video = FALSE;
      }
    }

    if (!got_audio)
      continue;

    // Then output enough audio frames to make up to a similar time
    while (audio_pts < video_pts || !got_video)
    {
      err = read_next_audio_frame(audio_file,audio_type,&aframe);
      if (err == EOF)
      {
        if (verbose)
          print_msg("EOF: no more audio data\n");
        got_audio = FALSE;
        break;
      }
      else if (err)
        return 1;

      audio_time = audio_frame_count *
        audio_samples_per_frame / (double)audio_sample_rate;
      audio_pts += audio_pts_increment;
      audio_frame_count ++;
      if (verbose)
        fprint_msg("** audio frame %5d (@ %.2fs, " LLU_FORMAT ")\n",
                   audio_frame_count,audio_time,audio_pts);

      err = write_ES_as_TS_PES_packet_with_pts_dts(output,aframe->data,
                                                   aframe->data_len,
                                                   DEFAULT_AUDIO_PID,
                                                   DEFAULT_AUDIO_STREAM_ID,
                                                   TRUE,audio_pts,
                                                   TRUE,audio_pts);
      if (err)
      {
        free_audio_frame(&aframe);
        print_err("### Error writing audio frame\n");
        return 1;
      }
      free_audio_frame(&aframe);
    }
  }

  if (!quiet)
  {
    uint32_t video_elapsed = 100*video_frame_count/video_frame_rate;
    uint32_t audio_elapsed = 100*audio_frame_count*
      audio_samples_per_frame/audio_sample_rate;
    fprint_msg("Read %d video frame%s, %.2fs elapsed (%dm %.2fs)\n",
               video_frame_count,(video_frame_count==1?"":"s"),
               video_elapsed/100.0,video_elapsed/6000,(video_elapsed%6000)/100.0);
    fprint_msg("Read %d audio frame%s, %.2fs elapsed (%dm %.2fs)\n",
               audio_frame_count,(audio_frame_count==1?"":"s"),
               audio_elapsed/100.0,audio_elapsed/6000,(audio_elapsed%6000)/100.0);
  }

  return 0;

}

static void print_usage()
{
  print_msg(
    "Usage:\n"
    "    esmerge <video-file> <audio-file> <output-file>\n"
    "\n"
    );
  REPORT_VERSION("esmerge");
  print_msg(
    "\n"
    "  Merge the contents of two Elementary Stream (ES) files, one containing\n"
    "  video data, and the other audio, to produce an output file containing\n"
    "  Transport Stream (TS).\n"
    "\n"
    "Files:\n"
    "  <video-file>  is the ES file containing video.\n"
    "  <audio-file>  is the ES file containing audio.\n"
    "  <output-file> is the resultant TS file.\n"
    "\n"
    "Switches:\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -quiet, -q        Only output error messages.\n"
    "  -verbose, -v      Output information about each audio/video frame.\n"
    "  -x                Output diagnostic information.\n"
    "\n"
    "  -h264             The video stream is H.264 (the default)\n"
    "  -avs              The video stream is AVS\n"
    "\n"
    "  -vidrate <hz>     Video frame rate in Hz - defaults to 25Hz.\n"
    "\n"
    "  -rate <hz>        Audio sample rate in Hertz - defaults to 44100, i.e., 44.1KHz.\n"
    "  -cd               Equivalent to -rate 44100 (CD rate), the default.\n"
    "  -dat              Equivalent to -rate 48000 (DAT rate).\n"
    "\n"
    "  -adts             The audio stream is ADTS (the default)\n"
    "  -l2               The audio stream is MPEG layer 2 audio\n"
    "  -mp2adts          The audio stream is MPEG-2 style ADTS regardless of ID bit\n"
    "  -mp4adts          The audio stream is MPEG-4 style ADTS regardless of ID bit\n"
    "  -ac3              The audio stream is Dolby AC-3 in ATSC\n"
    "\n"
    "  -patpmtfreq <f>    PAT and PMT will be inserted every <f> video frames. \n"
    "                     by default, f = 0 and PAT/PMT are inserted only at  \n"
    "                     the start of the output stream.\n"
    "\n"
    "Limitations\n"
    "===========\n"
    "For the moment, the video input must be H.264 or AVS, and the audio input\n"
    "ADTS, AC-3 ATSC or MPEG layer 2. Also, the audio is assumed to have a\n"
    "constant number of samples per frame.\n"
    );
}

int main(int argc, char **argv)
{
  int    had_video_name = FALSE;
  int    had_audio_name = FALSE;
  int    had_output_name = FALSE;
  char  *video_name = NULL;
  char  *audio_name = NULL;
  char  *output_name = NULL;
  int    err = 0;
  ES_p   video_es = NULL;
  access_unit_context_p h264_video_context = NULL;
  avs_context_p avs_video_context = NULL;
  int    audio_file = -1;
  TS_writer_p output = NULL;
  int    quiet = FALSE;
  int    verbose = FALSE;
  int    debugging = FALSE;
  int    audio_samples_per_frame = ADTS_SAMPLES_PER_FRAME;
  int    audio_sample_rate = CD_RATE;
  int    video_frame_rate = DEFAULT_VIDEO_FRAME_RATE;
  int    audio_type = AUDIO_ADTS;
  int    video_type = VIDEO_H264;
  int    pat_pmt_freq = 0;
  int    ii = 1;

#if TEST_PTS_DTS
  test_pts();
  return 0;
#endif

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-help",argv[ii]) ||
          !strcmp("-h",argv[ii]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("esmerge",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### esmerge: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        verbose = TRUE;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        quiet = TRUE;
      }
      else if (!strcmp("-x",argv[ii]))
      {
        debugging = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-rate",argv[ii]))
      {
        CHECKARG("esmerge",ii);
        err = int_value("esmerge",argv[ii],argv[ii+1],TRUE,10,&audio_sample_rate);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-cd",argv[ii]))
      {
        audio_sample_rate = CD_RATE;
      }
      else if (!strcmp("-dat",argv[ii]))
      {
        audio_sample_rate = DAT_RATE;
      }
      else if (!strcmp("-vidrate",argv[ii]))
      {
        CHECKARG("esmerge",ii);
        err = int_value("esmerge",argv[ii],argv[ii+1],TRUE,10,&video_frame_rate);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-adts",argv[ii]))
      {
        audio_type = AUDIO_ADTS;
      }
      else if (!strcmp("-l2",argv[ii]))
      {
        audio_type = AUDIO_L2;
      }
      else if (!strcmp("-ac3", argv[ii]))
      {
        audio_type = AUDIO_AC3;
      }
      else if (!strcmp("-h264",argv[ii]))
      {
        video_type = VIDEO_H264;
      }
      else if (!strcmp("-mp2adts", argv[ii]))
      {
        audio_type = AUDIO_ADTS_MPEG2;
      }
      else if (!strcmp("-mp4adts", argv[ii]))
      {
        audio_type = AUDIO_ADTS_MPEG4;
      }
      else if (!strcmp("-avs",argv[ii]))
      {
        video_type = VIDEO_AVS;
      }
      else if (!strcmp("-patpmtfreq", argv[ii]))
      {
        CHECKARG("esmerge",ii);
        err = int_value("esmerge", argv[ii], argv[ii+1], TRUE, 10, &pat_pmt_freq);
        if (err) { return 1; }
        ++ii;
      }
      else
      {
        fprint_err("### esmerge: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (!had_video_name)
      {
        video_name = argv[ii];
        had_video_name = TRUE;
      }
      else if (!had_audio_name)
      {
        audio_name = argv[ii];
        had_audio_name = TRUE;
      }
      else if (!had_output_name)
      {
        output_name = argv[ii];
        had_output_name = TRUE;
      }
      else
      {
        fprint_err("### esmerge: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
    }
    ii++;
  }

  if (!had_video_name)
  {
    print_err("### esmerge: No video input file specified\n");
    return 1;
  }
  if (!had_audio_name)
  {
    print_err("### esmerge: No audio input file specified\n");
    return 1;
  }
  if (!had_output_name)
  {
    print_err("### esmerge: No output file specified\n");
    return 1;
  }

  err = open_elementary_stream(video_name,&video_es);
  if (err)
  {
    print_err("### esmerge: "
              "Problem starting to read video as ES - abandoning reading\n");
    return 1;
  }

  if (video_type == VIDEO_H264)
  {
    err = build_access_unit_context(video_es,&h264_video_context);
    if (err)
    {
      print_err("### esmerge: "
                "Problem starting to read video as H.264 - abandoning reading\n");
      close_elementary_stream(&video_es);
      return 1;
    }
  }
  else if (video_type == VIDEO_AVS)
  {
    err = build_avs_context(video_es,&avs_video_context);
    if (err)
    {
      print_err("### esmerge: "
                "Problem starting to read video as H.264 - abandoning reading\n");
      close_elementary_stream(&video_es);
      return 1;
    }
  }
  else
  {
    print_err("### esmerge: Unknown video type\n");
    return 1;
  }

  audio_file = open_binary_file(audio_name,FALSE);
  if (audio_file == -1)
  {
    print_err("### esmerge: "
              "Problem opening audio file - abandoning reading\n");
    close_elementary_stream(&video_es);
    free_access_unit_context(&h264_video_context);
    free_avs_context(&avs_video_context);
    return 1;
  }

  err = tswrite_open(TS_W_FILE,output_name,NULL,0,quiet,&output);
  if (err)
  {
    fprint_err("### esmerge: "
               "Problem opening output file %s - abandoning reading\n",
               output_name);
    close_elementary_stream(&video_es);
    close_file(audio_file);
    free_access_unit_context(&h264_video_context);
    free_avs_context(&avs_video_context);
    return 1;
  }

  switch (audio_type)
  {
  case AUDIO_ADTS:
    audio_samples_per_frame = ADTS_SAMPLES_PER_FRAME;
    break;
  case AUDIO_L2:
    audio_samples_per_frame = L2_SAMPLES_PER_FRAME;
    break;
  case AUDIO_AC3:
    audio_samples_per_frame = AC3_SAMPLES_PER_FRAME;
    break;
  default:              // hmm - or we could give up...
    audio_samples_per_frame = ADTS_SAMPLES_PER_FRAME;
    break;
  }

  if (!quiet)
  {
    fprint_msg("Reading video from %s\n",video_name);
    fprint_msg("Reading audio from %s (as %s)\n",audio_name,AUDIO_STR(audio_type));
    fprint_msg("Writing output to  %s\n",output_name);
    fprint_msg("Audio sample rate: %dHz (%.2fKHz)\n",audio_sample_rate,
               audio_sample_rate/1000.0);
    fprint_msg("Audio samples per frame: %d\n",audio_samples_per_frame);
    fprint_msg("Video frame rate: %dHz\n",video_frame_rate);
  }


  if (video_type == VIDEO_H264)
    err = merge_with_h264(h264_video_context,audio_file,output,
                          audio_type,
                          audio_samples_per_frame,audio_sample_rate,
                          video_frame_rate,
                          pat_pmt_freq,
                          quiet,verbose,debugging);
  else if (video_type == VIDEO_AVS)
    err = merge_with_avs(avs_video_context,audio_file,output,
                         audio_type,
                         audio_samples_per_frame,audio_sample_rate,
                         video_frame_rate,
                         pat_pmt_freq,
                         quiet,verbose,debugging);
  else
  {
    print_err("### esmerge: Unknown video type\n");
    return 1;
  }
  if (err)
  {
    print_err("### esmerge: Error merging video and audio streams\n");
    close_elementary_stream(&video_es);
    close_file(audio_file);
    free_access_unit_context(&h264_video_context);
    free_avs_context(&avs_video_context);
    (void) tswrite_close(output,quiet);
    return 1;
  }

  close_elementary_stream(&video_es);
  close_file(audio_file);
  free_access_unit_context(&h264_video_context);
  free_avs_context(&avs_video_context);
  err = tswrite_close(output,quiet);
  if (err)
  {
    fprint_err("### esmerge: Error closing output %s\n",output_name);
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

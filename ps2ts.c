/*
 * Convert a Program Stream to Transport Stream.
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
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "pes_fns.h"
#include "ps_fns.h"
#include "ts_fns.h"
#include "tswrite_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"

static void print_usage()
{
  print_msg(
    "Usage: ps2ts [switches] [<infile>] [<outfile>]\n"
    "\n"
    );
  REPORT_VERSION("ps2ts");
  print_msg(
    "\n"
    "  Convert an H.222 program stream to H.222 transport stream.\n"
    "\n"
    "  This program does not make use of any Program Stream Map packets\n"
    "  in the data (mainly because I have yet to see data with any). This\n"
    "  means that the program has to determine the stream type of the data\n"
    "  based on the first few ES units.\n"
    "\n"
    "  This program does not output more than one video and one audio\n"
    "  stream. If the program stream data contains more than one of each,\n"
    "  the first will be used, and the others ignored (with a message\n"
    "  indicating this).\n"
    "\n"
    "  It is assumed that the video stream will contain DTS values in its\n"
    "  PES packets at reasonable intervals, which can be used as PCR values\n"
    "  in the transport stream, and thus the video stream's PID can be used\n"
    "  as the PCR PID in the transport stream.\n"
    "\n"
    "Files:\n"
    "  <infile>           is a file containing the program stream data\n"
    "                     (but see -stdin below)\n"
    "  <outfile>          is a transport stream file\n"
    "                     (but see -stdout and -host below)\n"
    "\n"
    "Input switches:\n"
    "  -stdin            Take input from <stdin>, instead of a named file\n"
    "  -dvd              The PS data is from a DVD. This is the default.\n"
    "                    This switch has no effect on MPEG-1 PS data.\n"
    "  -notdvd, -nodvd   The PS data is not from a DVD.\n"
    "                    The DVD specification stores AC-3 (Dolby), DTS and\n"
    "                    other audio in a specialised manner in private_stream_1.\n"
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
    "Output switches:\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -stdout           Write output to <stdout>, instead of a named file\n"
    "                    Forces -quiet and -err stderr.\n"
    "  -host <host>, -host <host>:<port>\n"
    "                    Writes output (over TCP/IP) to the named <host>,\n"
    "                    instead of to a named file. If <port> is not\n"
    "                    specified, it defaults to 88.\n"
    "  -vpid <pid>       <pid> is the video PID to use for the data.\n"
    "                    Use '-vpid 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x68.\n"
    "  -apid <pid>       <pid> is the audio PID to use for the data.\n"
    "                    Use '-apid 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x67.\n"
    "  -noaudio          Don't output the audio data\n"
    "  -pmt <pid>        <pid> is the PMT PID to use.\n"
    "                    Use '-pmt 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x66\n"
    "  -prepeat <n>      Output the program data (PAT/PMT) after every <n>\n"
    "                    PS packs. Defaults to 100.\n"
    "  -pad <n>          Pad the start with <n> filler TS packets, to allow\n"
    "                    a TS reader to synchronize with the datastream.\n"
    "                    Defaults to 8.\n"
    "\n"
    "General switches:\n"
    "  -verbose, -v      Print a 'v' for each video packet and an 'a' for \n"
    "                    each audio packet, as it is read\n"
    "  -quiet, -q        Only output error messages\n"
    "  -max <n>, -m <n>  Maximum number of PS packs to read\n"
    "\n"
    "Stream type:\n"
    "  When the TS data is being output, it is flagged to indicate whether\n"
    "  it conforms to H.262, H.264, etc. It is important to get this right, as\n"
    "  it will affect interpretation of the TS data.\n"
    "\n"
    "  If input is from a file, then the program will look at the start of\n"
    "  the file to determine if the stream is H.264 or H.262 data. This\n"
    "  process may occasionally come to the wrong conclusion, in which case\n"
    "  the user can override the choice using the following switches.\n"
    "\n"
    "  If input is from standard input (via -stdin), then it is not possible\n"
    "  for the program to make its own decision on the input stream type.\n"
    "  Instead, it defaults to H.262, and relies on the user indicating if\n"
    "  this is wrong.\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    "  -mp42             Force the program to treat the input as MPEG-4/Part 2.\n"
    "  -vtype <type>     Force the program to treat the input as video of\n"
    "                    stream type <type> (e.g., 0x42 means AVS video). It is\n"
    "                    up to the user to specify a valid <type>.\n"
    "\n"
    "  If the audio stream being output is Dolby (AC-3), then the stream type\n"
    "  used to output it differs for DVB (European) and ATSC (USA) data. It\n"
    "  may be specified as follows:\n"
    "\n"
    "  -dolby dvb       Use stream type 0x06 (the default)\n"
    "  -dolby atsc      Use stream type 0x81\n"
    );

}

int main(int argc, char **argv)
{
  int     use_stdin = FALSE;
  int     use_stdout = FALSE;
  int     use_tcpip = FALSE;
  int     port = 88; // Useful default port number
  char   *input_name = NULL;
  char   *output_name = NULL;
  int     had_input_name = FALSE;
  int     had_output_name = FALSE;
  PS_reader_p ps = NULL;
  TS_writer_p output = NULL;
  int     verbose = FALSE;
  int     quiet = FALSE;
  int     max = 0;
  uint32_t pmt_pid = 0x66;
  uint32_t video_pid = 0x68;
  uint32_t pcr_pid = video_pid;  // Use PCRs from the video stream
  uint32_t audio_pid = 0x67;
  int     keep_audio = TRUE;
  int     repeat_program_every = 100;
  int     pad_start = 8;
  int     err = 0;
  int     ii = 1;

  int     video_type = VIDEO_H262;  // hopefully a sensible default
  int     force_stream_type = FALSE;

  int     video_stream = -1;
  int     audio_stream = -1;
  int     want_ac3_audio = FALSE;

  int     input_is_dvd = TRUE;
  int     want_dolby_as_dvb = TRUE;

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
      else if (!strcmp("-avc",argv[ii]) || !strcmp("-h264",argv[ii]))
      {
        force_stream_type = TRUE;
        video_type = VIDEO_H264;
      }
      else if (!strcmp("-h262",argv[ii]))
      {
        force_stream_type = TRUE;
        video_type = VIDEO_H262;
      }
      else if (!strcmp("-vtype",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value("ps2ts",argv[ii],argv[ii+1],TRUE,0,
                        &video_type);
        if (err) return 1;
        ii++;
        force_stream_type = TRUE;
      }
      else if (!strcmp("-mp42",argv[ii]))
      {
        force_stream_type = TRUE;
        video_type = VIDEO_MPEG4_PART2;
      }
      else if (!strcmp("-dolby",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        if (!strcmp("dvb",argv[ii+1]))
          want_dolby_as_dvb = TRUE;
        else if (!strcmp("atsc",argv[ii+1]))
          want_dolby_as_dvb = FALSE;
        else
        {
          print_err("### ps2ts: -dolby must be followed by dvb or atsc\n");
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        had_input_name = TRUE; // more or less
        use_stdin = TRUE;
      }
      else if (!strcmp("-stdout",argv[ii]))
      {
        had_output_name = TRUE; // more or less
        use_stdout = TRUE;
        redirect_output_stderr();
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### ps2ts: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-dvd",argv[ii]))
      {
        input_is_dvd = TRUE;
      }
      else if (!strcmp("-notdvd",argv[ii]) || !strcmp("-nodvd",argv[ii]))
      {
        input_is_dvd = FALSE;
      }
      else if (!strcmp("-host",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = host_value("ps2ts",argv[ii],argv[ii+1],&output_name,&port);
        if (err) return 1;
        had_output_name = TRUE; // more or less
        use_tcpip = TRUE;
        ii++;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        verbose = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        verbose = FALSE;
        quiet = TRUE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value("ps2ts",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-prepeat",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value("ps2ts",argv[ii],argv[ii+1],TRUE,10,
                        &repeat_program_every);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pad",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = int_value("ps2ts",argv[ii],argv[ii+1],TRUE,10,&pad_start);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-vpid",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = unsigned_value("ps2ts",argv[ii],argv[ii+1],0,&video_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-apid",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = unsigned_value("ps2ts",argv[ii],argv[ii+1],0,&audio_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pmt",argv[ii]))
      {
        CHECKARG("ps2ts",ii);
        err = unsigned_value("ps2ts",argv[ii],argv[ii+1],0,&pmt_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-noaudio",argv[ii]))
      {
        keep_audio = FALSE;
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
      else
      {
        fprint_err("### ps2ts: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name && had_output_name)
      {
        fprint_err("### ps2ts: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
      else if (had_input_name)
      {
        output_name = argv[ii];
        had_output_name = TRUE;
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
    print_err("### ps2ts: No input file specified\n");
    return 1;
  }
  if (!had_output_name)
  {
    print_err("### ps2ts: No output file specified\n");
    return 1;
  }

  // Try to stop extraneous data ending up in our output stream
  if (use_stdout)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  err = open_PS_file(input_name,quiet,&ps);
  if (err)
  {
    fprint_err("### ps2ts: Unable to open input %s\n",
               (use_stdin?"<stdin>":input_name));
    return 1;
  }

  if (!quiet)
    fprint_msg("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  // Try to decide what sort of data stream we have
  if (force_stream_type || use_stdin)
  {
    if (!quiet)
      fprint_msg("Reading input as %s (0x%02x)\n",
                 h222_stream_type_str(video_type),video_type);
  }
  else
  {
    err = determine_PS_video_type(ps,&video_type);
    if (err) return 1;
    if (!quiet)
      fprint_msg("Video appears to be %s (0x%02x)\n",
                 h222_stream_type_str(video_type),video_type);
  }

  if (!quiet)
  {
    if (input_is_dvd)
      print_msg("Treating input as from DVD\n");
    else
      print_msg("Treating input as NOT from DVD\n");

    print_msg("Reading video from ");
    if (video_stream == -1)
      print_msg("first stream found");
    else
      fprint_msg("stream %0#x (%d)",video_stream,video_stream);
    if (keep_audio)
    {
      print_msg(", audio from ");
      if (audio_stream == -1)
        fprint_msg("first %s found",(want_ac3_audio?"AC3 stream":"stream"));
      else
        fprint_msg("%s %0#x (%d)",(want_ac3_audio?"AC3 stream":"stream"),
               audio_stream,audio_stream);
      print_msg("\n");
    }

    fprint_msg("Writing video with PID 0x%02x",video_pid);
    if (keep_audio)
      fprint_msg(", audio with PID 0x%02x,",audio_pid);
    fprint_msg(" PMT PID 0x%02x, PCR PID 0x%02x\n",pmt_pid,pcr_pid);
    if (max)
      fprint_msg("Stopping after %d program stream packets\n",max);
  }

  if (use_stdout)
    err = tswrite_open(TS_W_STDOUT,NULL,NULL,0,quiet,&output);
  else if (use_tcpip)
    err = tswrite_open(TS_W_TCP,output_name,NULL,port,quiet,&output);
  else
    err = tswrite_open(TS_W_FILE,output_name,NULL,0,quiet,&output);
  if (err)
  {
    fprint_err("### ps2ts: Unable to open %s\n",output_name);
    (void) close_PS_file(&ps);
    return 1;
  }

  err = ps_to_ts(ps,output,pad_start,repeat_program_every,
                 video_type,input_is_dvd,
                 video_stream,audio_stream,want_ac3_audio,
                 want_dolby_as_dvb,pmt_pid,pcr_pid,video_pid,
                 keep_audio,audio_pid,max,verbose,quiet);
  if (err)
  {
    print_err("### ps2ts: Error transferring data\n");
    (void) close_PS_file(&ps);
    (void) tswrite_close(output,TRUE);
    return 1;
  }

  // And tidy up when we're finished
  err = tswrite_close(output,quiet);
  if (err)
    fprint_err("### ps2ts: Error closing output %s: %s\n",output_name,
            strerror(errno));
  err = close_PS_file(&ps);
  if (err)
    fprint_err("### ps2ts: Error closing input %s\n",
               (use_stdin?"<stdin>":input_name));
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

/*
 * Attempt to determine if an input stream is Transport Stream or Elementary
 * Stream, and if the latter, if it is H.262 or H.264 (MPEG-2 or MPEG-4/AVC).
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
#include <io.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "es_fns.h"
#include "ts_fns.h"
#include "nalunit_fns.h"
#include "h262_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"

#define STREAM_IS_TS     10
#define STREAM_IS_PS     11
#define STREAM_IS_H262   12
#define STREAM_IS_H264   14
#define STREAM_IS_AVS    15
#define STREAM_MAYBE_PES  5
#define STREAM_IS_UNSURE  9
#define STREAM_IS_ERROR   0


/*
 * Look for an initial 0x47 sync byte, and check for TS
 *
 * Returns 0 if nothing went wrong, 1 if something did.
 */
static int check_if_TS(int   input,
                       byte  cur_byte,
                       int   verbose,
                       int  *decided,
                       int  *result)
{
  int  ii;

  if (verbose)
    print_msg("Is it Transport Stream?\n");

  // It may be enough to look at the first byte of the stream
  if (cur_byte != 0x47)
  {
    if (verbose)
      fprint_msg("  First byte in file is 0x%02X not 0x47, so it is not\n",cur_byte);
    return 0;
  }

  // Transport Stream packets start with 0x47, so it's a good bet.
  if (verbose)
    print_msg("  First byte in file is 0x47, so it looks like Transport Stream\n");

  // To check a bit, we can try looking at every 188th byte
  if (verbose)
    print_msg("  Checking next 500 packets to see if they start 0x47\n");
  for (ii=0; ii<500; ii++)
  {
    byte buf[TS_PACKET_SIZE];
    int err = read_bytes(input,TS_PACKET_SIZE,buf);
    if (err)
    {
      fprint_err("### %s trying to read start of packet %d\n",
                 (err==EOF?"EOF":"Error"),ii+1);
      return 1;
    }
    if (buf[TS_PACKET_SIZE-1] != 0x47)
    {
      if (verbose)
        fprint_msg("  Packet %d does not start with 0x47 (%02x instead)\n",
                   ii+1,buf[TS_PACKET_SIZE-1]);
      return 0;
    }
  }
  if (verbose)
    print_msg("The checked packets all start with 0x47 - looks like TS\n");
  *decided = TRUE;
  *result  = STREAM_IS_TS;
  return 0;
}

/*
 * Try to decide if we *have* got program stream
 *
 * Returns 0 if nothing went wrong, 1 if something did.
 */
static int check_if_PS(int   input,
                       int   verbose,
                       int  *decided,
                       int  *result)
{
  int   err;
  byte  buf[10];
  int   stuffing_length;

  if (verbose)
  {
    print_msg("Is it Program Stream?\n");
    print_msg("  Trying to read pack header\n");
  }

  err = read_bytes(input,4,buf);
  if (err)
  {
    fprint_err("### %s trying to read start of first PS packet\n",
               (err==EOF?"EOF":"Error"));
    return 1;
  }

  if (buf[0] != 0 || buf[1] != 0 || buf[2] != 1 || buf[3] != 0xba)
  {
    if (verbose)
      fprint_msg("  File starts %02X %02X %02X %02X, not 00 00 01 BA - not PS\n",
                 buf[0],buf[1],buf[2],buf[3]);
    return 0;
  }

  if (verbose)
    print_msg("  File starts 00 00 01 BA - could be PS,"
              " reading pack header body\n");
  
  err = read_bytes(input,8,buf);
  if (err)
  {
    fprint_err("### %s trying to read body of PS pack header\n",
               (err==EOF?"EOF":"Error"));
    return 1;
  }
  
  if ((buf[0] & 0xF0) == 0x20)
  {
    if (verbose)
      print_msg("  Looks like ISO/IEC 11171-1/MPEG-1 pack header\n");
  }
  else if ((buf[0] & 0xC0) == 0x40)
  {
    if (verbose)
      print_msg("  Looks like ISO/IEC 13818-1/H.222.0 pack header\n");
    err = read_bytes(input,2,&(buf[8]));
    if (err)
    {
      fprint_err("### %s trying to read last 2 bytes of body of PS pack header\n",
                 (err==EOF?"EOF":"Error"));
      return 1;
    }

    stuffing_length = buf[9] & 0x07;

    // And ignore that many stuffing bytes...
    if (stuffing_length > 0)
    {
      err = read_bytes(input,stuffing_length,buf);
      if (err)
      {
        fprint_err("### %s trying to read PS pack header stuffing bytes\n",
                   (err==EOF?"EOF":"Error"));
        return 1;
      }
    }
  }

  // We could check for reserved bits - maybe at another time

  if (verbose)
    print_msg("  OK, trying to read start of next packet\n");

  err = read_bytes(input,4,buf);
  if (err)
  {
    fprint_err("### %s trying to read start of next PS packet\n",
               (err==EOF?"EOF":"Error"));
    return 1;
  }

  if (buf[0] != 0 || buf[1] != 0 || buf[2] != 1)
  {
    if (verbose)
      fprint_msg("  Next 'packet' starts %02X %02X %02X, not 00 00 01 - not PS\n",
                 buf[0],buf[1],buf[2]);
    return 0;
  }

  if (verbose)
    print_msg("  Start of second packet found at right place - looks like PS\n");

  *decided = TRUE;
  *result  = STREAM_IS_PS;
  return 0;
}

/*
 * Look at the start of our "elementary" stream, and try to determine
 * its actual type.
 *
 * - `input` is the input stream to inspect
 * - if `verbose` is true, the caller wants details of how the decision
 *   is being made
 * - `decided` is returned TRUE if the function believes it has identified
 *   the stream type, in which case:
 * - `result` will an appropriate value indicating what we've decided
 *
 * Note that this function reads into the stream, and may attempt to
 * rewind it.
 *
 * Returns 0 if nothing went wrong, 1 if an error occurred
 */
static int determine_packet_type(int   input,
                                 int   verbose,
                                 int  *decided,
                                 int  *result)
{
  int  err;
#ifdef _WIN32
  int  length;
#else
  ssize_t length;
#endif
  byte   first_byte;
  int    video_type;

  length = read(input,&first_byte,1);
  if (length == 0)
  {
    print_err("### EOF reading first byte\n");
    return 1;
  }
  else if (length == -1)
  {
    fprint_err("### Error reading first byte: %s\n",strerror(errno));
    return 1;
  }
  
  // Does it look like transport stream?
  err = check_if_TS(input,first_byte,verbose,decided,result);
  if (err)
  {
    close_file(input);
    return 1;
  }
  if (*decided)
  {
    close_file(input);
    return 0;
  }

  seek_file(input,0);

  // Does it look like program stream?
  err = check_if_PS(input,verbose,decided,result);
  if (err)
  {
    close_file(input);
    return 1;
  }
  if (*decided)
  {
    close_file(input);
    return 0;
  }

  seek_file(input,0);
  
  // Does it look like one of the types of ES we recognise?
  if (verbose)
    print_msg("Is it an Elementary Stream we recognise?\n");

  err = decide_ES_file_video_type(input,!verbose,verbose,&video_type);
  if (err)
  {
    close_file(input);
    return 1;
  }

  switch (video_type)
  {
  case VIDEO_H264:
    *result = STREAM_IS_H264;
    *decided = TRUE;
    break;
  case VIDEO_H262:
    *result = STREAM_IS_H262;
    *decided = TRUE;
    break;
  case VIDEO_AVS:
    *result = STREAM_IS_AVS;
    *decided = TRUE;
    break;
  case VIDEO_UNKNOWN:
    *result = STREAM_IS_UNSURE;
    *decided = FALSE;
    if (verbose) print_msg("Still not sure\n");
    break;
  default:
    fprint_msg("### stream_type: Unexpected decision from"
               " decide_ES_file_video_type: %d\n",video_type);
    close_file(input);
    return 1;
  }

  close_file(input);
  return 0;
}

static void print_usage()
{
  print_msg(
    "Usage: stream_type [switches] <infile>\n"
    "\n"
    );
  REPORT_VERSION("stream_type");
  print_msg(
    "\n"
    "  Attempt to determine if an input stream is Transport Stream,\n"
    "  Program Stream, or Elementary Stream, and if the latter, if it\n"
    "  is H.262 or H.264 (i.e., MPEG-2 or MPEG-4/AVC respectively)."
    "\n"
    "  The mechanisms used are fairly crude, assuming that:\n"
    "  - data is byte aligned\n"
    "  - for TS, the first byte in the file will be the start of a NAL unit,\n"
    "    and PAT/PMT packets will be findable\n"
    "  - for PS, the first packet starts immediately at the start of the\n"
    "    file, and is a pack header\n"
    "  - if the first 1000 packets could be H.262 *or* H.264, then the data\n"
    "    is assumed to be H.264 (the program doesn't try to determine\n"
    "    sensible sequences of H.262/H.264 packets, so this is a reasonable\n"
    "    way of guessing)\n"
    "\n"
    "  It is quite possible that data which is not relevant will be\n"
    "  misidentified\n"
    "\n"
    "  The program exit value is:\n"
    "  *  10 if it detects Transport Stream,\n"
    "  *  11 if it detects Program Stream,\n"
    "  *  12 if it detects Elementary Stream containing H.262 (MPEG-2),\n"
    "  *  14 if it detects Elementary Stream containing H.264 (MPEG-4/AVC),\n"
    "  *  5 if it looks like it might be PES,\n"
    "  *  9 if it really cannot decide, or\n"
    "  *  0 if some error occurred\n"
    "\n"
    "Files:\n"
    "  <infile>           is the file to analyse\n"
    "\n"
    "Switches:\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -verbose, -v      Output more detailed information about how it is\n"
    "                    making its decision\n"
    "  -quiet, -q        Only output error messages\n"
    );
}

int main(int argc, char **argv)
{
  char   *input_name = NULL;
  int     had_input_name = FALSE;
  int     input = -1;
  int     verbose = FALSE;
  int     quiet = FALSE;
  int     err = 0;
  int     ii = 1;
  int     decided = FALSE;
  int     result = STREAM_IS_ERROR;
  
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
        return STREAM_IS_ERROR;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        verbose = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("stream_type",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### stream_type: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        verbose = FALSE;
        quiet = TRUE;
      }
      else
      {
        fprint_err("### stream_type: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return STREAM_IS_ERROR;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### stream_type: Unexpected '%s'\n",argv[ii]);
        return STREAM_IS_ERROR;
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
    print_err("### stream_type: No input file specified\n");
    return STREAM_IS_ERROR;
  }
  
  input = open_binary_file(input_name,FALSE);
  if (input == -1)
  {
    fprint_err("### stream_type: Unable to open input file %s\n",
               input_name);
    return 1;
  }

  if (!quiet)
    fprint_msg("Reading from %s\n",input_name);
  
  // Try to guess
  err = determine_packet_type(input,verbose,&decided,&result);
  if (err)
  {
    print_err("### Unable to decide on stream type due to error\n");
    return STREAM_IS_ERROR;
  }

  if (!quiet)
  {
    if (!decided)
    {
      print_msg("Unable to decide\n");
      result = STREAM_IS_UNSURE;
    }
    else
    {
      switch (result)
      {
      case STREAM_IS_TS:
        print_msg("It appears to be Transport Stream\n");
        break;
      case STREAM_IS_PS:
        print_msg("It appears to be Program Stream\n");
        break;
      case STREAM_IS_H262:
        print_msg("It appears to be Elementary Stream, MPEG-2 (H.262)\n");
        break;
      case STREAM_IS_H264:
        print_msg("It appears to be Elementary Stream, MPEG-4 (H.264)\n");
        break;
      case STREAM_IS_AVS:
        print_msg("It appears to be Elementary Stream, AVS\n");
        break;
      case STREAM_MAYBE_PES:
        print_msg("It looks likely to be PES\n");
        break;
      case STREAM_IS_UNSURE:
        print_msg("It is not recognised\n");
        break;
      default:
        fprint_msg("Unexpected decision value %d\n",result);
        result = STREAM_IS_ERROR;
        break;
      }
    }
  }
  return result;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

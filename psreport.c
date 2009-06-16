/*
 * Report on an H.222 program stream (PS) file.
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
#include "ps_fns.h"
#include "pes_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"


/*
 * Report on the given file
 *
 * - `ps` represents the PS file we're reading
 * - if `is_dvd` is TRUE, then assume that data in private_stream_1 is
 *   stored using the DVD "substream" convention
 * - if `max` is more than zero, then it is the maximum number of PS packs
 *   we want to read
 * - if `verbose` is true, then we want reporting on each packet,
 *   otherwise just a summary of the number of packs/packets is output.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int report_ps(PS_reader_p  ps,
                     int          is_dvd,
                     int          max,
                     int          verbose)
{
  int   err;
  offset_t posn = 0;  // The location in the input file of the current packet
  byte  stream_id; // The packet's stream id
  int   end_of_file = FALSE;

  struct PS_packet      packet = {0};
  struct PS_pack_header header = {0};

  // Summary data
  int  count = 0;
  int  num_packs = 0;
  int  num_maps = 0;
  int  num_dirs = 0;

  int  num_video[NUMBER_VIDEO_STREAMS];
  int  min_video_size[NUMBER_VIDEO_STREAMS];
  int  max_video_size[NUMBER_VIDEO_STREAMS];
  double sum_video_size[NUMBER_VIDEO_STREAMS];

  int  num_audio[NUMBER_AUDIO_STREAMS];
  int  min_audio_size[NUMBER_AUDIO_STREAMS];
  int  max_audio_size[NUMBER_AUDIO_STREAMS];
  double sum_audio_size[NUMBER_AUDIO_STREAMS];

#define cPRIVATE1       0
#define cPRIVATE2       1
#define cPRIVATE_SIZE   2
  int  num_private[cPRIVATE_SIZE]        = {0, 0};
  int  min_private_size[cPRIVATE_SIZE]   = {INT_MAX, INT_MAX};
  int  max_private_size[cPRIVATE_SIZE]   = {0, 0};
  double sum_private_size[cPRIVATE_SIZE] = {0, 0};

#define cAC3            0
#define cDTS            1
#define cLPCM           2
#define cSUBPICTURES    3
#define cOTHER          4
  // Our arrays are 5 wide (for the 4 types of data + other we know about) by
  // <n> deep, where <n>=32 allows for 32 subpictures. This wastes space
  // for the other datatypes, which can only go to 8, but is simple...
#define cSIZE           5
#define cDEPTH         32
  int  num_other[cSIZE][cDEPTH];
  int  min_other_size[cSIZE][cDEPTH];
  int  max_other_size[cSIZE][cDEPTH];
  double sum_other_size[cSIZE][cDEPTH];

  // AC3 data can have two other types of information we want to remember...
  byte ac3_bsmod[cDEPTH] = {0};
  byte ac3_acmod[cDEPTH] = {0};

  int ii,jj;
  for (jj=0; jj<NUMBER_VIDEO_STREAMS; jj++)
  {
    num_video[jj] = 0;
    min_video_size[jj] = INT_MAX;
    max_video_size[jj] = 0;
    sum_video_size[jj] = 0.0;
  }
  for (jj=0; jj<NUMBER_AUDIO_STREAMS; jj++)
  {
    num_audio[jj] = 0;
    min_audio_size[jj] = INT_MAX;
    max_audio_size[jj] = 0;
    sum_audio_size[jj] = 0.0;
  }
  for (ii=0; ii<cSIZE; ii++)
  {
    for (jj=0; jj<cDEPTH; jj++)
    {
      num_other[ii][jj] = 0;
      min_other_size[ii][jj] = INT_MAX;
      max_other_size[ii][jj] = 0;
      sum_other_size[ii][jj] = 0.0;
    }
  }

  // Read the start of the first packet (we confidently expect this
  // to be a pack header)
  err = read_PS_packet_start(ps,verbose,&posn,&stream_id);
  if (err == EOF)
  {
    print_err("### Error reading first pack header\n");
    print_err("    Unexpected end of PS at start of stream\n");
    return 1;
  }
  else if (err)
  {
    print_err("### Error reading first pack header\n");
    return 1;
  }
  count++;

  if (stream_id != 0xba)
  {
    print_err("### Program stream does not start with pack header\n");
    fprint_err("    First packet has stream id %02X (",stream_id);
    print_stream_id(FALSE,stream_id);
    print_err(")\n");
    return 1;
  }

  // But given that, we can now happily loop reading in packs
  for (;;)
  {
    int  num_system_headers = 0;

    if (max > 0 && num_packs >= max)
    {
      if (verbose)
        fprint_msg("Stopping after %d packs\n",num_packs);
      break;
    }

    num_packs ++;

    err = read_PS_pack_header_body(ps,&header);
    if (err)
    {
      fprint_err("### Error reading data for pack header starting at "
                 OFFSET_T_FORMAT "\n",posn);
      goto give_up;
    }

    if (verbose)
      fprint_msg("\n" OFFSET_T_FORMAT_08
                 ": Pack header: SCR " LLD_FORMAT " (" LLD_FORMAT
                 "/%d) mux rate %d\n",posn,header.scr,header.scr_base,
                 header.scr_extn,header.program_mux_rate);

    // Read (and, for the moment, at least, ignore) any system headers
    for (;;)
    {
      err = read_PS_packet_start(ps,verbose,&posn,&stream_id);
      if (err == EOF)
      {
        end_of_file = TRUE;
        break;
      }
      else if (err)
        goto give_up;
      count++;

      if (stream_id == 0xbb) // System header
      {
        err = read_PS_packet_body(ps,stream_id,&packet);
        if (err)
        {
          fprint_err("### Error reading system header starting at "
                     OFFSET_T_FORMAT "\n",posn);
          goto give_up;
        }
        // For the moment, just ignore the system header content
        num_system_headers ++;
        if (verbose)
          fprint_msg(OFFSET_T_FORMAT_08 ": System header %d\n",
                     posn,num_system_headers);
      }
      else
        break;
    }

    if (end_of_file)
      break;

    // We've finished with system headers - onto data (one fondly hopes)
    for (;;)
    {
      if (stream_id == 0xba)  // Start of the next pack
        break;

      err = read_PS_packet_body(ps,stream_id,&packet);
      if (err)
      {
        fprint_err("### Error reading PS packet starting at "
                   OFFSET_T_FORMAT "\n",posn);
        goto give_up;
      }
      // For the moment, just ignore its content
      if (verbose)
      {
        fprint_msg(OFFSET_T_FORMAT_08 ": PS Packet %2d stream %02X (",
                   posn,count,stream_id);
        print_stream_id(TRUE,stream_id);
        print_msg(")\n");
        print_data(TRUE,"          Packet",
                   packet.data,packet.data_len,20);
#if 1 // XXX
        print_end_of_data("                ",packet.data,packet.data_len,20);
#endif
        if (IS_AUDIO_STREAM_ID(stream_id) || IS_VIDEO_STREAM_ID(stream_id))
#if 1 // XXX
          report_PES_data_array2(-1,packet.data,packet.data_len,20);
#else
          report_PES_data_array("          ",packet.data,packet.data_len,TRUE);
#endif
      }

      if (stream_id == 0xBC)
        num_maps ++;
      else if (stream_id == 0xFF)
        num_dirs ++;
      else if (stream_id == PRIVATE1_AUDIO_STREAM_ID)
      {
        int  substream_index;
        byte bsmod, acmod;
        int  what = identify_private1_data(&packet,is_dvd,verbose,
                                           &substream_index,&bsmod,&acmod);

        num_private[cPRIVATE1] ++;
        sum_private_size[cPRIVATE1] += packet.data_len;
        if (packet.data_len > max_private_size[cPRIVATE1])
          max_private_size[cPRIVATE1] = packet.data_len;
        if (packet.data_len < min_private_size[cPRIVATE1])
          min_private_size[cPRIVATE1] = packet.data_len;

        if (what != SUBSTREAM_ERROR)
        {
          int index;
          if (substream_index < 0 || substream_index >= cDEPTH)
          {
            fprint_err("Internal error: got substream index %d"
                       " (instead, counting item wrongly as index %d)\n",
                       substream_index,cDEPTH-1);
            substream_index = cDEPTH-1;
          }
          switch (what)
          {
          case SUBSTREAM_AC3:
            index = cAC3;
            ac3_bsmod[substream_index] = bsmod;
            ac3_acmod[substream_index] = acmod;
            break;
          case SUBSTREAM_DTS: index = cDTS; break;
          case SUBSTREAM_LPCM: index = cLPCM; break;
          case SUBSTREAM_SUBPICTURES: index = cSUBPICTURES; break;
          case SUBSTREAM_OTHER: index = cOTHER; break;
          default: fprint_err("Internal error: got substream id %d"
                              " (instead, counting item wrongly as OTHER)\n",what);
                   index = cOTHER;
                   break;
          }
          num_other[index][substream_index] ++;
          sum_other_size[index][substream_index] += packet.data_len;
          if (packet.data_len > max_other_size[index][substream_index])
            max_other_size[index][substream_index] = packet.data_len;
          if (packet.data_len < min_other_size[index][substream_index])
            min_other_size[index][substream_index] = packet.data_len;
        }
      }
      else if (stream_id == PRIVATE2_AUDIO_STREAM_ID)
      {
        num_private[cPRIVATE2] ++;
        sum_private_size[cPRIVATE2] += packet.data_len;
        if (packet.data_len > max_private_size[cPRIVATE2])
          max_private_size[cPRIVATE2] = packet.data_len;
        if (packet.data_len < min_private_size[cPRIVATE2])
          min_private_size[cPRIVATE2] = packet.data_len;
      }
      else if (IS_AUDIO_STREAM_ID(stream_id))
      {
        int num = stream_id & 0x1F;
        num_audio[num] ++;
        sum_audio_size[num] += packet.data_len;
        if (packet.data_len > max_audio_size[num])
          max_audio_size[num] = packet.data_len;
        if (packet.data_len < min_audio_size[num])
          min_audio_size[num] = packet.data_len;
      }
      else if (IS_VIDEO_STREAM_ID(stream_id))
      {
        int num = stream_id & 0x0F;
        num_video[num] ++;
        sum_video_size[num] += packet.data_len;
        if (packet.data_len > max_video_size[num])
          max_video_size[num] = packet.data_len;
        if (packet.data_len < min_video_size[num])
          min_video_size[num] = packet.data_len;
      }
      err = read_PS_packet_start(ps,verbose,&posn,&stream_id);
      if (err == EOF)
      {
        end_of_file = TRUE;
        break;
      }
      else if (err)
        goto give_up;
      count++;
    }
    if (end_of_file)
      break;
  }

give_up:
  clear_PS_packet(&packet);

  {
    int ii;
    fprint_msg("Packets (total):                %8d\n",count);
    fprint_msg("Packs:                          %8d\n",num_packs);
    for (ii=0; ii<NUMBER_VIDEO_STREAMS; ii++)
      if (num_video[ii] > 0)
      {
        fprint_msg("Video packets (stream %2d):  %8d",ii,num_video[ii]);
        fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                   min_video_size[ii],max_video_size[ii],
                   sum_video_size[ii]/num_video[ii]);
      }
    for (ii=0; ii<NUMBER_AUDIO_STREAMS; ii++)
      if (num_audio[ii] > 0)
      {
        fprint_msg("Audio packets (stream %2d):  %8d",ii,num_audio[ii]);
        fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                   min_audio_size[ii],max_audio_size[ii],
                   sum_audio_size[ii]/num_audio[ii]);
      }
    if (num_private[cPRIVATE1] > 0)
    {
      int ii;
      fprint_msg("Private1 packets:           %8d",num_private[cPRIVATE1]);
      fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                 min_private_size[cPRIVATE1],max_private_size[cPRIVATE1],
                 sum_private_size[cPRIVATE1]/num_private[cPRIVATE1]);
      for (ii=0; ii<cDEPTH; ii++)
      {
        if (num_other[cAC3][ii] > 0)
        {
          fprint_msg("     AC3, index %2d:         %8d",ii,num_other[cAC3][ii]);
          fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                     min_other_size[cAC3][ii],max_other_size[cAC3][ii],
                     sum_other_size[cAC3][ii]/num_other[cAC3][ii]);
          fprint_msg("                                      %s\n",
                     BSMOD_STR(ac3_bsmod[ii],ac3_acmod[ii]));
          fprint_msg("                                      audio coding mode %s\n",
                     ACMOD_STR(ac3_acmod[ii]));
        }
      }
      for (ii=0; ii<cDEPTH; ii++)
      {
        if (num_other[cDTS][ii] > 0)
        {
          fprint_msg("     DTS, index %2d:         %8d",ii,num_other[cDTS][ii]);
          fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                     min_other_size[cDTS][ii],max_other_size[cDTS][ii],
                     sum_other_size[cDTS][ii]/num_other[cDTS][ii]);
        }
      }
      for (ii=0; ii<cDEPTH; ii++)
      {
        if (num_other[cLPCM][ii] > 0)
        {
          fprint_msg("     LPCM, index %2d:        %8d",ii,num_other[cLPCM][ii]);
          fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                     min_other_size[cLPCM][ii],max_other_size[cLPCM][ii],
                     sum_other_size[cLPCM][ii]/num_other[cLPCM][ii]);
        }
      }
      for (ii=0; ii<cDEPTH; ii++)
      {
        if (num_other[cSUBPICTURES][ii] > 0)
        {
          fprint_msg("     SUBPICTURES, index %2d: %8d",ii,num_other[cSUBPICTURES][ii]);
          fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                     min_other_size[cSUBPICTURES][ii],max_other_size[cSUBPICTURES][ii],
                     sum_other_size[cSUBPICTURES][ii]/num_other[cSUBPICTURES][ii]);
        }
      }
      for (ii=0; ii<cDEPTH; ii++)
      {
        if (num_other[cOTHER][ii] > 0)
        {
          fprint_msg("     OTHER, index %2d:       %8d",ii,num_other[cOTHER][ii]);
          fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                     min_other_size[cOTHER][ii],max_other_size[cOTHER][ii],
                     sum_other_size[cOTHER][ii]/num_other[cOTHER][ii]);
        }
      }
    }
    if (num_private[cPRIVATE2] > 0)
    {
      fprint_msg("Private2 packets:           %8d",num_private[cPRIVATE2]);
      fprint_msg("  min size %5d, max size %5d, mean size %7.1f\n",
                 min_private_size[cPRIVATE2],max_private_size[cPRIVATE2],
                 sum_private_size[cPRIVATE2]/num_private[cPRIVATE2]);
    }
    fprint_msg("Program stream maps:        %8d\n",num_maps);
    fprint_msg("Program stream directories: %8d\n",num_maps);
  }
  return 0;
}
  
static void print_usage()
{
  print_msg(
    "Usage: psreport [switches] [<infile>]\n"
    "\n"
    );
  REPORT_VERSION("psreport");
  print_msg(
    "\n"
    "  Report on the packets in a Program Stream.\n"
    "\n"
    "Files:\n"
    "  <infile>  is an H.222 Program Stream file (but see -stdin)\n"
    "\n"
    "Switches:\n"
    "  -err stdout        Write error messages to standard output (the default)\n"
    "  -err stderr        Write error messages to standard error (Unix traditional)\n"
    "  -stdin             Input from standard input, instead of a file\n"
    "  -verbose, -v       Output packet data as well.\n"
    "  -max <n>, -m <n>   Maximum number of PS packets to read\n"
    "  -dvd               The PS data is from a DVD. This is the default.\n"
    "                     This switch has no effect on MPEG-1 PS data.\n"
    "  -notdvd, -nodvd    The PS data is not from a DVD.\n"
    "                     The DVD specification stores AC-3 (Dolby), DTS and\n"
    "                     other audio in a specialised manner in private_stream_1.\n"
    );
}

int main(int argc, char **argv)
{
  int    use_stdin = FALSE;
  char  *input_name = NULL;
  int    had_input_name = FALSE;

  PS_reader_p  ps;        // The PS file we're reading
  int    max     = 0;     // The maximum number of PS packets to read (or 0)
  int    verbose = FALSE; // True => output diagnostic/progress messages
  int    is_dvd = TRUE;

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
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("psreport",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### psreport: "
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
      else if (!strcmp("-dvd",argv[ii]))
      {
        is_dvd = TRUE;
      }
      else if (!strcmp("-notdvd",argv[ii]) || !strcmp("-nodvd",argv[ii]))
      {
        is_dvd = FALSE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("psreport",ii);
        err = int_value("psreport",argv[ii],argv[ii+1],TRUE,10,&max);
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
        fprint_err("### psreport: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### psreport: Unexpected '%s'\n",argv[ii]);
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
    print_err("### psreport: No input file specified\n");
    return 1;
  }

  err = open_PS_file(input_name,FALSE,&ps);
  if (err)
  {
    fprint_err("### psreport: Unable to open input file %s\n",
               (use_stdin?"<stdin>":input_name));
    return 1;
  }
  fprint_msg("Reading from %s\n",(use_stdin?"<stdin>":input_name));
  if (is_dvd)
    print_msg("Assuming data is from a DVD\n");
  else
    print_msg("Assuming data is NOT from a DVD\n");

  if (max)
    fprint_msg("Stopping after %d PS packets\n",max);

  err = report_ps(ps,is_dvd,max,verbose);
  if (err)
    print_err("### psreport: Error reporting on input stream\n");

  err = close_PS_file(&ps);
  if (err)
  {
    fprint_err("### psreport: Error closing input file %s\n",
               (use_stdin?"<stdin>":input_name));
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

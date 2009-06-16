/*
 * Output a reversed representation of an H.264 (MPEG-4/AVC) or H.262 (MPEG-2)
 * elementary stream.
 *
 * Note that the input stream must be seekable, which means that an option
 * to read from standard input is not provided.
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
#include <fcntl.h>
#include <string.h>

#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "es_fns.h"
#include "nalunit_fns.h"
#include "accessunit_fns.h"
#include "h262_fns.h"
#include "ts_fns.h"
#include "tswrite_fns.h"
#include "pes_fns.h"
#include "reverse_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"

#define DEBUG 0
#define SHOW_REVERSE_DATA 1
#if SHOW_REVERSE_DATA
static int show_reverse_data = FALSE;
#endif


/*
 * Write out packet data as ES or TS. This is defined in reverse.c, but
 * otherwise unadvertised.
 */
extern int write_packet_data(WRITER   output,
                             int      as_TS,
                             byte     data[],
                             int      data_len,
                             uint32_t pid,
                             byte     stream_id);

/*
 * Find the I slices in our input stream, and output them in reverse order.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `frequency` is non-zero, then attempt to produce the effect of
 *   keeping every <frequency>th picture (similar to reversing at a
 *   multiplication factor of `frequency`) If 0, just retain all I pictures.
 * - if `as_TS` is true, then output as TS packets, not ES
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int reverse_h262(ES_p    es,
                        WRITER  output,
                        int     max,
                        int     frequency,
                        int     as_TS,
                        int     verbose,
                        int     quiet)
{
  int err = 0;
  reverse_data_p  reverse_data = NULL;
  h262_context_p  hcontext = NULL;

  err = build_h262_context(es,&hcontext);
  if (err) return 1;

  err = build_reverse_data(&reverse_data,FALSE);
  if (err)
  {
    free_h262_context(&hcontext);
    return 1;
  }

  if (!quiet)
    print_msg("\nScanning forwards\n");
  
  add_h262_reverse_context(hcontext,reverse_data);
  err = collect_reverse_h262(hcontext,max,verbose,quiet);
  if (err && err != EOF)
  {
    if (reverse_data->length > 0)
    {
      fprint_err("!!! Collected %d pictures and sequence headers,"
                 " continuing to reverse\n",reverse_data->length);
    }
    else
    {
      free_reverse_data(&reverse_data);
      free_h262_context(&hcontext);
      return 1;
    }
  }

#if SHOW_REVERSE_DATA
  if (show_reverse_data)
  {
    int ii;
    for (ii=0; ii<reverse_data->length; ii++)
      if (reverse_data->seq_offset[ii])
        fprint_msg("%3d: %4d at " OFFSET_T_FORMAT "/%d for %d\n",
                   ii,reverse_data->index[ii],
                   reverse_data->start_file[ii],
                   reverse_data->start_pkt[ii],
                   reverse_data->data_len[ii]);
    else
      fprint_msg("%3d: seqh at " OFFSET_T_FORMAT "/%d for %d\n",
                 ii,
                 reverse_data->start_file[ii],
                 reverse_data->start_pkt[ii],
                 reverse_data->data_len[ii]);
  }
  if (!es->reading_ES)
    write_program_data(es->reader,output.ts_output);
#endif
  
  if (!es->reading_ES)
  {
    // Just in case (it can't hurt)
    stop_server_output(es->reader);
    // But this is important
    set_PES_reader_video_only(es->reader,TRUE);
  }

  if (!quiet)
    print_msg("\nOutputting in reverse order\n");

  if (as_TS)
    err = output_in_reverse_as_TS(es,output.ts_output,frequency,verbose,quiet,
                                  -1,0,reverse_data);
  else
    err = output_in_reverse_as_ES(es,output.es_output,frequency,verbose,quiet,
                                  -1,0,reverse_data);

  if (!err && !quiet)
  {
    uint32_t final_index = reverse_data->index[reverse_data->first_written];
    print_msg("\n");
    print_msg("Summary\n");
    print_msg("=======\n");
    print_msg("              Considered       Used            Written\n");
    fprint_msg("Pictures      %10d %10d (%4.1f%%) %10d (%4.1f%%)\n",
               final_index,reverse_data->pictures_kept,
               100*(((double)reverse_data->pictures_kept)/final_index),
               reverse_data->pictures_written,
               100*(((double)reverse_data->pictures_written)/final_index));
    if (frequency != 0)
      fprint_msg("Target (pictures)      . %10d (%4.1f%%) at requested"
                 " frequency %d\n",final_index/frequency,100.0/frequency,
                 frequency);
  }
  
  free_reverse_data(&reverse_data);
  free_h262_context(&hcontext);
  return err;
}

/*
 * Output any sequence and picture parameter sets
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int output_parameter_sets(WRITER                 output,
                                 access_unit_context_p  context,
                                 int                    as_TS,
                                 int                    quiet)
{
  nal_unit_context_p  nac = context->nac;
  param_dict_p        seq_param_dict = nac->seq_param_dict;
  param_dict_p        pic_param_dict = nac->pic_param_dict;

  int  ii;
  int  err;

  for (ii = 0; ii < seq_param_dict->length; ii++)
  {
    ES_offset  posn = seq_param_dict->posns[ii];
    uint32_t   length = seq_param_dict->data_lens[ii];
    byte      *data = NULL;
    if (!quiet)
      fprint_msg("Writing out sequence parameter set %d\n",
                 seq_param_dict->ids[ii]);

    err = read_ES_data(nac->es,posn,length,NULL,&data);
    if (err)
    {
      fprint_err("### Error reading (sequence parameter set %d) data"
                 " from " OFFSET_T_FORMAT "/%d for %d\n",
                 seq_param_dict->ids[ii],posn.infile,posn.inpacket,length);
      return 1;
    }
    err = write_packet_data(output,as_TS,data,length,DEFAULT_VIDEO_PID,
                            DEFAULT_VIDEO_STREAM_ID);
    free(data);
    if (err)
    {
      fprint_err("### Error writing out (sequence parameter set %d)"
                 "data\n",seq_param_dict->ids[ii]);
      return 1;
    }
  }

  for (ii = 0; ii < pic_param_dict->length; ii++)
  {
    ES_offset  posn = pic_param_dict->posns[ii];
    uint32_t   length = pic_param_dict->data_lens[ii];
    byte      *data = NULL;
    if (!quiet)
      fprint_msg("Writing out picture parameter set %d\n",
                 pic_param_dict->ids[ii]);

    err = read_ES_data(nac->es,posn,length,NULL,&data);
    if (err)
    {
      fprint_err("### Error reading (picture parameter set %d) data"
                 " from " OFFSET_T_FORMAT "/%d for %d\n",
                 pic_param_dict->ids[ii],posn.infile,posn.inpacket,length);
      return 1;
    }
    err = write_packet_data(output,as_TS,data,length,DEFAULT_VIDEO_PID,
                            DEFAULT_VIDEO_STREAM_ID);
    free(data);
    if (err)
    {
      fprint_err("### Error writing out (picture parameter set %d)"
                 "data\n",pic_param_dict->ids[ii]);
      return 1;
    }
  }
  return 0;
}

/*
 * Find IDR and I access units, and output them in reverse order.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int reverse_access_units(ES_p   es,
                                WRITER output,
                                int    max,
                                int    frequency,
                                int    as_TS,
                                int    verbose,
                                int    quiet)
{
  int err = 0;
  reverse_data_p  reverse_data = NULL;
  access_unit_context_p  acontext = NULL;

  err = build_access_unit_context(es,&acontext);
  if (err) return 1;

  err = build_reverse_data(&reverse_data,TRUE);
  if (err)
  {
    free_access_unit_context(&acontext);
    return 1;
  }

  if (!quiet)
    print_msg("\nScanning forwards\n");

  add_access_unit_reverse_context(acontext,reverse_data);
  err = collect_reverse_access_units(acontext,max,verbose,quiet);
  if (err && err != EOF)
  {
    if (reverse_data->length > 0)
    {
      fprint_err("!!! Collected %d access units,"
                 " continuing to reverse\n",reverse_data->length);
    }
    else
    {
      free_reverse_data(&reverse_data);
      free_access_unit_context(&acontext);
      return 1;
    }
  }

#if SHOW_REVERSE_DATA
  if (show_reverse_data)
  {
    int ii;
    for (ii=0; ii<reverse_data->length; ii++)
      fprint_msg("%3d: %4d at " OFFSET_T_FORMAT "/%d for %d\n",
                 ii,reverse_data->index[ii],
                 reverse_data->start_file[ii],
                 reverse_data->start_pkt[ii],
                 reverse_data->data_len[ii]);
  }
  //if (!es->reading_ES)
  //  write_program_data(es->reader,output.ts_output);
#endif
  
  if (!es->reading_ES)
  {
    // Just in case (it can't hurt)
    stop_server_output(es->reader);
    // But this is important
    set_PES_reader_video_only(es->reader,TRUE);
  }

  // Before outputting any reverse data, it's a good idea to write out the
  // picture parameter set(s) and sequence parameter set(s)
  if (!quiet)
    print_msg("\nPreparing to output reverse data\n");
  err = output_parameter_sets(output,acontext,as_TS,quiet);
  if (err)
  {
    free_reverse_data(&reverse_data);
    free_access_unit_context(&acontext);
    return 1;
  }

  if (!quiet)
    print_msg("\nOutputting in reverse order\n");

  if (as_TS)
    err = output_in_reverse_as_TS(es,output.ts_output,frequency,verbose,quiet,
                                  -1,0,reverse_data);
  else
    err = output_in_reverse_as_ES(es,output.es_output,frequency,verbose,quiet,
                                  -1,0,reverse_data);
  if (!err && !quiet)
  {
    uint32_t final_index = reverse_data->index[reverse_data->first_written];
    print_msg("\n");
    print_msg("Summary\n");
    print_msg("=======\n");
    print_msg("              Considered       Used            Written\n");
    fprint_msg("Access units  %10d %10d (%4.1f%%) %10d (%4.1f%%)\n",
               final_index,reverse_data->pictures_kept,
               100*(((double)reverse_data->pictures_kept)/final_index),
               reverse_data->pictures_written,
               100*(((double)reverse_data->pictures_written)/final_index));
    if (frequency != 0)
      fprint_msg("Target (access units)  . %10d (%4.1f%%) at requested"
                 " frequency %d\n",final_index/frequency,100.0/frequency,
                 frequency);
  }
  free_reverse_data(&reverse_data);
  free_access_unit_context(&acontext);
  return err;
}

static void print_usage()
{
  print_msg(
    "Usage: esreverse [switches] [<infile>] [<outfile>]\n"
    "\n"
    );
  REPORT_VERSION("esreverse");
  print_msg(
    "\n"
    "  Output a reversed stream derived from the input H.264 (MPEG-4/AVC)\n"
    "  or H.262 (MPEG-2) elementary stream.\n"
    "\n"
    "  If output is to an H.222 Transport Stream, then fixed values for\n"
    "  the PMT PID (0x66) and video PID (0x68) are used.\n"
    "\n"
    "Files:\n"
    "  <infile>  is the input elementary stream.\n"
    "  <outfile> is the output stream, either an equivalent elementary\n"
    "            stream, or an H.222 Transport Stream (but see -stdout\n"
    "            and -host below).\n"
    "\n"
    "Switches:\n"
    "  -verbose, -v      Output additional (debugging) messages\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -quiet, -q        Only output error messages\n"
    "  -stdout           Write output to <stdout>, instead of a named file\n"
    "                    Forces -quiet and -err stderr.\n"
    "  -host <host>, -host <host>:<port>\n"
    "                    Writes output (over TCP/IP) to the named <host>,\n"
    "                    instead of to a named file. If <port> is not\n"
    "                    specified, it defaults to 88. Implies -tsout.\n"
    "  -max <n>, -m <n>  Maximum number of frames to read\n"
    "  -freq <n>         Specify the frequency of frames to try to keep\n"
    "                    when reversing. Defaults to 8.\n"
    "  -tsout               Output H.222 Transport Stream\n"
    "\n"
    "  -pes, -ts         The input file is TS or PS, to be read via the\n"
    "                    PES->ES reading mechanisms\n"
    "  -server           Also output as normal forward video as reversal\n"
    "                    data is being collected. Implies -pes and -tsout.\n"
#if SHOW_REVERSE_DATA
    "\n"
    "  -x                Temporary extra debugging information\n"
#endif
    "\n"
    "Stream type:\n"
    "  If input is from a file, then the program will look at the start of\n"
    "  the file to determine if the stream is H.264 or H.262 data. This\n"
    "  process may occasionally come to the wrong conclusion, in which case\n"
    "  the user can override the choice using the following switches.\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    );
}

int main(int argc, char **argv)
{
  char  *input_name = NULL;
  char  *output_name = NULL;
  int    had_input_name = FALSE;
  int    had_output_name = FALSE;
  int    use_stdout = FALSE;
  int    use_tcpip = FALSE;
  int    port = 88; // Useful default port number
  int    err = 0;
  ES_p   es = NULL;
  WRITER output;
  int    max = 0;
  int    as_TS = FALSE;
  int    frequency = 8; // The default as stated in the usage
  int    quiet = FALSE;
  int    verbose = FALSE;
  int    ii = 1;

  int    use_pes = FALSE;
  int    use_server = FALSE;

  int     want_data = VIDEO_H262;
  int     is_data;
  int     force_stream_type = FALSE;
  byte    stream_type;
 
  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  output.es_output = NULL;
  
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
#if SHOW_REVERSE_DATA
      else if (!strcmp("-x",argv[ii]))
        show_reverse_data = TRUE;
#endif
      else if (!strcmp("-avc",argv[ii]) || !strcmp("-h264",argv[ii]))
      {
        force_stream_type = TRUE;
        want_data = VIDEO_H264;
      }
      else if (!strcmp("-h262",argv[ii]))
      {
        force_stream_type = TRUE;
        want_data = VIDEO_H262;
      }
      else if (!strcmp("-pes",argv[ii]) || !strcmp("-ts",argv[ii]))
        use_pes = TRUE;
      else if (!strcmp("-server",argv[ii]))
      {
        use_server = TRUE;
        use_pes = TRUE;
        as_TS = TRUE;
      }
      else if (!strcmp("-tsout",argv[ii]))
        as_TS = TRUE;
      else if (!strcmp("-stdout",argv[ii]))
      {
        had_output_name = TRUE; // more or less
        use_stdout = TRUE;
        redirect_output_stderr();
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("esreverse",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### esreverse: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-host",argv[ii]))
      {
        CHECKARG("esreverse",ii);
        err = host_value("esreverse",argv[ii],argv[ii+1],&output_name,&port);
        if (err) return 1;
        had_output_name = TRUE; // more or less
        use_tcpip = TRUE;
        as_TS = TRUE;
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
        CHECKARG("esreverse",ii);
        err = int_value("esreverse",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-freq",argv[ii]))
      {
        CHECKARG("esreverse",ii);
        err = int_value("esreverse",argv[ii],argv[ii+1],TRUE,10,&frequency);
        if (err) return 1;
        ii++;
      }
      else
      {
        fprint_err("### esreverse: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name && had_output_name)
      {
        fprint_err("### esreverse: Unexpected '%s'\n",argv[ii]);
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
    print_err("### esreverse: No input file specified\n");
    return 1;
  }
  if (!had_output_name)
  {
    print_err("### esreverse: No output file specified\n");
    return 1;
  }

  // Try to stop extraneous data ending up in our output stream
  if (use_stdout)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  err = open_input_as_ES(input_name,use_pes,quiet,
                         force_stream_type,want_data,&is_data,&es);
  if (err)
  {
    print_err("### esreverse: Error opening input file\n");
    return 1;
  }

  if (is_data == VIDEO_H262)
    stream_type = MPEG2_VIDEO_STREAM_TYPE;
  else if (is_data == VIDEO_H264)
    stream_type = AVC_VIDEO_STREAM_TYPE;
  else
  {
    print_err("### esreverse: Unexpected type of video data\n");
    return 1;
  }
  
  if (as_TS)
  {
    if (use_stdout)
      err = tswrite_open(TS_W_STDOUT,NULL,NULL,0,quiet,&(output.ts_output));
    else if (use_tcpip)
      err = tswrite_open(TS_W_TCP,output_name,NULL,port,quiet,&(output.ts_output));
    else
      err = tswrite_open(TS_W_FILE,output_name,NULL,0,quiet,&(output.ts_output));
    if (err)
    {
      fprint_err("### esreverse: Unable to open %s\n",output_name);
      (void) close_input_as_ES(input_name,&es);
      return 1;
    }
  }
  else
  {
    output.es_output = fopen(output_name,"wb");
    if (output.es_output == NULL)
    {
      fprint_err("### esreverse: Unable to open output file %s: %s\n",
                 output_name,strerror(errno));
      (void) close_input_as_ES(input_name,&es);
      return 1;
    }
    if (!quiet)
      fprint_msg("Writing to   %s\n",output_name);
  }

  if (!quiet)
  {
    if (as_TS)
      print_msg("Writing as Transport Stream\n");
    fprint_msg("Filtering freqency %d\n",frequency);
    if (max)
      fprint_msg("Stopping as soon after %d %s as possible\n",max,
                 (is_data == VIDEO_H262?"MPEG2 items":"NAL units"));
  }

  if (use_pes)
  {
#if SHOW_REVERSE_DATA
    if (show_reverse_data)
      es->reader->debug_read_packets = TRUE;
#endif
    if (use_server)
    {
      // For testing purposes, let's try outputting video as we collect data
      set_server_output(es->reader,output.ts_output,FALSE,100);
      es->reader->debug_read_packets = TRUE;
    }
  }
  
  // If we're writing out TS data, start it off now
  // (we mustn't do it after our forwards-processing function,
  // because that itself may output some data...)
  if (as_TS)
  {
    if (use_pes)
    {
      if (!quiet)
        fprint_msg("Using transport stream id 1, PMT PID %#x, program 1 ="
                   " PID %#x\n",DEFAULT_PMT_PID,DEFAULT_VIDEO_PID);
      set_PES_reader_program_data(es->reader,1,DEFAULT_PMT_PID,
                                  DEFAULT_VIDEO_PID,
                                  DEFAULT_AUDIO_PID,  // not actually used
                                  DEFAULT_VIDEO_PID); // video as PCR
      // Note that (a) the server output will write program data for us,
      // and (b) for the moment, the TS writer does not allow us to set the
      // stream_type
    }
    else
    {
      if (!quiet)
        fprint_msg("Using transport stream id 1, PMT PID %#x, program 1 ="
                   " PID %#x, stream type %#x\n",DEFAULT_PMT_PID,DEFAULT_VIDEO_PID,
                   stream_type);
      err = write_TS_program_data(output.ts_output,
                                  1,1,DEFAULT_PMT_PID,DEFAULT_VIDEO_PID,
                                  stream_type);
      if (err)
      {
        print_err("### esreverse: Error writing out TS program data\n");
        (void) close_input_as_ES(input_name,&es);
        if (as_TS)
          (void) tswrite_close(output.ts_output,TRUE);
        else if (had_output_name && !use_stdout)
        {
          err = fclose(output.es_output);
          if (err)
            fprint_err("### esreverse: (Error closing output file %s: %s)\n",
                       output_name,strerror(errno));
        }
        return 1;
      }
    }
  }

  if (is_data == VIDEO_H262)
    err = reverse_h262(es,output,max,frequency,as_TS,verbose,quiet);
  else
    err = reverse_access_units(es,output,max,frequency,as_TS,verbose,quiet);

  if (err)
  {
    print_err("### esreverse: Error reversing input\n");
    (void) close_input_as_ES(input_name,&es);
    if (as_TS)
      (void) tswrite_close(output.ts_output,TRUE);
    else if (had_output_name && !use_stdout)
    {
      err = fclose(output.es_output);
      if (err)
        fprint_err("### esreverse: (Error closing output file %s: %s)\n",
                   output_name,strerror(errno));
    }
    return 1;
  }

  // And tidy up when we're finished
  if (as_TS)
  {
    err = tswrite_close(output.ts_output,quiet);
    if (err)
    {
      fprint_err("### esreverse: Error closing output file %s",
                 output_name);
      (void) close_input_as_ES(input_name,&es);
      return 1;
    }
  }
  else if (!use_stdout)
  {
    errno = 0;
    err = fclose(output.es_output);
    if (err)
    {
      fprint_err("### esreverse: Error closing output file %s: %s\n",
                 output_name,strerror(errno));
      (void) close_input_as_ES(input_name,&es);
      return 1;
    }
  }

  err = close_input_as_ES(input_name,&es);
  if (err)
  {
    print_err("### esreverse: Error closing input file\n");
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

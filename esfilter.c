/*
 * An example application, reading an H.264 ES and doing things with it.
 *
 * Incorporates code to output an ES as an H.222 transport stream (TS).
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
#include <io.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "es_fns.h"
#include "pes_fns.h"
#include "nalunit_fns.h"
#include "ts_fns.h"
#include "accessunit_fns.h"
#include "h262_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "tswrite_fns.h"
#include "filter_fns.h"
#include "version.h"

#define DEBUG 0

// Things this program can do - "actions"
enum actions
{
  ACTION_UNDEFINED,
  ACTION_COPY,
  ACTION_STRIP,
  ACTION_FILTER,
};
typedef enum actions ACTION;


/*
 * Copy the MPEG2 data to transport stream
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `as_TS`, then copy as transport stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int copy_h262(ES_p        es,
                     WRITER      output,
                     int         as_TS,
                     int         max,
                     int         quiet)
{
  int  err;
  int  count = 0;

  for (;;)
  {
    h262_item_p  item;

    err = find_next_h262_item(es,&item);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error copying NAL units\n");
      return err;
    }
    count++;

    if (as_TS)
      err = write_ES_as_TS_PES_packet(output.ts_output,item->unit.data,
                                      item->unit.data_len,DEFAULT_VIDEO_PID,
                                      DEFAULT_VIDEO_STREAM_ID);
    else
      err = write_ES_unit(output.es_output,&(item->unit));
    if (err)
    {
      print_err("### Error writing MPEG2 item\n");
      return err;
    }

    free_h262_item(&item);
    
    if (max > 0 && count >= max)
      break;
  }
  if (!quiet)
    fprint_msg("Copied %d MPEG2 item%s\n",count,(count==1?"":"s"));
  return 0;
}

/*
 * Output an H.262 picture, appropriately.
 *
 * - `output` is the output stream
 * - if `as_TS`, write as transport stream
 * - `picture` is the H.262 picture to write out
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int write_h262_picture(WRITER          output,
                              int             as_TS,
                              h262_picture_p  picture)
{
  int err;
  if (as_TS)
    err = write_h262_picture_as_TS(output.ts_output,picture,DEFAULT_VIDEO_PID);
  else
    err = write_h262_picture_as_ES(output.es_output,picture);
  if (err)
  {
    print_err("### Error writing out H.262 picture\n");
    return err;
  }
  return 0;
}

/*
 * Output just the I pictures.
 *
 * If `keep_P`, keep P pictures as well.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `as_TS`, write as transport stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `keep_p` is true, P pictures will be kept
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int strip_h262(ES_p    es,
                      WRITER  output,
                      int     as_TS,
                      int     max,
                      int     keep_p,
                      int     verbose,
                      int     quiet)
{
  int  err;
  int  count;
  h262_context_p         h262 = NULL;
  h262_filter_context_p  fcontext = NULL;

  // Keep a count of the pictures we encounter, regardless of picture type
  // (but note that, for the moment at least, we don't distinguish frame
  // and field pictures, which maybe we should)
  int  pictures_seen = 0;
  // And how many pictures (i.e., I pictures) we keep
  int  pictures_kept = 0;

  err = build_h262_context(es,&h262);
  if (err)
  {
    print_err("### Unable to build H.262 picture reading context\n");
    return 1;
  }
  
  err = build_h262_filter_context_strip(&fcontext,h262,keep_p);
  if (err)
  {
    print_err("### Unable to build filter context\n");
    free_h262_context(&h262);
    return 1;
  }

  for (count = 1; ; count++)
  {
    h262_picture_p   seq_hdr = NULL;
    h262_picture_p   picture = NULL;
    int              delta_pictures_seen;
    err = get_next_stripped_h262_frame(fcontext,verbose,quiet,
                                       &seq_hdr,&picture,&delta_pictures_seen);
    if (err == EOF)
    {
      if (!quiet) print_msg("EOF\n");
      break;
    }
    else if (err)
    {
      print_err("### Error getting next stripped picture\n");
      free_h262_filter_context(&fcontext);
      free_h262_context(&h262);
      return 1;
    }

    pictures_seen += delta_pictures_seen;
    pictures_kept ++;

    if (seq_hdr != NULL)
    {
      err = write_h262_picture(output,as_TS,seq_hdr);
      if (err)
      {
        print_err("### Error writing picture\n");
        free_h262_picture(&picture);
        free_h262_filter_context(&fcontext);
        free_h262_context(&h262);
        return 1;
      }
    }

    err = write_h262_picture(output,as_TS,picture);
    if (err)
    {
      print_err("### Error writing picture\n");
      free_h262_picture(&picture);
      free_h262_filter_context(&fcontext);
      free_h262_context(&h262);
      return 1;
    }
    free_h262_picture(&picture);

    if (max > 0 && count >= max)
    {
      if (!quiet)
        fprint_msg("Ending after %d pictures\n",count);
      break;
    }
  }
  
  free_h262_filter_context(&fcontext);
  free_h262_context(&h262);
  
  if (!quiet)
  {
    fprint_msg("Found %d frames, kept %d (%.1f%%)\n",
               pictures_seen,pictures_kept,
               100.0*pictures_kept/pictures_seen);
  }
  return 0;
}

/*
 * Filter the MPEG2 data, keeping just the I pictures, but aiming for
 * an "apparent" kept frequency as stated.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `as_TS`, write as transport stream
 * - `frequency` says how often we would like retained pictures to occur,
 *   ideally - i.e., try to keep every <frequency>th picture. The effect
 *   should be similar to viewing the video stream at a speed up of
 *   `frequency` times.
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int filter_h262(ES_p        es,
                       WRITER      output,
                       int         as_TS,
                       int         frequency,
                       int         max,
                       int         verbose,
                       int         quiet)
{
  int  err;
  int  count = 0;
  h262_context_p         h262 = NULL;
  h262_filter_context_p  fcontext = NULL;

  // Keep a count of the pictures we encounter, regardless of picture type
  // (but note that, for the moment at least, we don't distinguish frame
  // and field pictures, which maybe we should)
  int  pictures_seen = 0;
  // And how many pictures (i.e., I pictures) we keep
  int  pictures_kept = 0;
  // And how many we wrote
  int  pictures_written = 0;

  h262_picture_p   this_picture = NULL;
  h262_picture_p   last_picture = NULL;
  h262_picture_p   seq_hdr = NULL;  // *We* mustn't free this one

  err = build_h262_context(es,&h262);
  if (err)
  {
    print_err("### Unable to build H.262 picture reading context\n");
    return 1;
  }

  err = build_h262_filter_context(&fcontext,h262,frequency);
  if (err)
  {
    print_err("### Unable to build filter context\n");
    free_h262_context(&h262);
    return 1;
  }
  
  for (count = 1; ; count++)
  {
    int  delta_pictures_seen;
    err = get_next_filtered_h262_frame(fcontext,verbose,quiet,&seq_hdr,
                                       &this_picture,&delta_pictures_seen);
    if (err == EOF)
    {
      free_h262_picture(&last_picture);
      break;
    }
    else if (err)
    {
      print_err("### Error getting next filtered picture\n");
      free_h262_picture(&last_picture);
      free_h262_filter_context(&fcontext);
      free_h262_context(&h262);
      return 1;
    }

    pictures_seen += delta_pictures_seen;

    if (this_picture == NULL)
    {
      // We need to repeat the last picture
      this_picture = last_picture;
      last_picture = NULL;
    }
    else
      pictures_kept ++;

    if (seq_hdr != NULL)
    {
      err = write_h262_picture(output,as_TS,seq_hdr);
      if (err)
      {
        print_err("### Error writing sequence header\n");
        free_h262_picture(&this_picture);
        free_h262_picture(&last_picture);
        free_h262_filter_context(&fcontext);
        free_h262_context(&h262);
        return 1;
      }
    }

    if (this_picture != NULL)
    {
      err = write_h262_picture(output,as_TS,this_picture);
      if (err)
      {
        print_err("### Error writing picture\n");
        free_h262_picture(&this_picture);
        free_h262_picture(&last_picture);
        free_h262_filter_context(&fcontext);
        free_h262_context(&h262);
        return 1;
      }
      pictures_written ++;
    }

    free_h262_picture(&last_picture);
    last_picture = this_picture;
    
    if (max > 0 && count >= max)
    {
      if (!quiet)
        fprint_msg("Ending after %d frames\n",count);
      free_h262_picture(&this_picture);
      break;
    }
  }
  
  free_h262_filter_context(&fcontext);
  free_h262_context(&h262);

  if (!quiet)
  {
    print_msg("\n");
    print_msg("Summary\n");
    print_msg("=======\n");
    print_msg("                   Found       Kept            Written\n");
    fprint_msg("Frames      %10d %10d (%4.1f%%) %10d (%4.1f%%)\n",
               pictures_seen,pictures_kept,
               100*(((double)pictures_kept)/pictures_seen),
               pictures_written,
               100*(((double)pictures_written)/pictures_seen));
    if (frequency != 0)
      fprint_msg("Target (frames)     .  %10d (%4.1f%%) at requested"
                 " frequency %d\n",pictures_seen/frequency,
                 100.0/frequency,frequency);
  }
  return 0;
}

/*
 * Copy the data as NAL units.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int copy_nal_units(ES_p                es,
                          WRITER              output,
                          int                 as_TS,
                          int                 max,
                          int                 verbose,
                          int                 quiet)
{
  int err = 0;
  nal_unit_context_p  context = NULL;

  err = build_nal_unit_context(es,&context);
  if (err)
  {
    print_err("### Unable to build NAL unit context to read ES\n");
    return 1;
  }
  
  for (;;)
  {
    nal_unit_p  nal;

    if (max > 0 && context->count >= max)
      break;

    err = find_next_NAL_unit(context,verbose,&nal);
    if (err == EOF)
      break;
    else if (err == 2)
    {
      print_err("!!! Ignoring broken NAL unit\n");
      continue;
    }
    else if (err)
    {
      print_err("### Error getting next NAL unit\n");
      free_nal_unit_context(&context);
      return err;
    }

    if (as_TS)
      err = write_NAL_unit_as_TS(output.ts_output,nal,DEFAULT_VIDEO_PID);
    else
      err = write_NAL_unit_as_ES(output.es_output,nal);
    if (err)
    {
      free_nal_unit(&nal);
      print_err("### Error copying NAL units\n");
      free_nal_unit_context(&context);
      return err;
    }

    free_nal_unit(&nal);
  }
  if (!quiet)
    fprint_msg("Processed %d NAL unit%s\n",
               context->count,(context->count==1?"":"s"));
  free_nal_unit_context(&context);
  return 0;
}

/*
 * Output just IDR, I and maybe P access units.
 *
 * Access units are kept if they are reference frames, and if they are
 * IDR frames, or all of their slices are I (or maybe P).
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int strip_access_units(ES_p                   es,
                              WRITER                 output,
                              int                    as_TS,
                              int                    max,
                              int                    keep_all_ref,
                              int                    verbose,
                              int                    quiet)
{
  int err = 0;
  int count;
  access_unit_context_p  acontext = NULL;
  h264_filter_context_p  fcontext = NULL;

  // It's nice to output some statistics at the end
  int access_units_seen = 0;
  int access_units_kept = 0;

  err = build_access_unit_context(es,&acontext);
  if (err)
  {
    print_err("### Unable to build access unit context\n");
    return 1;
  }
  err = build_h264_filter_context_strip(&fcontext,acontext,keep_all_ref);
  if (err)
  {
    print_err("### Unable to build filter context\n");
    free_access_unit_context(&acontext);
    return 1;
  }

  for (count = 1; ; count++)
  {
    access_unit_p  access_unit = NULL;
    int            delta_access_units_seen;
    err = get_next_stripped_h264_frame(fcontext,verbose,quiet,
                                       &access_unit,
                                       &delta_access_units_seen);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next stripped picture\n");
      free_h264_filter_context(&fcontext);
      free_access_unit_context(&acontext);
      return 1;
    }

    access_units_seen += delta_access_units_seen;
    access_units_kept ++;

    if (as_TS)
      err = write_access_unit_as_TS(access_unit,fcontext->access_unit_context,
                                    output.ts_output,DEFAULT_VIDEO_PID);
    else
      err = write_access_unit_as_ES(access_unit,fcontext->access_unit_context,
                                    output.es_output);
    if (err)
    {
      print_err("### Error writing picture\n");
      free_h264_filter_context(&fcontext);
      free_access_unit_context(&acontext);
      return 1;
    }
    free_access_unit(&access_unit);

    if (max > 0 && count >= max)
    {
      if (!quiet)
        fprint_msg("Ending after %d frames\n",count);
      break;
    }
  }

  free_h264_filter_context(&fcontext);
  free_access_unit_context(&acontext);
  
  if (!quiet)
  {
    print_msg("\n");
    print_msg("Summary\n");
    print_msg("=======\n");
    print_msg("                  Found    Written\n");
    fprint_msg("Access units %10d %10d (%4.1f%%)\n",
               access_units_seen,
               access_units_kept,
               100*(((double)access_units_kept)/access_units_seen));
  }
  return 0;
}

/*
 * Filter out access units, aiming to keep one every `frequency`.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int filter_access_units(ES_p               es,
                               WRITER             output,
                               int                as_TS,
                               int                max,
                               int                frequency,
                               int                verbose,
                               int                quiet)
{
  int err = 0;
  int count;
  access_unit_context_p  acontext = NULL;
  h264_filter_context_p  fcontext = NULL;

  // It's nice to output some statistics at the end
  int access_units_seen = 0;
  int access_units_kept = 0;
  int access_units_written = 0;

  access_unit_p  this_access_unit = NULL;
  access_unit_p  last_access_unit = NULL;
  
  err = build_access_unit_context(es,&acontext);
  if (err)
  {
    print_err("### Unable to build access unit context\n");
    return 1;
  }
  err = build_h264_filter_context(&fcontext,acontext,frequency);
  if (err)
  {
    print_err("### Unable to build filter context\n");
    free_access_unit_context(&acontext);
    return 1;
  }

  for (count = 1; ; count++)
  {
    int  delta_access_units_seen;
    err = get_next_filtered_h264_frame(fcontext,verbose,quiet,
                                       &this_access_unit,
                                       &delta_access_units_seen);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next filtered picture\n");
      free_access_unit(&last_access_unit);
      free_h264_filter_context(&fcontext);
      free_access_unit_context(&acontext);
      return 1;
    }

    access_units_seen += delta_access_units_seen;

    if (this_access_unit == NULL)
    {
      // We need to repeat the last access unit
      this_access_unit = last_access_unit;
      last_access_unit = NULL;
    }
    else
      access_units_kept ++;

    if (this_access_unit != NULL)
    {
      if (as_TS)
        err = write_access_unit_as_TS(this_access_unit,
                                      fcontext->access_unit_context,
                                      output.ts_output,DEFAULT_VIDEO_PID);
      else
        err = write_access_unit_as_ES(this_access_unit,
                                      fcontext->access_unit_context,
                                      output.es_output);
      if (err)
      {
        print_err("### Error writing picture\n");
        free_access_unit(&this_access_unit);
        free_access_unit(&last_access_unit);
        free_h264_filter_context(&fcontext);
        free_access_unit_context(&acontext);
        return 1;
      }
      access_units_written ++;
    }

    free_access_unit(&last_access_unit);
    last_access_unit = this_access_unit;
    
    if (max > 0 && count >= max)
    {
      if (!quiet)
        fprint_msg("Ending after %d frames\n",count);
      free_access_unit(&this_access_unit);
      break;
    }
  }

  free_h264_filter_context(&fcontext);
  free_access_unit_context(&acontext);

  if (!quiet)
  {
    print_msg("\n");
    print_msg("Summary\n");
    print_msg("=======\n");
    print_msg("            Found       Kept            Written\n");
    fprint_msg("Frames %10d %10d (%4.1f%%) %10d (%4.1f%%)\n",
               access_units_seen,
               access_units_kept,
               100*(((double)access_units_kept)/access_units_seen),
               access_units_written,
               100*(((double)access_units_written)/access_units_seen));
    if (frequency != 0)
      fprint_msg("Target (frames) . %10d (%4.1f%%) at requested"
                 " frequency %d\n",access_units_seen/frequency,
                 100.0/frequency,frequency);
  }
  return 0;
}

/*
 * Perform whatever action we have been requested to do on the input
 * stream.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int do_action(ACTION   action,
                     ES_p     es,
                     WRITER   output,
                     int      max,
                     int      frequency,
                     int      is_h262,
                     int      as_TS,
                     int      keep_all_ref,
                     byte     stream_type,
                     int      verbose,
                     int      quiet)
{
  int err  = 0;

  // If we're writing Transport Stream, start with the PAT and PMT
  if (as_TS)
  {
    if (!quiet)
      fprint_msg("Using transport stream id 1, PMT PID %#x, program 1 ="
                 " PID %#x, stream type %#x\n",DEFAULT_PMT_PID,DEFAULT_VIDEO_PID,
                 stream_type);
    err = write_TS_program_data(output.ts_output,1,1,
                                DEFAULT_PMT_PID,DEFAULT_VIDEO_PID,stream_type);
    if (err) return 1;
  }
    
  switch (action)
  {
  case ACTION_FILTER:
    if (is_h262)
      err = filter_h262(es,output,as_TS,frequency,max,verbose,quiet);
    else
      err = filter_access_units(es,output,as_TS,max,frequency,verbose,quiet);
    break;

  case ACTION_STRIP:
    if (is_h262)
      err = strip_h262(es,output,as_TS,max,keep_all_ref,verbose,quiet);
    else
      err = strip_access_units(es,output,as_TS,max,keep_all_ref,verbose,quiet);
    break;

  case ACTION_COPY:
    if (is_h262)
      err = copy_h262(es,output,as_TS,max,quiet);
    else
      err = copy_nal_units(es,output,as_TS,max,verbose,quiet);
    break;
    
  default:
    fprint_err("### Unexpected action %d\n",action);
    err = 1;
    break;
  }
  return err;
}

static void print_usage()
{
  print_msg(
    "Usage: esfilter [actions/switches] [<infile>] [<outfile>]\n"
    "\n"
    );
  REPORT_VERSION("esfilter");
  print_msg(
    "\n"
    "  Output a filtered or truncated version of an elementary stream.\n"
    "  The input is either H.264 (MPEG-4/AVC) or H.262 (MPEG-2).\n"
    "  The output is either an elementary stream, or an H.222 transport\n"
    "  stream\n"
    "\n"
    "  If output is to an H.222 Transport Stream, then fixed values for\n"
    "  the PMT PID (0x66) and video PID (0x68) are used.\n"
    "\n"
    "Files:\n"
    "  <infile>  is the input elementary stream (but see -stdin below).\n"
    "  <outfile> is the output stream, either an equivalent elementary\n"
    "            stream, or an H.222 Transport Stream (but see -stdout\n"
    "            and -host below).\n"
    "\n"
    "Actions:\n"
    "  -copy     Copy the input data to the output file\n"
    "            (mostly useful as a way of truncating data with -max)\n"
    "  -filter   Filter data from input to output, aiming to keep every\n"
    "            <n>th frame (where <n> is specified by -freq).\n"
    "  -strip    For H.264, output just the IDR and I pictures, for H.262,\n"
    "            output just the I pictures, but see -allref below.\n"
    "\n"
    "Switches:\n"
    "  -verbose, -v      Output extra (debugging) messages\n"
    "  -quiet, -q        Only output error messages\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -stdin            Take input from <stdin>, instead of a named file\n"
    "  -stdout           Write output to <stdout>, instead of a named file\n"
    "                    Forces -quiet and -err stderr.\n"
    "  -host <host>, -host <host>:<port>\n"
    "                    Writes output (over TCP/IP) to the named <host>,\n"
    "                    instead of to a named file. If <port> is not\n"
    "                    specified, it defaults to 88. Implies -tsout.\n"
    "  -max <n>, -m <n>  Maximum number of frames to read (for -filter\n"
    "                    and -strip), or ES units/NAL units (for -copy).\n"
    "  -freq <n>         Specify the frequency of frames to try to keep\n"
    "                    with -filter. Defaults to 8.\n"
    "  -allref           With -strip, keep all reference pictures (H.264)\n"
    "                    or all I and P pictures (H.262)\n"
    "  -tsout            Output data as Transport Stream PES packets\n"
    "                    (the default is as Elementary Stream)\n"
    "  -pes, -ts         The input file is TS or PS, to be read via the\n"
    "                    PES->ES reading mechanisms. Not allowed with -stdin.\n"
    "\n"
    "Stream type:\n"
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
    );
}

int main(int argc, char **argv)
{
  char  *input_name = NULL;
  char  *output_name = NULL;
  int    had_input_name = FALSE;
  int    had_output_name = FALSE;
  char  *action_switch = "None";
  int    use_stdin = FALSE;
  int    use_stdout = FALSE;
  int    use_tcpip = FALSE;
  int    port = 88; // Useful default port number
  int    err = 0;
  ES_p   es = NULL;
  WRITER output;
  int    max = 0;
  ACTION action = ACTION_UNDEFINED;
  int    as_TS = FALSE;
  int    keep_all_ref = FALSE;
  int    frequency = 8; // The default as stated in the usage
  int    quiet = FALSE;
  int    verbose = FALSE;
  int    ii = 1;

  int    use_pes = FALSE;

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
      else if (!strcmp("-copy",argv[ii]))
      {
        action = ACTION_COPY;
        action_switch = argv[ii];
      }
      else if (!strcmp("-filter",argv[ii]))
      {
        action = ACTION_FILTER;
        action_switch = argv[ii];
      }
      else if (!strcmp("-strip",argv[ii]))
      {
        action = ACTION_STRIP;
        action_switch = argv[ii];
      }
      else if (!strcmp("-tsout",argv[ii]))
        as_TS = TRUE;
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
        CHECKARG("esfilter",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### esfilter: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-host",argv[ii]))
      {
        CHECKARG("esfilter",ii);
        err = host_value("esfilter",argv[ii],argv[ii+1],&output_name,&port);
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
      else if (!strcmp("-allref",argv[ii]))
      {
        keep_all_ref = TRUE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("esfilter",ii);
        err = int_value("esfilter",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-freq",argv[ii]))
      {
        CHECKARG("esfilter",ii);
        err = int_value("esfilter",argv[ii],argv[ii+1],TRUE,10,&frequency);
        if (err) return 1;
        ii++;
      }
      else
      {
        fprint_err("### esfilter: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name && had_output_name)
      {
        fprint_err("### esfilter: Unexpected '%s'\n",argv[ii]);
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
    print_err("### esfilter: No input file specified\n");
    return 1;
  }
  if (!had_output_name)
  {
    print_err("### esfilter: No output file specified\n");
    return 1;
  }
  if (action == ACTION_UNDEFINED)
  {
    print_err("### esfilter: No action specified (-copy, -strip,"
              " -filter)\n");
    return 1;
  }

  // Try to stop extraneous data ending up in our output stream
  if (use_stdout)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  err = open_input_as_ES((use_stdin?NULL:input_name),use_pes,quiet,
                         force_stream_type,want_data,&is_data,&es);
  if (err)
  {
    print_err("### esfilter: Error opening input file\n");
    return 1;
  }

  // If we're reading via PES, then we can ignore all but the video
  // - this may make things slightly faster, and will allow us to ignore
  // any errors in the non-video packets
  if (use_pes)
    set_PES_reader_video_only(es->reader,TRUE);

  if (is_data == VIDEO_H262)
    stream_type = MPEG2_VIDEO_STREAM_TYPE;
  else if (is_data == VIDEO_H264)
    stream_type = AVC_VIDEO_STREAM_TYPE;
  else
  {
    print_err("### esfilter: Unexpected type of video data\n");
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
      fprint_err("### esfilter: Unable to open %s\n",output_name);
      (void) close_input_as_ES(input_name,&es);
      return 1;
    }
  }
  else
  {
    output.es_output = fopen(output_name,"wb");
    if (output.es_output == NULL)
    {
      fprint_err("### esfilter: Unable to open output file %s: %s\n",
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
    if (action == ACTION_FILTER)
      fprint_msg("Filtering freqency %d\n",frequency);
    if (action == ACTION_STRIP)
    {
      if (want_data == VIDEO_H262)
      {
        if (keep_all_ref)
          print_msg("Just keeping I and P pictures\n");
        else
          print_msg("Just keep I pictures\n");
      }
      else
      {
        if (keep_all_ref)
          print_msg("Just keeping reference pictures\n");
        else
          print_msg("Just keep IDR and I pictures\n");
      }
    }
    if (max)
      fprint_msg("Stopping as soon after %d NAL units as possible\n",max);
  }

  err = do_action(action,es,output,max,frequency,want_data==VIDEO_H262,
                  as_TS,keep_all_ref,stream_type,verbose,quiet);
  if (err)
  {
    fprint_err("### esfilter: Error doing '%s'\n",action_switch);
    (void) close_input_as_ES(input_name,&es);
    if (as_TS)
      (void) tswrite_close(output.ts_output,TRUE);
    else if (had_output_name && !use_stdout)
    {
      err = fclose(output.es_output);
      if (err)
        fprint_err("### esfilter: (Error closing output file %s: %s)\n",
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
      fprint_err("### esfilter: Error closing output file %s",output_name);
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
      fprint_err("### esfilter: Error closing output file %s: %s\n",
                 output_name,strerror(errno));
      (void) close_input_as_ES(input_name,&es);
      return 1;
    }
  }

  err = close_input_as_ES(input_name,&es);
  if (err)
  {
    print_err("### esfilter: Error closing input file\n");
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

/*
 * Report on the contents of an H.264 (MPEG-4/AVC) or H.262 (MPEG-2)
 * elementary stream.
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
#ifdef _WIN32
#include <io.h>
#else  // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "es_fns.h"
#include "nalunit_fns.h"
#include "ts_fns.h"
#include "pes_fns.h"
#include "accessunit_fns.h"
#include "h262_fns.h"
#include "avs_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"

#define FRAMES_PER_SECOND  25
#define FRAMES_PER_MINUTE  (FRAMES_PER_SECOND * 60)


/*
 * Report on the content of an AVS file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` AVS items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - if `count_sizes` is true, then a summary of frame sizes will be kept
 */
static void report_avs_frames(ES_p    es,
                              int     max,
                              int     verbose,
                              int     quiet,
                              int     count_sizes)
{
  int  err;
  int  count = 0;
  int  num_frames = 0;
  int  num_sequence_headers = 0;
  int  num_sequence_ends = 0;

  uint32_t min_frame_size = 1000000;
  uint32_t max_frame_size = 0;
  uint32_t sum_frame_size = 0;

  // I, P, B = 0, 1, 2 (we "make up" picture coding type 0 for I frames)
  uint32_t min_x_frame_size[3] = {1000000,1000000,1000000};
  uint32_t max_x_frame_size[3] = {0,0,0};
  uint32_t sum_x_frame_size[3] = {0,0,0};
  int     num_x_frames[3] = {0,0,0};

  uint32_t min_seq_hdr_size = 1000000;
  uint32_t max_seq_hdr_size = 0;
  uint32_t sum_seq_hdr_size = 0;

  ES_offset  start;
  uint32_t   length;
  
  avs_context_p  avs;

  err = build_avs_context(es,&avs);
  if (err)
  {
    print_err("### Error trying to build AVS reader from ES reader\n");
    return;
  }

  for (;;)
  {
    avs_frame_p  frame;

    err = get_next_avs_frame(avs,verbose,quiet,&frame);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next AVS frame\n");
      break;
    }
    count++;

    if (!quiet)
      report_avs_frame(frame,FALSE);
    else if (verbose)
      report_avs_frame(frame,TRUE);

    if (frame->is_frame)
    {
      if (count_sizes)
      {
        err = get_ES_unit_list_bounds(frame->list,&start,&length);
        if (err) break;
        if (min_frame_size > length) min_frame_size = length;
        if (max_frame_size < length) max_frame_size = length;
        sum_frame_size += length;
        if (frame->picture_coding_type < 3)  // paranoia - check in array bounds
        {
          // I, P or B -- even though there isn't a "real" picture coding type
          // for I, we forge one when we read the frame
          int ii = frame->picture_coding_type;
          num_x_frames[ii] ++;
          if (min_x_frame_size[ii] > length) min_x_frame_size[ii] = length;
          if (max_x_frame_size[ii] < length) max_x_frame_size[ii] = length;
          sum_x_frame_size[ii] += length;
        }
      }
      num_frames ++;
      if (frame->picture_coding_type < 3)  // paranoia - check in array bounds
        num_x_frames[frame->picture_coding_type] ++;
    }
    else if (frame->is_sequence_header)
    {
      if (count_sizes)
      {
        err = get_ES_unit_list_bounds(frame->list,&start,&length);
        if (err) break;
        if (min_seq_hdr_size > length) min_seq_hdr_size = length;
        if (max_seq_hdr_size < length) max_seq_hdr_size = length;
        sum_seq_hdr_size += length;
      }
      num_sequence_headers ++;
    }
    else
      num_sequence_ends ++;

    free_avs_frame(&frame);
    
    if (max > 0 && count >= max)
      break;
  }
  free_avs_context(&avs);

  fprint_msg("Found %d AVS 'frame'%s:\n"
             "   %5d frame%s (%d I, %d P, %d B)\n"
             "   %5d sequence header%s\n"
             "   %5d sequence end%s\n",
             count,(count==1?"":"s"),
             num_frames,(num_frames==1?"":"s"),
             num_x_frames[AVS_I_PICTURE_CODING],
             num_x_frames[AVS_P_PICTURE_CODING],
             num_x_frames[AVS_B_PICTURE_CODING],
             num_sequence_headers,(num_sequence_headers==1?"":"s"),
             num_sequence_ends,(num_sequence_ends==1?"":"s"));
  
  {
    double total_seconds = num_frames / (double)FRAMES_PER_SECOND;
    int    minutes = (int)(total_seconds / 60);
    double seconds = total_seconds - 60*minutes;
    fprint_msg("At 25 frames/second, that is %dm %.1fs (%.2fs)\n",minutes,seconds,
               total_seconds);
  }

  if (count_sizes)
  {
    int ii;
    if (num_frames > 0)
      fprint_msg("Frame sizes ranged from %5u to %7u bytes, mean %9.2f\n",
                 min_frame_size,max_frame_size,
                 sum_frame_size/(double)num_frames);
    for (ii = 0; ii < 3; ii++)
    {
      if (num_x_frames[ii] > 0)
        fprint_msg("          %s frames from %5u to %7u bytes, mean %9.2f\n",
                   (ii==0?"I":
                    ii==1?"P":
                    ii==2?"B":"?"),
                   min_x_frame_size[ii],max_x_frame_size[ii],
                   sum_x_frame_size[ii]/(double)num_x_frames[ii]);
    }
    if (num_sequence_headers > 0)
    {
      if (min_seq_hdr_size == max_seq_hdr_size)
        fprint_msg("Sequence headers were all %u bytes\n",min_seq_hdr_size);
      else
        fprint_msg("Sequence headers    from %5u to %7u bytes, mean %9.2f\n",
                   min_seq_hdr_size,max_seq_hdr_size,
                   sum_seq_hdr_size/(double)num_sequence_headers);
    }
  }
}

/*
 * Report on the ES units in a file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 */
static void report_ES_units(ES_p    es,
                            int     max,
                            int     verbose,
                            int     quiet)
{
  int  err;
  int  count = 0;
  struct ES_unit unit;

  (void) setup_ES_unit(&unit);

  for (;;)
  {
    err = find_next_ES_unit(es,&unit);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error finding next ES unit\n");
      break;
    }
    count++;

    if (!quiet)
      report_ES_unit(TRUE,&unit);

    if (verbose)
      print_data(TRUE,"        Data",
                 unit.data,unit.data_len,10);
    
    if (max > 0 && count >= max)
      break;
  }
  clear_ES_unit(&unit);
  fprint_msg("Found %d ES unit%s\n",count,(count==1?"":"s"));
}

/*
 * Report on the content of an MPEG2 file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 */
static void find_h262_fields(ES_p    es,
                             int     max,
                             int     verbose)
{
  int  err;
  int  count = 0;
  int  num_fields = 0;
  int  num_frames = 0;
  int  num_sequence_headers = 0;
  int  num_sequence_ends = 0;
  h262_context_p  h262;

  err = build_h262_context(es,&h262);
  if (err)
  {
    print_err("### Error trying to build H.262 reader from ES reader\n");
    return;
  }

  for (;;)
  {
    h262_picture_p  picture;

    err = get_next_h262_single_picture(h262,verbose,&picture);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next H.262 picture\n");
      break;
    }
    count++;

    if (picture->is_picture)
    {
      if (picture->picture_structure < 3)
      {
        report_h262_picture(picture,verbose);
        num_fields ++;
      }
      else
        num_frames ++;
    }
    else if (picture->is_sequence_header)
      num_sequence_headers ++;
    else
      num_sequence_ends ++;

    free_h262_picture(&picture);
    
    if (max > 0 && count >= max)
      break;
  }
  free_h262_context(&h262);

  fprint_msg("Found %d MPEG-2 'picture'%s:\n"
             "   %5d field%s\n"
             "   %5d frame%s\n"
             "   %5d sequence header%s\n"
             "   %5d sequence end%s\n",
             count,(count==1?"":"s"),
             num_fields,(num_fields==1?"":"s"),
             num_frames,(num_frames==1?"":"s"),
             num_sequence_headers,(num_sequence_headers==1?"":"s"),
             num_sequence_ends,(num_sequence_ends==1?"":"s"));
}

/*
 * Report on the content of an MPEG2 file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - if `count_sizes` is true, then a summary of frame sizes will be kept
 */
static void report_h262_frames(ES_p    es,
                               int     max,
                               int     verbose,
                               int     quiet,
                               int     count_sizes)
{
  int  err;
  int  count = 0;
  int  num_frames = 0;
  int  num_sequence_headers = 0;
  int  num_sequence_ends = 0;

  uint32_t min_frame_size = 1000000;
  uint32_t max_frame_size = 0;
  uint32_t sum_frame_size = 0;

  // I=1, P=2, B=3, D=4 -- so subtract one before using the picture coding type
  // as an index into the arrays...
  uint32_t min_x_frame_size[4] = {1000000,1000000,1000000,1000000};
  uint32_t max_x_frame_size[4] = {0,0,0,0};
  uint32_t sum_x_frame_size[4] = {0,0,0,0};
  int     num_x_frames[4] = {0,0,0,0};

  uint32_t min_seq_hdr_size = 1000000;
  uint32_t max_seq_hdr_size = 0;
  uint32_t sum_seq_hdr_size = 0;

  ES_offset  start;
  uint32_t   length;
  
  h262_context_p  h262;

  err = build_h262_context(es,&h262);
  if (err)
  {
    print_err("### Error trying to build H.262 reader from ES reader\n");
    return;
  }

  for (;;)
  {
    h262_picture_p  picture;

    err = get_next_h262_frame(h262,verbose,quiet,&picture);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next H.262 picture\n");
      break;
    }
    count++;

    if (!quiet)
      report_h262_picture(picture,FALSE);
    else if (verbose)
      report_h262_picture(picture,TRUE);

    if (picture->is_picture)
    {
      if (count_sizes)
      {
        err = get_ES_unit_list_bounds(picture->list,&start,&length);
        if (err) break;
        if (min_frame_size > length) min_frame_size = length;
        if (max_frame_size < length) max_frame_size = length;
        sum_frame_size += length;
        if (picture->picture_coding_type < 5 &&
            picture->picture_coding_type > 0)  // paranoia - check for array bounds
        {
          // I, P, B or D frame
          int ii = picture->picture_coding_type - 1;
          if (min_x_frame_size[ii] > length) min_x_frame_size[ii] = length;
          if (max_x_frame_size[ii] < length) max_x_frame_size[ii] = length;
          sum_x_frame_size[ii] += length;
        }
      }
      num_frames ++;
        if (picture->picture_coding_type < 5 &&
            picture->picture_coding_type > 0)  // paranoia - check for array bounds
        num_x_frames[picture->picture_coding_type - 1] ++;
    }
    else if (picture->is_sequence_header)
    {
      if (count_sizes)
      {
        err = get_ES_unit_list_bounds(picture->list,&start,&length);
        if (err) break;
        if (min_seq_hdr_size > length) min_seq_hdr_size = length;
        if (max_seq_hdr_size < length) max_seq_hdr_size = length;
        sum_seq_hdr_size += length;
      }
      num_sequence_headers ++;
    }
    else
      num_sequence_ends ++;

    free_h262_picture(&picture);
    
    if (max > 0 && count >= max)
      break;
  }
  free_h262_context(&h262);

  fprint_msg("Found %d MPEG-2 'picture'%s:\n"
             "   %5d frame%s (%d I, %d P, %d B, %d D)\n"
             "   %5d sequence header%s\n"
             "   %5d sequence end%s\n",
             count,(count==1?"":"s"),
             num_frames,(num_frames==1?"":"s"),
             num_x_frames[0],
             num_x_frames[1],
             num_x_frames[2],
             num_x_frames[3],
             num_sequence_headers,(num_sequence_headers==1?"":"s"),
             num_sequence_ends,(num_sequence_ends==1?"":"s"));

  {
    double total_seconds = num_frames / (double)FRAMES_PER_SECOND;
    int    minutes = (int)(total_seconds / 60);
    double seconds = total_seconds - 60*minutes;
    fprint_msg("At 25 frames/second, that is %dm %.1fs (%.2fs)\n",minutes,seconds,
               total_seconds);
  }

  if (count_sizes)
  {
    int ii;
    if (num_frames > 0)
      fprint_msg("Frame sizes ranged from %5u to %7u bytes, mean %9.2f\n",
                 min_frame_size,max_frame_size,
                 sum_frame_size/(double)num_frames);
    for (ii = 0; ii < 4; ii++)
    {
      if (num_x_frames[ii] > 0)
        fprint_msg("          %s frames from %5u to %7u bytes, mean %9.2f\n",
                   H262_PICTURE_CODING_STR(ii),
                   min_x_frame_size[ii],max_x_frame_size[ii],
                   sum_x_frame_size[ii]/(double)num_x_frames[ii]);
    }
    if (num_sequence_headers > 0)
    {
      if (min_seq_hdr_size == max_seq_hdr_size)
        fprint_msg("Sequence headers were all %u bytes\n",min_seq_hdr_size);
      else
        fprint_msg("Sequence headers    from %5u to %7u bytes, mean %9.2f\n",
                   min_seq_hdr_size,max_seq_hdr_size,
                   sum_seq_hdr_size/(double)num_sequence_headers);
    }
  }
}

/*
 * Report on changes in AFD in an MPEG2 file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 */
static void report_h262_afds(ES_p    es,
                             int     max,
                             int     verbose,
                             int     quiet)
{
  int  err;
  int  frames = 0;
  byte afd = 0;  // not '1000', so we see the first value
  h262_context_p  h262;
  int  report_every = 5 * FRAMES_PER_MINUTE;

  err = build_h262_context(es,&h262);
  if (err)
  {
    print_err("### Error trying to build H.262 reader from ES reader\n");
    return;
  }

  for (;;)
  {
    h262_picture_p  picture;

    err = get_next_h262_frame(h262,verbose,quiet,&picture);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next H.262 picture\n");
      break;
    }

    if (picture->is_picture)
    {
      // NB: the time at which the frame *starts*
      if (frames % report_every == 0)
        fprint_msg("%d minute%s\n",frames/FRAMES_PER_MINUTE,
                   (frames/FRAMES_PER_MINUTE==1?"":"s"));
      frames ++;
    }
    
    if (picture->is_picture && picture->afd != afd)
    {
      double total_seconds = frames / (double)FRAMES_PER_SECOND;
      int    minutes = (int)(total_seconds / 60);
      double seconds = total_seconds - 60*minutes;
      fprint_msg("%dm %4.1fs (frame %d @ %.2fs): ",minutes,seconds,
                 frames,total_seconds);
      report_h262_picture(picture,FALSE);
      afd = picture->afd;
    }

    free_h262_picture(&picture);
    
    if (max > 0 && frames >= max)
      break;
  }
  free_h262_context(&h262);

  {
    double total_seconds = frames / (double)FRAMES_PER_SECOND;
    int    minutes = (int)(total_seconds / 60);
    double seconds = total_seconds - 60*minutes;
    fprint_msg("Found %d MPEG-2 frame%s",frames,(frames==1?"":"s"));
    fprint_msg(" which is %dm %.1fs (%.2fs)\n",minutes,seconds,total_seconds);
  }
}

/*
 * Report on the content of an MPEG2 file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 */
static void report_h262_items(ES_p    es,
                              int     max,
                              int     verbose,
                              int     quiet)
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
      print_err("### Error finding next H.262 item\n");
      break;
    }
    count++;

    if (!quiet)
      report_h262_item(item);

    if (verbose)
      print_data(TRUE,"        Data",
                 item->unit.data,item->unit.data_len,10);
    free_h262_item(&item);
    
    if (max > 0 && count >= max)
      break;
  }
  fprint_msg("Found %d MPEG-2 item%s\n",count,(count==1?"":"s"));
}

/*
 * Report on the data by NAL units.
 */
static void report_by_nal_unit(ES_p   es,
                               int    max,
                               int    quiet,
                               int    show_nal_details)
{
  int err = 0;

  nal_unit_context_p  context = NULL;

  int ref_idcs[4] = {0};    // values 0,1,2,3
  int unit_types[15] = {0};
  int slice_types[10] = {0};

  err = build_nal_unit_context(es,&context);
  if (err)
  {
    print_err("### Unable to build NAL unit context to read ES\n");
    return;
  }

  if (show_nal_details)
    set_show_nal_reading_details(context,TRUE);

  for (;;)
  {
    nal_unit_p  nal;

    if (max > 0 && context->count >= max)
    {
      fprint_msg("\nStopping because %d NAL units have been read\n",
                 context->count);
      break;
    }

    err = find_next_NAL_unit(context,!quiet,&nal);
    if (err == 2)
    {
      print_msg("... ignoring broken NAL unit\n");
      continue;
    }
    else if (err)
      break;

    ref_idcs[nal->nal_ref_idc] ++;

    if (nal->nal_unit_type < 13)
      unit_types[nal->nal_unit_type] ++;
    else if (nal->nal_unit_type < 24)
      unit_types[13] ++;
    else
      unit_types[14] ++;

    if (nal_is_slice(nal))
      slice_types[nal->u.slice.slice_type] ++;
    
    free_nal_unit(&nal);    
  }
  if (err == EOF && !quiet)
    print_msg("EOF\n");
  if (err == 0 || err == EOF)
  {
    int ii;
    fprint_msg("Found %d NAL unit%s\n",context->count,(context->count==1?"":"s"));
    print_msg("nal_ref_idc:\n");
    for (ii=0; ii<4; ii++)
      if (ref_idcs[ii] > 0)
        fprint_msg("  %8d of %2d%s\n",ref_idcs[ii],ii,ii?"":" (non-reference)");

    print_msg("nal_unit_type:\n");
    for (ii=0; ii<13; ii++)
      if (unit_types[ii] > 0)
        fprint_msg("  %8d of type %2d (%s)\n",unit_types[ii],ii,NAL_UNIT_TYPE_STR(ii));
    if (unit_types[13] > 0)
      fprint_msg("  %8d of type 13..23 (Reserved)\n",unit_types[13]);
    if (unit_types[14] > 0)
      fprint_msg("  %8d of typ 24..31 (Unspecified)\n",unit_types[14]);

    print_msg("slice_type:\n");
    for (ii=0; ii<10; ii++)
      if (slice_types[ii] > 0)
        fprint_msg("  %8d of type %2d (%s)\n",slice_types[ii],ii,
               NAL_SLICE_TYPE_STR(ii));
  }
  else
    print_err("### Abandoning reporting due to error\n");
  free_nal_unit_context(&context);
}

/*
 * Report on the content of an MPEG2 file
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `quiet` is true, then only errors will be reported
 */
static void find_h264_fields(ES_p    es,
                             int     max,
                             int     quiet,
                             int     verbose,
                             int     show_nal_details)
{
  int  err;
  int  count = 0;
  int  num_fields = 0;
  int  num_frames = 0;
  access_unit_context_p  context;
  uint32_t num_with_PTS = 0;

  err = build_access_unit_context(es,&context);
  if (err) return;

  if (show_nal_details)
    set_show_nal_reading_details(context->nac,TRUE);

  for (;;)
  {
    access_unit_p  access_unit;

    // NB: remember *not* to call get_next_h264_frame!
    err = get_next_access_unit(context,quiet,verbose,&access_unit);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error getting next access unit\n");
      break;
    }
    count++;

    if (access_unit->field_pic_flag == 1)
    {
      report_access_unit(access_unit);
      num_fields ++;
    }
    else
      num_frames ++;

    if (access_unit_has_PTS(access_unit))
      num_with_PTS ++;

    free_access_unit(&access_unit);
    
    if (max > 0 && count >= max)
      break;
  }
  fprint_msg("Found %d MPEG-4 picture%s, %d field%s, %d frame%s\n",
             count,(count==1?"":"s"),
             num_fields,(num_fields==1?"":"s"),
             num_frames,(num_frames==1?"":"s"));

  fprint_msg("Fields with PTS associated: %u\n",num_with_PTS);
  free_access_unit_context(&context);
}

/*
 * Report on data by access unit.
 */
static void report_h264_frames(ES_p  es,
                               int   max,
                               int   quiet,
                               int   verbose,
                               int   show_nal_details,
                               int   count_sizes,
                               int   count_types)
{
  int err = 0;
  int access_unit_count = 0;
  access_unit_context_p  context;

  uint32_t min_frame_size = 1000000;
  uint32_t max_frame_size = 0;
  uint32_t sum_frame_size = 0;

  uint32_t num_with_PTS = 0;

#define I_NON_REF     0
#define I_REF_IDR     1
#define I_REF_NON_IDR 2
#define I_OTHER       3
#define I_SLICE_I     0
#define I_SLICE_P     1
#define I_SLICE_B     2
#define I_SLICE_MIX   3
  uint32_t slice_types[3][4] = {{0},{0}};
  uint32_t slice_categories[4] = {0};

  ES_offset  start;
  uint32_t   length;

  err = build_access_unit_context(es,&context);
  if (err) return;

  if (show_nal_details)
    set_show_nal_reading_details(context->nac,TRUE);
    
  for (;;)
  {
    access_unit_p      access_unit;

    access_unit_count ++;

    err = get_next_h264_frame(context,quiet,verbose,&access_unit);
    if (err)
      break;

    if (!quiet)
      report_access_unit(access_unit);

    if (count_sizes)
    {
      err = get_access_unit_bounds(access_unit,&start,&length);
      if (err) break;
      if (min_frame_size > length) min_frame_size = length;
      if (max_frame_size < length) max_frame_size = length;
      sum_frame_size += length;
    }

    if (count_types && access_unit->primary_start != NULL)
    {
      if (access_unit->primary_start->nal_ref_idc == 0)
      {
        slice_categories[I_NON_REF] ++;
        if (all_slices_I(access_unit))
          slice_types[I_NON_REF][I_SLICE_I] ++;
        else if (all_slices_P(access_unit))
          slice_types[I_NON_REF][I_SLICE_P] ++;
        else if (all_slices_B(access_unit))
          slice_types[I_NON_REF][I_SLICE_B] ++;
        else
          slice_types[I_NON_REF][I_SLICE_MIX] ++;
      }
      else if (access_unit->primary_start->nal_unit_type == NAL_IDR)
      {
        // Yes, I know that only I and SI frames should be allowed for IDR
        slice_categories[I_REF_IDR] ++;
        if (all_slices_I(access_unit))
          slice_types[I_REF_IDR][I_SLICE_I] ++;
        else if (all_slices_P(access_unit))
          slice_types[I_REF_IDR][I_SLICE_P] ++;
        else if (all_slices_B(access_unit))
          slice_types[I_REF_IDR][I_SLICE_B] ++;
        else
          slice_types[I_REF_IDR][I_SLICE_MIX] ++;
      }
      else if (access_unit->primary_start->nal_unit_type == NAL_NON_IDR)
      {
        slice_categories[I_REF_NON_IDR] ++;
        if (all_slices_I(access_unit))
          slice_types[I_REF_NON_IDR][I_SLICE_I] ++;
        else if (all_slices_P(access_unit))
          slice_types[I_REF_NON_IDR][I_SLICE_P] ++;
        else if (all_slices_B(access_unit))
          slice_types[I_REF_NON_IDR][I_SLICE_B] ++;
        else
          slice_types[I_REF_NON_IDR][I_SLICE_MIX] ++;
      }
      else
        slice_categories[I_OTHER] ++;
    }

    if (access_unit_has_PTS(access_unit))
      num_with_PTS ++;
    
    free_access_unit(&access_unit);

    // Did the logical stream end after the last access unit?
    if (context->end_of_stream)
    {
      if (!quiet) print_msg("Found End-of-stream NAL unit\n");
      break;
    }

    if (max > 0 && access_unit_count >= max)
    {
      fprint_msg("\nStopping because (at least) %d frames have been read\n",
                 access_unit_count);
      break;
    }
  }

  fprint_msg("Found %d frame%s (%d NAL unit%s)\n",
             access_unit_count,(access_unit_count==1?"":"s"),
             context->nac->count,(context->nac->count==1?"":"s"));

  if (count_types)
  {
    if (slice_categories[I_NON_REF] > 0)
    {
      print_msg("Non-reference frames:\n");
      if (slice_types[I_NON_REF][I_SLICE_I] != 0)
        fprint_msg("   I frames    %7d\n",slice_types[I_NON_REF][I_SLICE_I]);
      if (slice_types[I_NON_REF][I_SLICE_P] != 0)
        fprint_msg("   P frames    %7d\n",slice_types[I_NON_REF][I_SLICE_P]);
      if (slice_types[I_NON_REF][I_SLICE_B] != 0)
        fprint_msg("   B frames    %7d\n",slice_types[I_NON_REF][I_SLICE_B]);
      if (slice_types[I_NON_REF][I_SLICE_MIX] != 0)
        fprint_msg("   Mixed/other %7d\n",slice_types[I_NON_REF][I_SLICE_MIX]);
    }

    if (slice_categories[I_REF_IDR] > 0)
    {
      print_msg("IDR frames\n");
      if (slice_types[I_REF_IDR][I_SLICE_I] != 0)
        fprint_msg("   I frames    %7d\n",slice_types[I_REF_IDR][I_SLICE_I]);
      if (slice_types[I_REF_IDR][I_SLICE_P] != 0)
        fprint_msg("   P frames    %7d\n",slice_types[I_REF_IDR][I_SLICE_P]);
      if (slice_types[I_REF_IDR][I_SLICE_B] != 0)
        fprint_msg("   B frames    %7d\n",slice_types[I_REF_IDR][I_SLICE_B]);
      if (slice_types[I_REF_IDR][I_SLICE_MIX] != 0)
        fprint_msg("   Mixed/other %7d\n",slice_types[I_REF_IDR][I_SLICE_MIX]);
    }

    if (slice_categories[I_REF_NON_IDR] > 0)
    {
      print_msg("Non-IDR reference frames:\n");
      if (slice_types[I_REF_NON_IDR][I_SLICE_I] != 0)
        fprint_msg("   I frames    %7d\n",slice_types[I_REF_NON_IDR][I_SLICE_I]);
      if (slice_types[I_REF_NON_IDR][I_SLICE_P] != 0)
        fprint_msg("   P frames    %7d\n",slice_types[I_REF_NON_IDR][I_SLICE_P]);
      if (slice_types[I_REF_NON_IDR][I_SLICE_B] != 0)
        fprint_msg("   B frames    %7d\n",slice_types[I_REF_NON_IDR][I_SLICE_B]);
      if (slice_types[I_REF_NON_IDR][I_SLICE_MIX] != 0)
        fprint_msg("   Mixed/other %7d\n",slice_types[I_REF_NON_IDR][I_SLICE_MIX]);
    }

    if (slice_categories[I_OTHER] > 0)
      fprint_msg("Other frame types: %d\n",slice_categories[I_OTHER]);
  }
  
  {
    double total_seconds = access_unit_count / (double)FRAMES_PER_SECOND;
    int    minutes = (int)(total_seconds / 60);
    double seconds = total_seconds - 60*minutes;
    fprint_msg("At 25 frames/second, that is %dm %.1fs (%.2fs)\n",minutes,seconds,
               total_seconds);
  }

  if (count_sizes && access_unit_count > 0)
    fprint_msg("Frame sizes ranged from %u to %u bytes, mean %.2f\n",
               min_frame_size,max_frame_size,
               sum_frame_size/(double)access_unit_count);

  fprint_msg("Frames with PTS associated: %u\n",num_with_PTS);

  free_access_unit_context(&context);
}

static void print_usage()
{
  print_msg(
    "Usage: esreport [switches] [<infile>]\n"
    "\n"
    );
  REPORT_VERSION("esreport");
  print_msg(
    "\n"
    "  Report on the content of an elementary stream containing H.264\n"
    "  (MPEG-4/AVC), H.262 (MPEG-2) or AVS video data.\n"
    "\n"
    "Files:\n"
    "  <infile>  is the Elementary Stream file (but see -stdin below)\n"
    "\n"
    "What to report:\n"
    "  The default is to report on H.262 items, AVS frames or H.264 NAL units.\n"
    "  Other choices are:\n"
    "\n"
    "  -frames           Report by frames. The default for AVS.\n"
    "  -findfields       Report on any fields in the data. Ignored for AVS.\n"
    "  -afd              Report (just) on AFD changes in H.262. Ignored for the\n"
    "                    other types of file.\n"
    "  -es               Report on ES units.\n"
    "\n"
    "  Reporting on frames may be modified by:\n"
    "\n"
    "  -framesize        Report on the sizes of frames (mean, etc.).\n"
    "  -frametype        Report on the numbers of different type of frame.\n"
    "\n"
    "  (in fact, both of these imply -frame).\n"
    "\n"
    "Other switches:\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -verbose, -v      For H.262 data, output information about the data\n"
    "                    in each MPEG-2 item. For ES units, output information\n"
    "                    about the data in each ES unit. Ignored for H.264 data.\n"
    "  -quiet, -q        Only output summary information (i.e., the number\n"
    "                    of entities in the file, statistics, etc.)\n"
    "  -x                Show details of each NAL unit as it is read.\n"
    "  -stdin            Take input from <stdin>, instead of a named file\n"
    "  -max <n>, -m <n>  Maximum number of NAL units/MPEG-2 items/AVS frames/ES units\n"
    "                    to read. If -frames, then the program will stop after\n"
    "                    that many frames. If reading 'frames', MPEG-2 and AVS will\n"
    "                    also count sequence headers and sequence end.\n"
    "  -pes, -ts         The input file is TS or PS, to be read via the\n"
    "                    PES->ES reading mechanisms\n"
    "  -pesreport        Report on PES headers. Implies -pes and -q.\n"
    "\n"
    "Stream type:\n"
    "  If input is from a file, then the program will look at the start of\n"
    "  the file to determine if the stream is H.264, H.262 or AVS data. This\n"
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
    "  -avs              Force the program to treat the input as AVS.\n"
    );
}

int main(int argc, char **argv)
{
  char  *input_name = NULL;
  int    had_input_name = FALSE;
  int    use_stdin = FALSE;
  int    err = 0;
  ES_p   es = NULL;
  int    max = 0;
  int    by_frame = FALSE;
  int    find_fields = FALSE;
  int    quiet = FALSE;
  int    verbose = FALSE;
  int    show_nal_details = FALSE;
  int    give_pes_info = FALSE;
  int    report_afds = FALSE;
  int    report_framesize = FALSE;
  int    report_frametype = FALSE;
  int    report_pes_headers = FALSE;
  int    report_ES = FALSE;
  int    ii = 1;

  int    use_pes = FALSE;

  int    want_data = VIDEO_H262;
  int    is_data;
  int    force_stream_type = FALSE;
  
  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-help",argv[ii]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("esreport",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### esreport: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
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
      else if (!strcmp("-avs",argv[ii]))
      {
        force_stream_type = TRUE;
        want_data = VIDEO_AVS;
      }
      else if (!strcmp("-es",argv[ii]))
      {
        report_ES = TRUE;
      }
      else if (!strcmp("-frames",argv[ii]))
        by_frame = TRUE;
      else if (!strcmp("-framesize",argv[ii]))
      {
        by_frame = TRUE;
        report_framesize = TRUE;
      }
      else if (!strcmp("-frametype",argv[ii]))
      {
        by_frame = TRUE;
        report_frametype = TRUE;
      }
      else if (!strcmp("-afd",argv[ii]) || !strcmp("-afds",argv[ii]))
        report_afds = TRUE;
      else if (!strcmp("-findfields",argv[ii]))
        find_fields = TRUE;
      else if (!strcmp("-stdin",argv[ii]))
      {
        had_input_name = TRUE; // more or less
        use_stdin = TRUE;
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
        show_nal_details = TRUE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("esreport",ii);
        err = int_value("esreport",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pes",argv[ii]) || !strcmp("-ts",argv[ii]))
        use_pes = TRUE;
      else if (!strcmp("-pesreport",argv[ii]))
      {
        report_pes_headers = TRUE;
        use_pes = TRUE;
        quiet = TRUE;
      }
      else if (!strcmp("-pesinfo",argv[ii]))
      {
        give_pes_info = TRUE;
        use_pes = TRUE;
      }
      else
      {
        fprint_err("### esreport: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### esreport: Unexpected '%s'\n",argv[ii]);
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
    print_err("### esreport: No input file specified\n");
    return 1;
  }

  err = open_input_as_ES((use_stdin?NULL:input_name),use_pes,quiet,
                         force_stream_type,want_data,&is_data,&es);
  if (err)
  {
    print_err("### esreport: Error opening input file\n");
    return 1;
  }

  if (report_pes_headers)
  {
    es->reader->debug_read_packets = TRUE;
  }

  if (give_pes_info)
  {
    es->reader->give_info = TRUE;
  }

  if (report_ES)
  {
    report_ES_units(es,max,verbose,quiet);
  }
  else if (is_data == VIDEO_H262)
  {
    if (find_fields)
      find_h262_fields(es,max,verbose);
    else if (by_frame)
      report_h262_frames(es,max,verbose,quiet,report_framesize);
    else if (report_afds)
      report_h262_afds(es,max,verbose,quiet);
    else
      report_h262_items(es,max,verbose,quiet);
  }
  else if (is_data == VIDEO_AVS)
  {
    report_avs_frames(es,max,verbose,quiet,report_framesize);
  }
  else if (is_data == VIDEO_H264)
  {
    if (find_fields)
      find_h264_fields(es,max,quiet,verbose,show_nal_details);
    else if (by_frame)
      report_h264_frames(es,max,quiet,verbose,show_nal_details,
                         report_framesize,report_frametype);
    else
      report_by_nal_unit(es,max,quiet,show_nal_details);
  }
  else
  {
    print_err("### esreport: Unexpected type of video data\n");
    return 1;
  }

  err = close_input_as_ES(input_name,&es);
  if (err)
  {
    print_err("### esreport: Error closing input file\n");
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

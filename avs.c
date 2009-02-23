/*
 * Datastructures and prototypes for reading AVS elementary streams.
 *
 * XXX Ignores the issue of the equivalent of AFD data. This *will* cause
 * XXX problems if rewinding or filtering is to be done. However, what
 * XXX needs to be done to fix this can probably be based on the code in
 * XXX h262.c.
 * XXX And, also, reversing is not yet supported for AVS, anyway.
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
#include "printing_fns.h"
#include "avs_fns.h"
#include "es_fns.h"
#include "ts_fns.h"
#include "reverse_fns.h"
#include "misc_fns.h"

#define DEBUG                  0
#define DEBUG_GET_NEXT_PICTURE 0


/*
 * Return a string representing the start code
 */
extern const char *avs_start_code_str(byte   start_code)
{
  if (start_code < 0xB0) return "Slice";
  switch (start_code)
  {
    // AVS start codes that we are interested in
  case 0xB0: return "Video sequence start";
  case 0xB1: return "Video sequence end";
  case 0xB2: return "User data";
  case 0xB3: return "I frame";
  case 0xB4: return "Reserved";
  case 0xB5: return "Extension start";
  case 0xB6: return "P/B frame";
  case 0xB7: return "Video edit";
  default:   return "Reserved";
  }
}

// ------------------------------------------------------------
// AVS item *data* stuff
// ------------------------------------------------------------
/*
 * Build a new AVS frame reading context.
 *
 * This acts as a "jacket" around the ES context, and is used when reading
 * AVS frames with get_next_avs_frame(). It "remembers" the last
 * item read, which is the first item that was not part of the frame.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_avs_context(ES_p            es,
                             avs_context_p  *context)
{
  avs_context_p  new = malloc(SIZEOF_AVS_CONTEXT);
  if (new == NULL)
  {
    print_err("### Unable to allocate AVS context datastructure\n");
    return 1;
  }

  new->es = es;
  new->frame_index = 0;
  new->last_item = NULL;
  new->reverse_data = NULL;
  new->count_since_seq_hdr = 0;

  *context = new;
  return 0;
}

/*
 * Free an AVS frame reading context.
 *
 * Clears the datastructure, frees it, and returns `context` as NULL.
 *
 * Does not free any `reverse_data` datastructure.
 *
 * Does nothing if `context` is already NULL.
 */
extern void free_avs_context(avs_context_p *context)
{
  avs_context_p  cc = *context;

  if (cc == NULL)
    return;

  if (cc->last_item != NULL)
    free_ES_unit(&cc->last_item);

  cc->reverse_data = NULL;
  
  free(*context);
  *context = NULL;
  return;
}

/*
 * Rewind a file being read as AVS frames
 *
 * This is a wrapper for `seek_ES` that also knows to unset things appropriate
 * to the AVS frame reading context.
 *
 * If a reverse context is attached to this context, it also will
 * be "rewound" appropriately.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int rewind_avs_context(avs_context_p  context)
{
  ES_offset  start_of_file = {0,0};
  
  // First, forget where we are
  if (context->last_item)
  {
    free_ES_unit(&context->last_item);
    context->last_item = NULL;
  }

  context->frame_index = 0;  // no frames read from this file yet

  // Next, take care of rewinding
  if (context->reverse_data)
  {
    context->reverse_data->last_posn_added = -1; // next entry to be 0
    context->count_since_seq_hdr = 0;            // what else can we do?
  }

  // And then, do the relocation itself
  return seek_ES(context->es,start_of_file);
}

// ------------------------------------------------------------
// AVS "frames"
// ------------------------------------------------------------
/*
 * Add (the information from) an AVS ES unit to the given frame.
 *
 * Note that since this takes a copy of the ES unit data,
 * it is safe to free the original ES unit.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int append_to_avs_frame(avs_frame_p  frame,
                               ES_unit_p    unit)
{
  return append_to_ES_unit_list(frame->list,unit);
}

/*
 * Determine the picture coding type of an AVS ES unit
 *
 * P/B frames are distinguished by their picture coding types. For I frames,
 * we make one up...
 *
 * Returns an appropriate value (0 if none suitable)
 */
extern int avs_picture_coding_type(ES_unit_p  unit)
{
  if (unit->start_code == 0xB3)
    return AVS_I_PICTURE_CODING;                // strictly our invention
  else if (unit->start_code == 0xB6)
  {
    byte picture_coding_type = (unit->data[6] & 0xC0) >> 6;
    if (picture_coding_type == AVS_P_PICTURE_CODING ||
        picture_coding_type == AVS_B_PICTURE_CODING)
      return picture_coding_type;
    else
    {
      fprint_err("AVS Picture coding type %d (in %02x)\n",
                 picture_coding_type,unit->data[3]);
      return 0;
    }
  }
  else
  {
    fprint_err("AVS 'frame' with start code %02x does not have picture coding type\n",
               unit->data[0]);
    return 0;
  }
}

/*
 * Build a new AVS "frame", starting with the given item (which is
 * copied, so may be freed after this call).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int build_avs_frame(avs_context_p   context,
                           avs_frame_p    *frame,
                           ES_unit_p       unit)
{
  int  err;
  byte        *data = unit->data;
  avs_frame_p  new  = malloc(SIZEOF_AVS_FRAME);
  if (new == NULL)
  {
    print_err("### Unable to allocate AVS frame datastructure\n");
    return 1;
  }

  err = build_ES_unit_list(&(new->list));
  if (err)
  {
    print_err("### Unable to allocate internal list for AVS frame\n");
    free(new);
    return 1;
  }

  // Deduce what we can from the first unit of the "frame"
  new->start_code = unit->start_code;
  if (is_avs_frame_item(unit))
  {
    new->picture_coding_type = avs_picture_coding_type(unit);
    new->is_frame = TRUE;
    new->is_sequence_header = FALSE;
    if (new->picture_coding_type != AVS_I_PICTURE_CODING)
      new->picture_distance = (data[6] << 2) | ((data[7] & 0xC0) >> 6);
    else
      // I frames *do* have a picture_distance field, but I'm not interested
      // and it takes more work to find it...
      new->picture_distance = 0;
  }
  else if (is_avs_seq_header_item(unit))
  {
    new->is_frame = FALSE;
    new->is_sequence_header = TRUE;
    new->picture_coding_type = 0xFF;    // Meaningless value, just in case
    new->aspect_ratio = (data[10] & 0x3C) >> 2;
    new->frame_rate_code = ((data[10] & 0x03) << 2) | ((data[11] & 0xC0) >> 4);
#if DEBUG
    fprint_msg("aspect_ratio=%d, frame_rate_code=%d (%.2f)\n",new->aspect_ratio,
               new->frame_rate_code,avs_frame_rate(new->frame_rate_code));
#endif
  }
  else if (is_avs_seq_end_item(unit))
  {
    new->is_frame = FALSE;
    new->is_sequence_header = FALSE;
    new->picture_coding_type = 0xFF;    // Meaningless value, just in case
  }
  else
  {
    fprint_err("!!! Building AVS frame that starts with a %s (%02x)\n",
               avs_start_code_str(unit->start_code),unit->start_code);
    new->is_frame = FALSE;
    new->is_sequence_header = FALSE;
    new->picture_coding_type = 0xFF;    // Meaningless value, just in case
  }

  err = append_to_avs_frame(new,unit);
  if (err)
  {
    fprint_err("### Error appending first ES unit to AVS %s\n",
               avs_start_code_str(unit->start_code));
    free_avs_frame(&new);
    return 1;
  }

  *frame = new;
  return 0;
}

/*
 * Free an AVS "frame".
 *
 * Clears the datastructure, frees it, and returns `frame` as NULL.
 *
 * Does nothing if `frame` is already NULL.
 */
extern void free_avs_frame(avs_frame_p *frame)
{
  avs_frame_p  pic = *frame;

  if (pic == NULL)
    return;

  if (pic->list != NULL)
    free_ES_unit_list(&pic->list);

  free(*frame);
  *frame = NULL;
  return;
}

#if DEBUG_GET_NEXT_PICTURE
/*
 * Print a representation of an item for debugging
 */
static void _show_item(ES_unit_p        unit)
{
  print_msg("__ ");
  if (unit == NULL)
  {
    print_msg("<no ES unit>\n");
    return;
  }
  if (is_avs_frame_item(unit))
    fprint_msg("%s frame",AVS_PICTURE_CODING_STR(avs_picture_coding_type(unit)));
  else
    fprint_msg("%s",avs_start_code_str(unit->start_code));
  fprint_msg(" at " OFFSET_T_FORMAT "/%d for %d\n",
         unit->start_posn.infile,unit->start_posn.inpacket,unit->data_len);
}
#endif

/*
 * Retrieve the the next AVS "frame".
 *
 * The AVS "frame" returned can be one of:
 *
 * 1. A frame, including its data.
 * 2. A sequence header, including its sequence extension, if any.
 * 3. A sequence end.
 *
 * - `context` is the AVS frame reading context.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `frame` is the AVS "frame", containing a frame, a sequence header or a
 *   sequence end
 *
 * Returns 0 if it succeeds, EOF if we reach the end of file, or 1 if some
 * error occurs.
 */
static int get_next_avs_single_frame(avs_context_p  context,
                                     int            verbose,
                                     avs_frame_p   *frame)
{
  int  err = 0;

  int  in_sequence_header = FALSE;
  int  in_sequence_end    = FALSE;
  int  in_frame         = FALSE;
  int  last_was_slice     = FALSE;

  ES_unit_p  item = context->last_item;

#if DEBUG_GET_NEXT_PICTURE
  int  num_slices = 0;
  int  had_slice = FALSE;
  int  last_slice_start_code = 0;
  if (verbose && context->last_item) print_msg("__ reuse last item\n");
#endif

  context->last_item = NULL;

  // Find the first item of our next "frame"
  for (;;)
  {
    if (item == NULL)
    {
      err = find_and_build_next_ES_unit(context->es,&item);
      if (err) return err;
    }
    if (is_avs_frame_item(item))
    {
      in_frame = TRUE;
      break;
    }
    else if (is_avs_seq_header_item(item))
    {
      in_sequence_header = TRUE;
      break;
    }
    else if (is_avs_seq_end_item(item))
    {
      in_sequence_end = TRUE;
      break;
    }
#if DEBUG_GET_NEXT_PICTURE
    else if (verbose)
      _show_item(item);
#endif
    free_ES_unit(&item);
  }

#if DEBUG_GET_NEXT_PICTURE
  if (verbose)
  {
    print_msg("__ --------------------------------- <start frame>\n");
    _show_item(item);
  }
#endif
  
  err = build_avs_frame(context,frame,item);
  if (err) return 1;

  free_ES_unit(&item);

  if (in_sequence_end)
  {
    // A sequence end is a single item, so we're done
#if DEBUG_GET_NEXT_PICTURE
    if (verbose)
      print_msg("__ --------------------------------- <end frame>\n");
#endif
    return 0;
  }
  
  // Now find all the rest of the frame/sequence header
  for (;;)
  {
    err = find_and_build_next_ES_unit(context->es,&item);
    if (err)
    {
      if (err != EOF)
        free_avs_frame(frame);
      return err;
    }

    if (in_frame)
    {
      // Have we just finished a frame?
      // We know we have if the last item was a slice, but this one isn't
      if (last_was_slice && !is_avs_slice_item(item))
        break;
      last_was_slice = is_avs_slice_item(item);
    }
    else if (in_sequence_header)
    {
      // Have we just finished a sequence header and its friends?
      // We know we have if we've hit something that isn't an
      // extension start or user data start code (perhaps we could
      // get away with just keeping the (in MPEG-2) sequence_extension,
      // but it's safer (and simpler) to keep the lot
      if (!is_avs_extension_start_item(item) &&
          !is_avs_user_data_item(item))
        break;
    }

#if DEBUG_GET_NEXT_PICTURE
    if (verbose)
    {
      if (!had_slice)
        _show_item(item);
      if (is_avs_slice_item(item))
      {
        num_slices ++;
        last_slice_start_code = item->start_code;
        if (!had_slice)
          had_slice = TRUE;
      }
    }
#endif

    // Don't forget to remember the actual item
    err = append_to_avs_frame(*frame,item);
    if (err)
    {
      print_err("### Error adding item to AVS sequence header\n");
      free_avs_frame(frame);
      return 1;
    }
    free_ES_unit(&item);
  }

  if (in_frame)
    context->frame_index ++;

  context->last_item = item;
#if DEBUG_GET_NEXT_PICTURE
  if (verbose)
  {
    if (in_frame)
    {
      if (num_slices > 1)
      {
        ES_unit_p  unit = &(*frame)->list->array[(*frame)->list->length-1];
        print_msg("__ ...\n");
        fprint_msg("__ slice %2x",last_slice_start_code);
        fprint_msg(" at " OFFSET_T_FORMAT "/%d for %d\n",
                   unit->start_posn.infile,unit->start_posn.inpacket,
                   unit->data_len);
      }
      fprint_msg("__ (%2d slices)\n",num_slices);
    }
    print_msg("__ --------------------------------- <end frame>\n");
    if (in_frame || in_sequence_header)
      _show_item(item);
  }
#endif
  return 0;
}

/*
 * Retrieve the the next AVS "frame".
 *
 * The AVS "frame" returned can be one of:
 *
 * 1. A frame, including its slices.
 * 2. A sequence header, including its sequence extension, if any.
 * 3. A sequence end.
 *
 * Specifically, the next AVS "frame" is retrieved from the input stream.
 *
 * If that "frame" represents a sequence header or a frame, it is returned.
 *
 *   Note that if the context is associated with a reverse context,
 *   then appropriate frames/sequence headers will automatically be
 *   remembered therein.
 *
 * - `context` is the AVS frame reading context.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `frame` is the AVS "frame", containing a frame, a sequence header or a
 *   sequence end
 *
 * Returns 0 if it succeeds, EOF if we reach the end of file, or 1 if some
 * error occurs.
 */
extern int get_next_avs_frame(avs_context_p  context,
                              int            verbose,
                              int            quiet,
                              avs_frame_p   *frame)
{
  int  err;

  err = get_next_avs_single_frame(context,verbose,frame);
  if (err) return err;

#if 0
  if (context->reverse_data)
  {
    err = maybe_remember_this_frame(context,verbose,*frame);
    if (err)
    {
      free_avs_frame(frame);
      return 1;
    }
  }
#endif

  return 0;
}

/*
 * Write out an AVS frame as TS
 *
 * - `tswriter` is TS the output stream
 * - `frame` is the frame to write out
 * - `pid` is the PID to use for the TS packets
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_avs_frame_as_TS(TS_writer_p     tswriter,
                                 avs_frame_p     frame,
                                 uint32_t        pid)
{
  int ii;
  ES_unit_list_p  list;

  if (frame == NULL || frame->list == NULL)
    return 0;
  
  list = frame->list;
  
  for (ii = 0; ii < list->length; ii++)
  {
    int  err;
    ES_unit_p  unit = &(list->array[ii]);

    err = write_ES_as_TS_PES_packet(tswriter,unit->data,unit->data_len,pid,
                                    DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing out frame list to TS\n");
      return err;
    }
  }
  return 0;
}

/*
 * Write out an AVS frame as TS, with PTS timing in the first PES packet
 * (and PCR timing in the first TS of the frame).
 *
 * - `frame` is the frame to write out
 * - `tswriter` is the TS context to write with
 * - `video_pid` is the PID to use to write the data
 * - `got_pts` is TRUE if we have a PTS value, in which case
 * - `pts` is said PTS value
 * - `got_dts` is TRUE if we also have DTS, in which case
 * - `dts` is said DTS value.
 *
 * If we are given a DTS (which must, by definition, always go up) we will also
 * use it as the value for PCR.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_avs_frame_as_TS_with_pts_dts(avs_frame_p          frame,
                                              TS_writer_p          tswriter,
                                              uint32_t             video_pid,
                                              int                  got_pts,
                                              uint64_t             pts,
                                              int                  got_dts,
                                              uint64_t             dts)
{
  int ii;
  ES_unit_list_p  list;

  if (frame == NULL || frame->list == NULL)
    return 0;
  
  list = frame->list;
  
  for (ii = 0; ii < list->length; ii++)
  {
    int  err;
    ES_unit_p  unit = &(list->array[ii]);

    // Only write the first PES packet out with PTS
    if (ii == 0)
      err = write_ES_as_TS_PES_packet_with_pts_dts(tswriter,
                                                   unit->data,
                                                   unit->data_len,
                                                   video_pid,
                                                   DEFAULT_VIDEO_STREAM_ID,
                                                   got_pts,pts,
                                                   got_dts,dts);
    else
      err = write_ES_as_TS_PES_packet(tswriter,
                                      unit->data,unit->data_len,
                                      video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing out frame list to TS\n");
      return err;
    }
  }
  return 0;
}

/*
 * Write out an AVS frame as TS, with PCR timing in the first TS of the
 * frame.
 *
 * - `frame` is the frame to write out
 * - `tswriter` is the TS context to write with
 * - `video_pid` is the PID to use to write the data
 * - `pcr_base` and `pcr_extn` encode the PCR value.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_avs_frame_as_TS_with_PCR(avs_frame_p   frame,
                                          TS_writer_p   tswriter,
                                          uint32_t      video_pid,
                                          uint64_t      pcr_base,
                                          uint32_t      pcr_extn)
{
  int ii;
  ES_unit_list_p  list;

  if (frame == NULL || frame->list == NULL)
    return 0;
  
  list = frame->list;
  
  for (ii = 0; ii < list->length; ii++)
  {
    int  err;
    ES_unit_p  unit = &(list->array[ii]);
  
    // Only write the first PES packet out with PCR
    if (ii == 0)
      err = write_ES_as_TS_PES_packet_with_pcr(tswriter,
                                               unit->data,
                                               unit->data_len,
                                               video_pid,
                                               DEFAULT_VIDEO_STREAM_ID,
                                               pcr_base,pcr_extn);
    else
      err = write_ES_as_TS_PES_packet(tswriter,
                                      unit->data,unit->data_len,
                                      video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing out frame list to TS\n");
      return err;
    }
  }
  return 0;
}

/*
 * Write out a frame (as stored in an ES unit list) as ES
 *
 * - `output` is the ES output file
 * - `frame` is the frame to write out
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_avs_frame_as_ES(FILE        *output,
                                 avs_frame_p  frame)
{
  int ii;
  ES_unit_list_p  list;

  if (frame == NULL || frame->list == NULL)
    return 0;
  
  list = frame->list;
  
  for (ii = 0; ii < list->length; ii++)
  {
    int  err;
    ES_unit_p  unit = &(list->array[ii]);
    err = write_ES_unit(output,unit);
    if (err)
    {
      print_err("### Error writing out frame list to ES\n");
      return err;
    }
  }
  return 0;
}

/*
 * Report on an AVS frame's contents.
 *
 * - `stream` is where to write the information
 * - `frame` is the frame to report on
 * - if `report_data`, then the component ES units will be printed out as well
 */
extern void report_avs_frame(avs_frame_p  frame,
                             int          report_data)
{
  if (frame->is_frame)
  {
    fprint_msg("%s #%02d",
               AVS_PICTURE_CODING_STR(frame->picture_coding_type),
               frame->picture_distance);
    print_msg("\n");
  }
  else if (frame->is_sequence_header)
  {
    print_msg("Sequence header: ");
    fprint_msg(" frame rate %d (%.2f), aspect ratio %d (%s)",
               frame->frame_rate_code,
               avs_frame_rate(frame->frame_rate_code),
               frame->aspect_ratio,
               (frame->aspect_ratio==1?"SAR: 1.0":
                frame->aspect_ratio==2?"4/3":
                frame->aspect_ratio==3?"16/9":
                frame->aspect_ratio==4?"2.21/1":"???"));
    print_msg("\n");
  }
  else
  {
    print_msg("Sequence end\n");
  }
  if (report_data)
    report_ES_unit_list("ES units",frame->list);
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

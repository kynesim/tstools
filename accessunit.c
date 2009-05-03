/*
 * Utilities for working with access units in H.264 elementary streams.
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

#include "compat.h"
#include "printing_fns.h"
#include "es_fns.h"
#include "ts_fns.h"
#include "nalunit_fns.h"
#include "accessunit_fns.h"
#include "reverse_fns.h"

#define DEBUG 0


/*
 * Build a new access unit datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static inline int build_access_unit(access_unit_p  *acc_unit,
                                    uint32_t        index)
{
  int err;

  access_unit_p  new = malloc(SIZEOF_ACCESS_UNIT);
  if (new == NULL)
  {
    print_err("### Unable to allocate access unit datastructure\n");
    return 1;
  }

  err = build_nal_unit_list(&(new->nal_units));
  if (err)
  {
    free(new);
    *acc_unit = NULL;
    return err;
  }
  new->index = index;
  new->started_primary_picture = FALSE;
  new->primary_start = NULL;
  new->ignored_broken_NAL_units = 0;

  new->frame_num = new->field_pic_flag = new->bottom_field_flag = 0;

  *acc_unit = new;
  return 0;
}

/*
 * Tidy up an access unit datastructure after we've finished with it.
 *
 * If `deep` is TRUE, also frees all of the NAL units in the NAL unit
 * list (which is normally what we want to do).
 */
static inline void clear_access_unit(access_unit_p  acc_unit,
                                     int            deep)
{
  free_nal_unit_list(&(acc_unit->nal_units),deep);
  acc_unit->primary_start = NULL;
}

/*
 * Tidy up and free an access unit datastructure after we've finished with it.
 *
 * Clears the datastructure, frees it, and returns `acc_unit` as NULL.
 *
 * Does nothing if `acc_unit` is already NULL.
 */
extern void free_access_unit(access_unit_p  *acc_unit)
{
  if (*acc_unit == NULL)
    return;
  clear_access_unit(*acc_unit,TRUE);
  free(*acc_unit);
  *acc_unit = NULL;
}

/*
 * Report on this access unit
 */
extern void report_access_unit(access_unit_p  access_unit)
{
  int ii;
  fprint_msg("Access unit %u",access_unit->index);
  if (access_unit->started_primary_picture)
    fprint_msg(" (%s)",access_unit->primary_start->start_reason);
  print_msg(":\n");
  if (access_unit->field_pic_flag)
    fprint_msg("  %s field of frame %u\n",
               (access_unit->bottom_field_flag==1?"Bottom":"Top"),
               access_unit->frame_num);
  else
    fprint_msg("  Frame %u\n",access_unit->frame_num);

  if (access_unit->ignored_broken_NAL_units)
    fprint_msg("  Ignored %d broken NAL unit%s\n",
               access_unit->ignored_broken_NAL_units,
               (access_unit->ignored_broken_NAL_units==1?"":"s"));
  
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p nal = access_unit->nal_units->array[ii];
    if (nal == NULL)
      print_msg("     <null>\n");
    else
    {
      fprint_msg("    %c",((access_unit->primary_start == nal)?'*':' '));
      report_nal(TRUE,nal);
    }
  }
}

/*
 * How many slices (VCL NAL units) are there in this access unit?
 */
static inline int num_slices(access_unit_p  access_unit)
{
  int count = 0;
  int ii;
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    if (nal_is_slice(access_unit->nal_units->array[ii]))
      count ++;
  }
  return count;
}

/*
 * Retrieve the bounds of this access unit in the file it was read from.
 *
 * - `access_unit` is the access unit we're interested in
 * - `start` is its start position (i.e., the location at which to start
 *   reading to retrieve all of the data for the access unit, including
 *   the 00 00 01 prefix at the start of the first NAL unit therein)
 * - `length` is the total length of the NAL units within this access unit
 *
 * Returns 0 if all goes well, 1 if the access unit has no content.
 */
extern int get_access_unit_bounds(access_unit_p     access_unit,
                                  ES_offset        *start,
                                  uint32_t         *length)
{
  int ii;
  if (access_unit->primary_start == NULL)
  {
    print_err("### Cannot determine bounds of an access unit with no content\n");
    return 1;
  }

  *start = access_unit->nal_units->array[0]->unit.start_posn;
  *length = 0;

  // Maybe we should precalculate, or even cache, the total length...
  for (ii=0; ii<access_unit->nal_units->length; ii++)
    (*length) += access_unit->nal_units->array[ii]->unit.data_len;

  return 0;
}

/*
 * Are all slices in this access unit I slices?
 */
extern int all_slices_I(access_unit_p  access_unit)
{
  int ii;
  if (access_unit->primary_start == NULL)
    return FALSE;
  if (!nal_is_slice(access_unit->primary_start))
    return FALSE;
  // All I
  if (access_unit->primary_start->u.slice.slice_type == ALL_SLICES_I)
    return TRUE;
  // Only one slice, and it's I
  if (num_slices(access_unit) == 1 &&
      access_unit->primary_start->u.slice.slice_type == SLICE_I)
    return TRUE;
  // Are any of the slices not I?
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal_unit = access_unit->nal_units->array[ii];
    if (nal_is_slice(nal_unit) && nal_unit->u.slice.slice_type != SLICE_I)
      return FALSE;
  }
  return TRUE;
}

/*
 * Are all slices in this access unit P slices?
 */
extern int all_slices_P(access_unit_p  access_unit)
{
  int ii;
  if (access_unit->primary_start == NULL)
    return FALSE;
  if (!nal_is_slice(access_unit->primary_start))
    return FALSE;
  // All P
  if (access_unit->primary_start->u.slice.slice_type == ALL_SLICES_P)
    return TRUE;
  // Only one slice, and it's P
  if (num_slices(access_unit) == 1 &&
      access_unit->primary_start->u.slice.slice_type == SLICE_P)
    return TRUE;
  // Are any of the slices not P?
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal_unit = access_unit->nal_units->array[ii];
    if (nal_is_slice(nal_unit) && nal_unit->u.slice.slice_type != SLICE_P)
      return FALSE;
  }
  return TRUE;
}

/*
 * Are all slices in this access unit I or P slices?
 */
extern int all_slices_I_or_P(access_unit_p  access_unit)
{
  int ii;
  if (access_unit->primary_start == NULL)
    return FALSE;
  if (!nal_is_slice(access_unit->primary_start))
    return FALSE;
  // All P or all I
  if (access_unit->primary_start->u.slice.slice_type == SLICE_I ||
      access_unit->primary_start->u.slice.slice_type == SLICE_P)
    return TRUE;
  // Only one slice, and it's P or I
  if (num_slices(access_unit) == 1 &&
      (access_unit->primary_start->u.slice.slice_type == ALL_SLICES_I ||
       access_unit->primary_start->u.slice.slice_type == ALL_SLICES_P))
    return TRUE;
  // Are any of the slices not either P or I?
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal_unit = access_unit->nal_units->array[ii];
    if (nal_is_slice(nal_unit) &&
        (nal_unit->u.slice.slice_type != SLICE_I &&
         nal_unit->u.slice.slice_type != SLICE_P))
      return FALSE;
  }
  return TRUE;
}

/*
 * Are all slices in this access unit B slices?
 */
extern int all_slices_B(access_unit_p  access_unit)
{
  int ii;
  if (access_unit->primary_start == NULL)
    return FALSE;
  if (!nal_is_slice(access_unit->primary_start))
    return FALSE;
  // All B
  if (access_unit->primary_start->u.slice.slice_type == ALL_SLICES_B)
    return TRUE;
  // Only one slice, and it's B
  if (num_slices(access_unit) == 1 &&
      access_unit->primary_start->u.slice.slice_type == SLICE_B)
    return TRUE;
  // Are any of the slices not B?
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal_unit = access_unit->nal_units->array[ii];
    if (nal_is_slice(nal_unit) && nal_unit->u.slice.slice_type != SLICE_B)
      return FALSE;
  }
  return TRUE;
}

/*
 * Append a NAL unit to the list of NAL units for this access unit
 *
 * NB: `pending` may be NULL
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int access_unit_append(access_unit_p    access_unit,
                              nal_unit_p       nal,
                              int              starts_primary,
                              nal_unit_list_p  pending)
{
  int err;
  if (starts_primary && access_unit->started_primary_picture)
  {
    // Our caller should have started a new access unit instead
    fprint_err("### Already had a start of primary picture in access"
               " unit %d\n",access_unit->index);
    return 1;
  }

  if (starts_primary)
  {
    access_unit->primary_start = nal;
    access_unit->started_primary_picture = TRUE;
    access_unit->frame_num = nal->u.slice.frame_num;
    access_unit->field_pic_flag = nal->u.slice.field_pic_flag;
    access_unit->bottom_field_flag = nal->u.slice.bottom_field_flag;
  }
  if (pending != NULL && pending->length > 0)
  {
    int ii;
    for (ii=0; ii<pending->length; ii++)
    {
      err = append_to_nal_unit_list(access_unit->nal_units,
                                    pending->array[ii]);
      if (err)
      {
        fprint_err("### Error extending access unit %d\n",
                   access_unit->index);
        return err;
      }
    }
  }

  if (nal != NULL)
  {
    err = append_to_nal_unit_list(access_unit->nal_units,nal);
    if (err)
    {
      fprint_err("### Error extending access unit %d\n",
                 access_unit->index);
      return err;
    }
  }
  return 0;
}

/*
 * Merge the NAL units of the second access unit into the first, and then
 * free the second access unit.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int merge_access_unit_nals(access_unit_p   access_unit1,
                                  access_unit_p  *access_unit2)
{
  int  err, ii;

  for (ii = 0; ii < (*access_unit2)->nal_units->length; ii++)
  {
    err = append_to_nal_unit_list(access_unit1->nal_units,
                                  (*access_unit2)->nal_units->array[ii]);
    if (err)
    {
      print_err("### Error merging two access units\n");
      return err;
    }
  }

  // Don't forget that we're now "sharing" any ignored NAL units
  access_unit1->ignored_broken_NAL_units +=
    (*access_unit2)->ignored_broken_NAL_units;

  // Take care not to free the individual NAL units in our second access
  // unit, as they are still being used by the first
  clear_access_unit(*access_unit2,FALSE);
  free(*access_unit2);
  *access_unit2 = NULL;

  // Fake the flags in our remaining access unit to make us "look" like
  // a frame
  access_unit1->field_pic_flag = 0;
  return 0;
}

/*
 * Write out an access unit as ES.
 *
 * Also writes out any end of sequence or end of stream NAL unit found in the
 * `context` (since they are assumed to have immediately followed this access
 * unit).
 *
 * - `access_unit` is the access unit to write out
 * - `context` may contain additional things to write (see above), but may
 *   legitimately be NULL if there is no context.
 * - `output` is the ES file to write to
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_access_unit_as_ES(access_unit_p           access_unit,
                                   access_unit_context_p   context,
                                   FILE                   *output)
{
  int ii, err;
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    err = write_ES_unit(output,&(access_unit->nal_units->array[ii]->unit));
    if (err)
    {
      print_err("### Error writing NAL unit ");
      report_nal(FALSE,access_unit->nal_units->array[ii]);
      return err;
    }
  }

  if (context != NULL && context->end_of_sequence)
  {
    err = write_ES_unit(output,&(context->end_of_sequence->unit));
    if (err)
    {
      print_err("### Error writing end of sequence NAL unit ");
      report_nal(FALSE,context->end_of_sequence);
      return err;
    }
    free_nal_unit(&context->end_of_sequence);
  }

  if (context != NULL && context->end_of_stream)
  {
    err = write_ES_unit(output,&(context->end_of_stream->unit));
    if (err)
    {
      print_err("### Error writing end of stream NAL unit ");
      report_nal(FALSE,context->end_of_sequence);
      return err;
    }
    free_nal_unit(&context->end_of_stream);
  }
  return 0;
}

/*
 * Write out the (potential) trailing components of an access unit as TS.
 *
 * I.e., writes out any end of sequence or end of stream NAL unit found in the
 * `context` (since they are assumed to have immediately followed this access
 * unit).
 *
 * - `context` may contain additional things to write (see above), but may
 *   legitimately be NULL if there is no context.
 * - `tswriter` is the TS context to write with
 * - `video_pid` is the PID to use to write the data
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int write_access_unit_trailer_as_TS(access_unit_context_p  context,
                                           TS_writer_p            tswriter,
                                           uint32_t               video_pid)
{
  int err;

  if (context != NULL && context->end_of_sequence)
  {
    nal_unit_p  nal = context->end_of_sequence;
    err = write_ES_as_TS_PES_packet(tswriter,nal->unit.data,nal->unit.data_len,
                                    video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing end of sequence NAL unit ");
      report_nal(FALSE,nal);
      return err;
    }
    free_nal_unit(&context->end_of_sequence);
  }

  if (context != NULL && context->end_of_stream)
  {
    nal_unit_p  nal = context->end_of_stream;
    err = write_ES_as_TS_PES_packet(tswriter,nal->unit.data,nal->unit.data_len,
                                    video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing end of stream NAL unit ");
      report_nal(FALSE,nal);
      return err;
    }
    free_nal_unit(&context->end_of_stream);
  }
  return 0;
}

/*
 * Write out an access unit as TS.
 *
 * Also writes out any end of sequence or end of stream NAL unit found in the
 * `context` (since they are assumed to have immediately followed this access
 * unit).
 *
 * - `access_unit` is the access unit to write out
 * - `context` may contain additional things to write (see above), but may
 *   legitimately be NULL if there is no context.
 * - `tswriter` is the TS context to write with
 * - `video_pid` is the PID to use to write the data
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_access_unit_as_TS(access_unit_p          access_unit,
                                   access_unit_context_p  context,
                                   TS_writer_p            tswriter,
                                   uint32_t               video_pid)
{
  int ii, err;
  
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal = access_unit->nal_units->array[ii];

    err = write_ES_as_TS_PES_packet(tswriter,
                                    nal->unit.data,nal->unit.data_len,
                                    video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing NAL unit ");
      report_nal(FALSE,nal);
      return err;
    }
  }
  return write_access_unit_trailer_as_TS(context,tswriter,video_pid);
}

/*
 * Write out an access unit as TS, with PTS timing in the first PES packet
 * (and PCR timing in the first TS of the frame).
 *
 * Also writes out any end of sequence or end of stream NAL unit found in the
 * `context` (since they are assumed to have immediately followed this access
 * unit).
 *
 * - `access_unit` is the access unit to write out
 * - `context` may contain additional things to write (see above), but may
 *   legitimately be NULL if there is no context.
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
extern int write_access_unit_as_TS_with_pts_dts(access_unit_p          access_unit,
                                                access_unit_context_p  context,
                                                TS_writer_p            tswriter,
                                                uint32_t               video_pid,
                                                int                    got_pts,
                                                uint64_t               pts,
                                                int                    got_dts,
                                                uint64_t               dts)
{
  int ii, err;
  
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal = access_unit->nal_units->array[ii];

    // Only write the first PES packet out with PTS
    if (ii == 0)
      err = write_ES_as_TS_PES_packet_with_pts_dts(tswriter,
                                                   nal->unit.data,
                                                   nal->unit.data_len,
                                                   video_pid,
                                                   DEFAULT_VIDEO_STREAM_ID,
                                                   got_pts,pts,
                                                   got_dts,dts);
    else
      err = write_ES_as_TS_PES_packet(tswriter,
                                      nal->unit.data,nal->unit.data_len,
                                      video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing NAL unit ");
      report_nal(FALSE,nal);
      return err;
    }
  }
  return write_access_unit_trailer_as_TS(context,tswriter,video_pid);
}

/*
 * Write out an access unit as TS, with PCR timing in the first TS of the
 * frame.
 *
 * Also writes out any end of sequence or end of stream NAL unit found in the
 * `context` (since they are assumed to have immediately followed this access
 * unit).
 *
 * - `access_unit` is the access unit to write out
 * - `context` may contain additional things to write (see above), but may
 *   legitimately be NULL if there is no context.
 * - `tswriter` is the TS context to write with
 * - `video_pid` is the PID to use to write the data
 * - `pcr_base` and `pcr_extn` encode the PCR value.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_access_unit_as_TS_with_PCR(access_unit_p          access_unit,
                                            access_unit_context_p  context,
                                            TS_writer_p            tswriter,
                                            uint32_t               video_pid,
                                            uint64_t               pcr_base,
                                            uint32_t               pcr_extn)
{
  int ii, err;
  
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    nal_unit_p  nal = access_unit->nal_units->array[ii];

    // Only write the first PES packet out with PCR
    if (ii == 0)
      err = write_ES_as_TS_PES_packet_with_pcr(tswriter,
                                               nal->unit.data,
                                               nal->unit.data_len,
                                               video_pid,
                                               DEFAULT_VIDEO_STREAM_ID,
                                               pcr_base,pcr_extn);
    else
      err = write_ES_as_TS_PES_packet(tswriter,
                                      nal->unit.data,nal->unit.data_len,
                                      video_pid,DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing NAL unit ");
      report_nal(FALSE,nal);
      return err;
    }
  }
  return write_access_unit_trailer_as_TS(context,tswriter,video_pid);
}

/*
 * End this access unit.
 *
 * - `access_unit` is the access unit to end.
 * - if `show_details` is true, then a summary of its contents is printed
 *   out.
 *
 * Actually, with the current code scheme, this only does much if
 * `show_details` is true. However, it may still be a useful hook
 * for actual work later on.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static inline int end_access_unit(access_unit_context_p context,
                                  access_unit_p         access_unit,
                                  int                   show_details)
{
  if (show_details)
  {
    report_access_unit(access_unit);
    if (context->pending_nal)
    {
      print_msg("... pending: ");
      report_nal(TRUE,context->pending_nal);
    }
    if (context->end_of_sequence)
    {
      print_msg("--> EndOfSequence ");
      report_nal(TRUE,context->end_of_sequence);
    }
    if (context->end_of_stream)
    {
      print_msg("--> EndOfStream ");
      report_nal(TRUE,context->end_of_stream);
    }
  }
  return 0;
}

/*
 * Build a new access unit context datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_access_unit_context(ES_p                   es,
                                     access_unit_context_p *context)
{
  int err;
  access_unit_context_p  new = malloc(SIZEOF_ACCESS_UNIT_CONTEXT);
  if (new == NULL)
  {
    print_err("### Unable to allocate access unit context datastructure\n");
    return 1;
  }

  new->pending_nal = NULL;
  new->end_of_stream = NULL;
  new->end_of_sequence = NULL;
  new->access_unit_index = 0;
  new->reverse_data = NULL;
  new->no_more_data = FALSE;
  new->earlier_primary_start = NULL;

  err = build_nal_unit_context(es,&new->nac);
  if (err)
  {
    print_err("### Error building access unit context datastructure\n");
    free(new);
    return err;
  }
  err = build_nal_unit_list(&new->pending_list);
  if (err)
  {
    print_err("### Error building access unit context datastructure\n");
    free_nal_unit_context(&new->nac);
    free(new);
    return err;
  }

  *context = new;
  return 0;
}

/*
 * Free a new access unit context datastructure.
 *
 * Clears the datastructure, frees it, and returns `context` as NULL.
 *
 * Does not free any `reverse_data` datastructure.
 *
 * Does nothing if `context` is already NULL.
 */
extern void free_access_unit_context(access_unit_context_p *context)
{
  access_unit_context_p  cc = *context;

  if (cc == NULL)
    return;

  // We assume no-one else has an interest in the NAL units in
  // our "pending" list.
  free_nal_unit_list(&cc->pending_list,TRUE);

  // And similarly, we should be the only "person" holding on to these
  free_nal_unit(&cc->earlier_primary_start); // although this is bluff
  free_nal_unit(&cc->end_of_sequence);
  free_nal_unit(&cc->end_of_stream);
  free_nal_unit(&cc->pending_nal);

  free_nal_unit_context(&cc->nac);

  cc->reverse_data = NULL;
  
  free(*context);
  *context = NULL;
  return;
}

/*
 * Reset an acccess unit context, so it "forgets" its current information
 * about what it is reading, etc.
 */
extern void reset_access_unit_context(access_unit_context_p  context)
{
  free_nal_unit(&context->earlier_primary_start);
  free_nal_unit(&context->end_of_sequence);
  free_nal_unit(&context->end_of_stream);
  free_nal_unit(&context->pending_nal);
  reset_nal_unit_list(context->pending_list,FALSE); // @@@ leak???
  context->no_more_data = FALSE;
  // We have to hope that the "previous" sequence parameter and picture
  // parameter dictionaries are still applicable, since we don't still
  // have a record of the ones that would have been in effect at this
  // point.
}

/*
 * Rewind a file being read as access units.
 *
 * This is a wrapper for `rewind_nal_unit_context` that also knows to
 * unset things appropriate to the access unit context.
 *
 * If a reverse context is attached to this access unit, it also will
 * be "rewound" appropriately.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int rewind_access_unit_context(access_unit_context_p  context)
{
  // First, forget where we are
  reset_access_unit_context(context);

  context->access_unit_index = 0;  // no access units read from this file yet

  // Next, take care of rewinding
  if (context->reverse_data)
  {
    context->reverse_data->last_posn_added = -1; // next entry to be 0
  }

  // And then, do the relocation itself
  return rewind_nal_unit_context(context->nac);
}

/*
 * Remember the required information from the previous access unit's
 * first VLC NAL unit (i.e., the one that starts its primary picture).
 *
 * If we just remembered the (address of the) NAL unit itself, we would
 * have a problem if/when the access unit containing it was freed, since
 * that would also free the NAL unit. Luckily, the information we want
 * to remember is well defined, and does not require us to do anything
 * other than copy data, so we can reuse the same "internal" NAL unit
 * without needing to do lots of mallocing around.
 *
 * It *should* be obvious, given its intended use, but do not call this
 * on a NAL unit that has not been decoded - things may fall apart
 * messily later on...
 *
 * (NB: the "pseudo" NAL unit we use to remember the information is
 * a true NAL unit except for not having any of the data/rbsp arrays
 * filled in, so it *does* cause the NAL unit id to be incremented,
 * which has confused me at least once when reading diagnostic output.)
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int remember_earlier_primary_start(access_unit_context_p context,
                                          nal_unit_p            nal)
{
  nal_unit_p  tgt = context->earlier_primary_start;

  if (tgt == NULL)
  {
    int err = build_nal_unit(&tgt);
    if (err)
    {
      print_err("### Error building NAL unit for 'earlier primary start'\n");
      free(tgt);
      return err;
    }
    context->earlier_primary_start = tgt;
  }
  
  tgt->starts_picture_decided = nal->starts_picture_decided;
  tgt->starts_picture = nal->starts_picture;
  tgt->start_reason = nal->start_reason;
  tgt->decoded = nal->decoded;

  tgt->nal_ref_idc = nal->nal_ref_idc;
  tgt->nal_unit_type = nal->nal_unit_type;
  tgt->u = nal->u;

  // Lastly, we may not need the following, but they are sufficient to
  // allow us to read the whole NAL unit back in if we should need to.
  tgt->unit.start_posn = nal->unit.start_posn;
  tgt->unit.data_len = nal->unit.data_len;
  
  return 0;
}

/*
 * Maybe remember an access unit for reversing - either an IDR or one with all
 * frames I
 */
static int maybe_remember_access_unit(reverse_data_p  reverse_data,
                                      access_unit_p   access_unit,
                                      int             verbose)
{
  // Keep it if it is an IDR, or all of its contents are I slices
  if (access_unit->primary_start != NULL &&
      access_unit->primary_start->nal_ref_idc != 0 &&
      (access_unit->primary_start->nal_unit_type == NAL_IDR ||
       all_slices_I(access_unit)))
  {
    ES_offset  start_posn = {0,0};
    uint32_t   num_bytes = 0;
    int err = get_access_unit_bounds(access_unit,&start_posn,&num_bytes);
    if (err)
    {
      fprint_err("### Error working out position/size of access unit %d"
                 " for reversing\n",access_unit->index);
      return 1;
    }
    err = remember_reverse_h264_data(reverse_data,access_unit->index,
                                     start_posn,num_bytes);
    if (err)
    {
      fprint_err("### Error remembering access unit %d for reversing\n",
                 access_unit->index);
      return 1;
    }
    if (verbose) fprint_msg("REMEMBER IDR %5d at " OFFSET_T_FORMAT_08
                            "/%04d for %5d\n",access_unit->index,
                            start_posn.infile,start_posn.inpacket,num_bytes);
  }
  return 0;
}

/*
 * Retrieve the next access unit from the given elementary stream.
 *
 * - `context` is the context information needed to allow us to find
 *   successive access units.
 * - `quiet` is true if we should try to be silent about it
 * - `show_details` is true if we should output more info than normal
 * - `ret_access_unit` is the next access unit.
 *
 * If the access unit was ended because an end of sequence or end of
 * stream NAL unit was encountered, then said end of sequence/stream
 * NAL unit will be remembered in the `context`.
 *
 * Note that it is possible to get back an *empty* access unit in
 * certain situations - the most obvious of which is if we get two
 * ``end of sequence`` NAL units with nothing betwen them.
 *
 * Because of this possibility, some care should be taken to allow for
 * access units that do not contain a primary picture (no VCL NAL unit),
 * and contain zero NAL units. Also, if one is trying for an accurate
 * count of access units, such instances should probably be ignored.
 *
 * Returns 0 if it succeeds, EOF if there is no more data to read, or 1 if
 * some error occurs.
 *
 * EOF can be returned because the end of file has been reached, or because an
 * end of stream NAL unit has been encountered. The two may be distinguished
 * by looking at `context->end_of_stream`, which will be NULL if it was a true
 * EOF.
 *
 * Note that `ret_access_unit` will be NULL if EOF is returned.
 */
extern int get_next_access_unit(access_unit_context_p context,
                                int                   quiet,
                                int                   show_details,
                                access_unit_p        *ret_access_unit)
{
  int            err;
  nal_unit_p     nal = NULL;
  access_unit_p  access_unit;

  // Is there anything more to read from the input stream?
  if (context->no_more_data)
  {
    *ret_access_unit = NULL;
    return EOF;
  }
  
  // Since we're expecting to return a new access unit,
  // we'd better build it...
  err = build_access_unit(&access_unit,context->access_unit_index+1);
  if (err) return err;

  // Did we have any left over stuff to put at its start?
  if (context->pending_nal != NULL)
  {
    err = access_unit_append(access_unit,
                             context->pending_nal,TRUE,context->pending_list);
    if (err) goto give_up;
    context->pending_nal = NULL;
    reset_nal_unit_list(context->pending_list,FALSE);
  }
  
  for (;;)
  {
    err = find_next_NAL_unit(context->nac,FALSE,&nal);
    if (err == EOF)
    {
      context->no_more_data = TRUE;  // prevent future reads on this stream
      break;
    }
    else if (err == 2)
    {
      // The NAL unit was broken. Should we:
      // a) ignore it and pretend it never happened (i.e., ``continue``)
      // b) ignore it and give up on the current access unit (i.e., unset
      //    our current status, and hunt for the start of the next access
      //    unit).
      // Clearly, option (a) is the easiest to try, so let's see how that
      // works for now...
      print_err("!!! Ignoring broken NAL unit\n");
      access_unit->ignored_broken_NAL_units ++;
      continue;
    }
    else if (err)
    {
      print_err("### Error retrieving next NAL\n");
      goto give_up;
    }
    
    if (nal_is_slice(nal))
    {
      if (!access_unit->started_primary_picture)
      {
        // We're in a new access unit, but we haven't had a slice
        // yet, so we can be lazy and assume that this must be the
        // first slice
        // (What we're *not* checking is whether the first access
        // unit in the bitstream starts with an IDR, which might be
        // a good idea)
        nal->start_reason = "First slice of new access unit";
        err = access_unit_append(access_unit,nal,TRUE,context->pending_list);
        if (err) goto give_up_free_nal;
        reset_nal_unit_list(context->pending_list,FALSE);
        err = remember_earlier_primary_start(context,nal);
        if (err) goto give_up_free_nal;
      }
      else if (nal_is_first_VCL_NAL(nal,context->earlier_primary_start))
      {
        // Regardless of what we determine next, we need to remember that the
        // NAL started (what may later be the previous) access unit
        err = remember_earlier_primary_start(context,nal);
        if (err) goto give_up_free_nal;
        if (access_unit->started_primary_picture)
        {
          // We were already in an access unit with a primary
          // picture, so this NAL unit must start a new access unit.
          // Remember it for next time, and return the access unit so far.
          context->pending_nal = nal;
          break;  // Ready to return the access unit
        }
        else
        {
          // This access unit was waiting for its primary picture
          err = access_unit_append(access_unit,nal,TRUE,context->pending_list);
          if (err) goto give_up_free_nal;
          reset_nal_unit_list(context->pending_list,FALSE);
        }
      }
      else if (!access_unit->started_primary_picture)
      {
        // But this is not a NAL unit that may start a new
        // access unit. So what should we do? Ignore it?
        if (!quiet)
        {
          print_err("!!! Ignoring VCL NAL that cannot start a picture:\n");
          print_err("    ");
          report_nal(FALSE,nal);
          print_err("\n");
        }
        free_nal_unit(&nal);
      }
      else if (nal_is_redundant(nal))
      {
        // pass
        // print_msg("   ignoring redundant NAL unit\n");
        free_nal_unit(&nal);
      }
      else
      {
        // We're part of the same access unit, but not special
        err = access_unit_append(access_unit,nal,FALSE,context->pending_list);
        if (err) goto give_up_free_nal;
        reset_nal_unit_list(context->pending_list,FALSE);
      }
    }
    else if (nal->nal_unit_type == NAL_ACCESS_UNIT_DELIM)
    {
      // We always start an access unit...
      if (access_unit->started_primary_picture)
      {
        err = append_to_nal_unit_list(context->pending_list,nal);
        if (err) goto give_up_free_nal;
        break; // Ready to return the "previous" access unit
      }
      else
      {
        // The current access unit doesn't yet have any VCL NALs
        if (context->pending_list->length > 0 ||
            access_unit->nal_units->length > 0)
        {
          print_err("!!! Ignoring incomplete access unit:\n");
          if (access_unit->nal_units->length > 0)
          {
            report_nal_unit_list(FALSE,"    ",access_unit->nal_units);
            reset_nal_unit_list(access_unit->nal_units,TRUE);
          }
          if (context->pending_list->length > 0)
          {
            report_nal_unit_list(FALSE,"    ",context->pending_list);
            reset_nal_unit_list(context->pending_list,TRUE);
          }
        }
        err = access_unit_append(access_unit,nal,FALSE,NULL);
        if (err) goto give_up_free_nal;
      }
    }
    else if (nal->nal_unit_type == NAL_SEI)
    {
      // SEI units always precede the primary coded picture
      // - so they also implicitly end any access unit that has already
      // started its primary picture
      if (access_unit->started_primary_picture)
      {
        err = append_to_nal_unit_list(context->pending_list,nal);
        if (err) goto give_up_free_nal;
        break; // Ready to return the "previous" access unit
      }
      else
      {
        err = append_to_nal_unit_list(context->pending_list,nal);
        if (err) goto give_up_free_nal;
      }
    }
    else if (nal->nal_unit_type == NAL_SEQ_PARAM_SET || 
             nal->nal_unit_type == NAL_PIC_PARAM_SET ||
             nal->nal_unit_type == 13 ||
             nal->nal_unit_type == 14 ||
             nal->nal_unit_type == 15 ||
             nal->nal_unit_type == 16 ||
             nal->nal_unit_type == 17 ||
             nal->nal_unit_type == 18)
    {
      // These start a new access unit *if* they come after the
      // last VCL NAL of an access unit. But we can only *tell*
      // that they are after the last VCL NAL of an access unit
      // when we start the next access unit (!) - so we need to
      // hold them in hand until we know that we need them.
      // (i.e., they'll get added to an access unit just before
      // the next "more determined" NAL unit we add to an access
      // unit)
      err = append_to_nal_unit_list(context->pending_list,nal);
      if (err) goto give_up_free_nal;
    }
    else if (nal->nal_unit_type == NAL_END_OF_SEQ)
    {
      if (context->pending_list->length > 0)
      {
        print_err("!!! Ignoring items after last VCL NAL and"
                  " before End of Sequence:\n");
        report_nal_unit_list(FALSE,"    ",context->pending_list);
        reset_nal_unit_list(context->pending_list,TRUE);
      }
      // And remember this as the End of Sequence marker
      context->end_of_sequence = nal;
      break;
    }
    else if (nal->nal_unit_type == NAL_END_OF_STREAM)
    {
      if (context->pending_list->length > 0)
      {
        print_err("!!! Ignoring items after last VCL NAL and"
                  " before End of Stream:\n");
        report_nal_unit_list(FALSE,"    ",context->pending_list);
        reset_nal_unit_list(context->pending_list,TRUE);
      }
      // And remember this as the End of Stream marker
      context->end_of_stream = nal;
      // Which means there's no point in reading more from this stream
      // (setting no_more_data like this means that *next* time this
      // function is called, it will return EOF)
      context->no_more_data = TRUE;
      break;
    }
    else
    {
      // It's not a slice, or an access unit delimiter, or an
      // end of sequence or stream, or a sequence or picture
      // parameter set, or various other odds and ends, so it
      // looks like we can ignore it.
      free_nal_unit(&nal);
    }
  }

  // Check for an immediate "end of file with no data"
  // - i.e., we read EOF or end of stream, and there was nothing
  // between the last access unit and such reading
  if (context->no_more_data && access_unit->nal_units->length == 0)
  {
    free_access_unit(&access_unit);
    *ret_access_unit = NULL;
     return EOF;
  }
  
  // Otherwise, finish off and return the access unit we have in hand
  err = end_access_unit(context,access_unit,show_details);
  if (err) goto give_up;

  // Remember to count it
  context->access_unit_index ++;

  *ret_access_unit = access_unit;
  return 0;

give_up_free_nal:
  free_nal_unit(&nal);
give_up:
  free_access_unit(&access_unit);
  return 1;
}

/*
 * Retrieve the next non-empty access unit from the given elementary stream.
 *
 * - `context` is the context information needed to allow us to find
 *   successive access units.
 * - `quiet` is true if we should try to be silent about it
 * - `show_details` is true if we should output more info than normal
 * - `frame` is an access unit datastructure representing the next
 *   frame.
 *
 * If the access unit was ended because an end of sequence or end of
 * stream NAL unit was encountered, then said end of sequence/stream
 * NAL unit will be remembered in the `context`.
 *
 * Returns 0 if it succeeds, EOF if there is no more data to read, or 1 if
 * some error occurs.
 *
 * EOF can be returned because the end of file has been reached, or because an
 * end of stream NAL unit has been encountered. The two may be distinguished
 * by looking at `context->end_of_stream`, which will be NULL if it was a true
 * EOF.
 *
 * Note that `ret_access_unit` will be NULL if EOF is returned.
 */
static int get_next_non_empty_access_unit(access_unit_context_p context,
                                          int                   quiet,
                                          int                   show_details,
                                          access_unit_p        *access_unit)
{
  for (;;)
  {
    int err = get_next_access_unit(context,quiet,show_details,access_unit);
    if (err) return err;

    if ((*access_unit)->primary_start)
      return 0;
  }
}

/*
 * Try for the next field of a pair, and return a frame formed therefrom
 *
 * - `context` is the context information needed to allow us to find
 *   successive access units.
 * - `quiet` is true if we should try to be silent about it
 * - `show_details` is true if we should output more info than normal
 * - if `first_time` is true, then we will try to match a second field
 *   with a third, if the second field has a different temporal reference
 *   than the first. If it is false, we will not (thus stopping us from
 *   trying forever...)
 * - `picture` starts out at the first field of our (hoped for) pair, and
 *   will end up as the merged result of our two fields. If the input stream
 *   is awry (or we are misaligned with respect to it), this might instead be
 *   replaced by a "proper" frame.
 *
 * Returns 0 if it succeeds, EOF if there is no more data to read, or 1 if
 * some error occurs.
 */
static int get_next_field_of_pair(access_unit_context_p  context,
                                  int                    quiet,
                                  int                    show_details,
                                  int                    first_time,
                                  access_unit_p         *access_unit)
{
  int  err;
  access_unit_p  second;

  if (show_details || context->nac->show_nal_details)
    fprint_msg("@@ Looking for second field (%s time)\n",
               (first_time?"first":"second"));
  
  // We assume (hope) the next picture will be our second half
  err = get_next_non_empty_access_unit(context,quiet,show_details,&second);
  if (err)
  {
    if (err != EOF)
      print_err("### Trying to read second field\n");
    return err;
  }
  
  if (second->field_pic_flag == 0)
  {
    if (!quiet)
      print_err("!!! Field followed by a frame - ignoring the field\n");
    free_access_unit(access_unit);
    *access_unit = second;
    // and pretend to success
  }
  else if ((*access_unit)->frame_num == second->frame_num)
  {
    // They appear to be matching fields - make a frame from them
    if (show_details || context->nac->show_nal_details)
      print_msg("@@ Merging two field access units\n");
    err = merge_access_unit_nals(*access_unit,&second); // (frees `second`)
    if (err)
    {
      free_access_unit(&second);
      return 1;
    }
    if (show_details)
      report_access_unit(*access_unit);
  }
  else if (first_time)
  {
    if (!quiet)
      fprint_err("!!! Field with frame number %d (%x) followed by"
                 " field with frame number %d (%x) - ignoring first field\n",
                 (*access_unit)->frame_num,(*access_unit)->frame_num,
                 second->frame_num,second->frame_num);

    // Try again
    free_access_unit(access_unit);
    *access_unit = second;
    err = get_next_field_of_pair(context,quiet,show_details,FALSE,access_unit);
    if (err) return 1;
  }
  else
  {
    print_err("### Adjacent fields do not share frame numbers"
              " - unable to match fields up\n");
    return 1;
  }
  return 0;
}

/*
 * Retrieve the next H.264 frame from the given elementary stream.
 *
 * The next access unit is retrieved from the input stream (using
 * get_next_access_unit).
 *
 * If that access unit represents a frame, it is returned.
 *
 * If it represents a field, then the *following* access unit is retrieved,
 * and if that is the second field of its frame, it is merged into the first,
 * and the resultant frame is returned.
 *
 * If a field with frame number A is followed by a field with frame number B,
 * it is assumed that synchronisation has been lost. In this case, the first
 * field (frame A) will be discarded, and an attempt made to read the second
 * field of frame B.
 *
 * Similarly, if a frame is found instead of the second field, the first
 * field will be discarded and the frame returned.
 *
 *   Note that if the context is associated with a reverse context,
 *   then appropriate frames will automatically be remembered therein.
 *
 * - `context` is the context information needed to allow us to find
 *   successive access units.
 * - `quiet` is true if we should try to be silent about it
 * - `show_details` is true if we should output more info than normal
 * - `frame` is an access unit datastructure representing the next
 *   frame.
 *
 * If the access unit was ended because an end of sequence or end of
 * stream NAL unit was encountered, then said end of sequence/stream
 * NAL unit will be remembered in the `context`.
 *
 * Returns 0 if it succeeds, EOF if there is no more data to read, or 1 if
 * some error occurs.
 *
 * EOF can be returned because the end of file has been reached, or because an
 * end of stream NAL unit has been encountered. The two may be distinguished
 * by looking at `context->end_of_stream`, which will be NULL if it was a true
 * EOF.
 *
 * Note that `ret_access_unit` will be NULL if EOF is returned.
 */
extern int get_next_h264_frame(access_unit_context_p context,
                               int                   quiet,
                               int                   show_details,
                               access_unit_p        *frame)
{
  int  err;
  access_unit_p  access_unit;

  *frame = NULL;

  err = get_next_non_empty_access_unit(context,quiet,show_details,
                                       &access_unit);
  if (err) return err;

  if (access_unit->field_pic_flag == 1)
  {
    // We assume (hope) the next access_unit will be our second half
    // - let's try to get it, and merge it into our current access unit
    err = get_next_field_of_pair(context,quiet,show_details,TRUE,&access_unit);
    if (err)
    {
      free_access_unit(&access_unit);
      return 1;
    }
  }

  if (context->reverse_data)
  {
    err = maybe_remember_access_unit(context->reverse_data,access_unit,
                                     show_details);
    if (err)
    {
      free_access_unit(&access_unit);
      return 1;
    }
  }
  *frame = access_unit;
  return 0;
}

/*
 * If this access unit was read from PES, did any of its PES packets contain
 * a PTS?
 *
 * Returns TRUE if so, FALSE if not.
 */
extern int access_unit_has_PTS(access_unit_p access_unit)
{
  // We need to look at each ES unit (within each NAL unit) of this access unit
  int ii;
  for (ii=0; ii<access_unit->nal_units->length; ii++)
  {
    if (access_unit->nal_units->array[ii]->unit.PES_had_PTS)
      return TRUE;
  }
  return FALSE;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

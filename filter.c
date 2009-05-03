/*
 * Support for "filtering" ES, outputting to either ES or TS.
 *
 * This provides the ability to "fast forward" through ES data.
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
#include "ts_fns.h"
#include "accessunit_fns.h"
#include "h262_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "filter_fns.h"

#define DEBUG 0


// ============================================================
// Managing H.262 filter contexts
// ============================================================
/*
 * Build a new H.262 (MPEG-2 and also MPEG-1) filter context
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
static int new_h262_filter_context(h262_filter_context_p *fcontext)
{
  h262_filter_context_p new = malloc(SIZEOF_H262_FILTER_CONTEXT);
  if (new == NULL)
  {
    print_err("### Unable to allocate H.262 filter context\n");
    return 1;
  }
  new->h262 = NULL;
  new->last_seq_hdr = NULL;
  new->new_seq_hdr  = FALSE;

  reset_h262_filter_context(new);
  
  *fcontext = new;
  return 0;
}

/*
 * Build a new filter context for "stripping" H.262 data
 *
 * - `fcontext` is the new filter context
 * - `h262` is the H.262 stream to read from
 * - `all_IP` is true if the software should keep all I and P pictures
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int build_h262_filter_context_strip(h262_filter_context_p *fcontext,
                                           h262_context_p         h262,
                                           int                    all_IP)
{
  int err = new_h262_filter_context(fcontext);
  if (err) return 1;

  (*fcontext)->h262 = h262;
  (*fcontext)->filter = FALSE;
  (*fcontext)->allref = all_IP;
  return 0;
}

/*
 * Build a new filter context for "filtering" H.262 data
 *
 * - `fcontext` is the new filter context
 * - `h262` is the H.262 stream to read from
 * - `freq` is the desired speed-up, or the frequency at which frames
 *   should (ideally) be kept
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int build_h262_filter_context(h262_filter_context_p *fcontext,
                                     h262_context_p         h262,
                                     int                    freq)
{
  int err = new_h262_filter_context(fcontext);
  if (err) return 1;

  (*fcontext)->h262 = h262;
  (*fcontext)->filter = TRUE;
  (*fcontext)->freq = freq;
  return 0;
}

/*
 * Reset an H.262 filter context, ready to start filtering anew.
 */
extern void reset_h262_filter_context(h262_filter_context_p  fcontext)
{
  fcontext->pending_EOF = FALSE;
  fcontext->last_was_slice = FALSE;
  fcontext->had_previous_picture = FALSE;
  if (fcontext->last_seq_hdr != NULL)
    free_h262_picture(&fcontext->last_seq_hdr);
  fcontext->new_seq_hdr = FALSE;

  fcontext->count = 0;
  fcontext->frames_seen = 0;
  fcontext->frames_written = 0;
}

/*
 * Free a filter context
 *
 * NOTE that this does *not* free the H.262 datastructure to which the
 * filter context refers.
 *
 * - `fcontext` is the filter context, which will be freed, and returned
 *   as NULL.
 */
extern void free_h262_filter_context(h262_filter_context_p *fcontext)
{
  if ((*fcontext) == NULL)
    return;

  // It's a little wasteful to call this, but on the other hand it is
  // guaranteed to free everything we want freeing
  reset_h262_filter_context(*fcontext);

  // Just lose our reference to the H.262 datastructure, don't free it
  (*fcontext)->h262 = NULL;
  
  free(*fcontext);
  *fcontext = NULL;
  return;
}

// ============================================================
// Managing H.264 filter contexts
// ============================================================
/*
 * Build a new H.264 filter context
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
static int new_h264_filter_context(h264_filter_context_p *fcontext)
{
  h264_filter_context_p new = malloc(SIZEOF_H264_FILTER_CONTEXT);
  if (new == NULL)
  {
    print_err("### Unable to allocate H.264 filter context\n");
    return 1;
  }

  // Unset the important things - things we might otherwise try to free
  // (or, for new->es, things that stop us doing anything until we're
  // setup properly by the user)
  new->access_unit_context = NULL;

  reset_h264_filter_context(new);
  
  *fcontext = new;
  return 0;
}

/*
 * Build a new filter context for "stripping" ES data
 *
 * - `fcontext` is the new filter context
 * - `access` is the access unit context to read from
 * - `allref` is true if the software should keep all reference pictures
 *   (H.264) or all I and P pictures (H.264)
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int build_h264_filter_context_strip(h264_filter_context_p *fcontext,
                                           access_unit_context_p  access,
                                           int                    allref)
{
  int err = new_h264_filter_context(fcontext);
  if (err) return 1;

  (*fcontext)->access_unit_context = access;
  (*fcontext)->filter = FALSE;
  (*fcontext)->allref = allref;
  return 0;
}

/*
 * Build a new filter context for "filtering" ES data
 *
 * - `fcontext` is the new filter context
 * - `access` is the access unit context to read from
 * - `freq` is the desired speed-up, or the frequency at which frames
 *   should (ideally) be kept
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int build_h264_filter_context(h264_filter_context_p *fcontext,
                                     access_unit_context_p  access,
                                     int                    freq)
{
  int err = new_h264_filter_context(fcontext);
  if (err) return 1;

  (*fcontext)->access_unit_context = access;
  (*fcontext)->filter = TRUE;
  (*fcontext)->freq = freq;
  return 0;
}

/*
 * Reset an H.264 filter context, ready to start filtering anew.
 */
extern void reset_h264_filter_context(h264_filter_context_p  fcontext)
{
  // `skipped_ref_pic` is TRUE if we've skipped any reference pictures
  // since our last IDR (hmm - should it start off True or False?)
  fcontext->skipped_ref_pic = FALSE;
  // `last_accepted_was_not_IDR` is TRUE if the last frame kept (output)
  // was not an IDR. We set it TRUE initially so that we will decide
  // to output the first IDR we *do* find, regardless of the count.
  fcontext->last_accepted_was_not_IDR = TRUE;
  // And plainly we didn't have a previous access unit
  fcontext->had_previous_access_unit = FALSE;
  // Especially not an IDR
  fcontext->not_had_IDR = TRUE;
  
  fcontext->count = 0;
  fcontext->frames_seen = 0;
  fcontext->frames_written = 0;
}

/*
 * Free an H.264 filter context
 *
 * NOTE that this does *not* free the access unit context to which the
 * filter context refers.
 *
 * - `fcontext` is the filter context, which will be freed, and returned
 *   as NULL.
 */
extern void free_h264_filter_context(h264_filter_context_p *fcontext)
{
  if ((*fcontext) == NULL)
    return;

  // It's a little wasteful to call this, but on the other hand it is
  // guaranteed to free everything we want freeing
  reset_h264_filter_context(*fcontext);

  // Just lose our reference to the access unit context, don't free it
  (*fcontext)->access_unit_context = NULL;
  
  free(*fcontext);
  *fcontext = NULL;
  return;
}

// ============================================================
// Filtering H.262
// ============================================================
/*
 * Retrieve the next I (and/or, if fcontext->allref, P) frame in this H.262 ES.
 *
 * Any sequence end "pictures" will be ignored.
 *
 * Note that the ES data being read should be video-only.
 *
 * - `fcontext` is the information that tells us what to filter and how
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * - `seq_hdr` is a sequence header, i.e., that used by the next frame to
 *   output. This will be NULL if the sequence header has not changed since
 *   the last call of this function.
 *
 *   Note that the caller should *not* free this, and that it will not be
 *   maintained over calls of this function (i.e., it is a reference to a
 *   value within the `fcontext` which is altered by this function).
 *
 * - `frame` is the next frame to output.
 *
 *   Note that it is the caller's responsibility to free this with
 *   `free_h262_picture()`.
 *
 *   If an error or EOF is returned, this value is undefined.
 *
 * - `frames_seen` is the number of I and P frames (start code 0)
 *   found by this call of the function, including the item returned
 *   if appropriate.
 *
 * Returns 0 if it succeeds, EOF if end-of-file is read (or the last call
 * returned a sequence end item), 1 if some error occurs.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int get_next_stripped_h262_frame(h262_filter_context_p  fcontext,
                                        int                    verbose,
                                        int                    quiet,
                                        h262_picture_p        *seq_hdr,
                                        h262_picture_p        *frame,
                                        int                   *frames_seen)
{
  int  err;

  // A picture is built up from several items - we start with none in hand
  h262_picture_p this_picture = NULL;

  *frames_seen = 0;

  if (fcontext->filter)
  {
    print_err("### Calling get_next_stripped_h262_frame with a context"
              " set for filtering\n");
    return 1;
  }

  // Otherwise, look for something we want to keep
  for (;;)
  {
    if (es_command_changed(fcontext->h262->es))
    {
      *frame = *seq_hdr = NULL;
      return COMMAND_RETURN_CODE;
    }

    err = get_next_h262_frame(fcontext->h262,verbose,quiet,&this_picture);
    if (err == EOF)
    {
      *frame = *seq_hdr = NULL;
      return err;
    }
    else if (err)
    {
      print_err("### Error filtering H.262 frames\n");
      return 1;
    }

    // Now to stripping
    if (this_picture->is_picture)
    {
      (*frames_seen) ++;
      if ((this_picture->picture_coding_type == 1) ||
          (this_picture->picture_coding_type == 2 && fcontext->allref) )
      {
        *frame = this_picture;
        if (fcontext->new_seq_hdr)
          *seq_hdr = fcontext->last_seq_hdr;
        else
          *seq_hdr = NULL;
        fcontext->new_seq_hdr = FALSE;
        if (verbose) fprint_msg(">> %s picture \n",
                                (this_picture->picture_coding_type==1?"I":"P"));
        return 0;
      }
      else
        free_h262_picture(&this_picture);
    }
    else if (this_picture->is_sequence_header)
    {
      // We maybe want to remember this sequence header for the next picture
      if (fcontext->last_seq_hdr == NULL)
      {
        fcontext->last_seq_hdr = this_picture;
        fcontext->new_seq_hdr = TRUE;
        if (verbose) print_msg(">> First sequence header\n");
      }
      else if (!same_h262_picture(this_picture,fcontext->last_seq_hdr))
      {
        if (verbose) print_msg(">> Different sequence header\n");
        free_h262_picture(&fcontext->last_seq_hdr);
        fcontext->last_seq_hdr = this_picture;
        fcontext->new_seq_hdr = TRUE;
      }
      else
      {
        fcontext->new_seq_hdr = FALSE;
        if (verbose) print_msg(">> Identical sequence header\n");
        free_h262_picture(&this_picture);
      }
    }
  }
}

/*
 * Retrieve the next I frame, from the H.262 ES, aiming for an "apparent" kept
 * frequency as stated.
 *
 * Any sequence end "pictures" will be ignored.
 *
 * Note that the ES data being read should be video-only.
 *
 * - `fcontext` is the information that tells us what to filter and how
 *   (including the desired frequency)
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * - `seq_hdr` is a sequence header, i.e., that used by the next picture to
 *   output. This will be NULL if `frame` is NULL.
 *
 *   Note that the caller should *not* free this, and that it will not be
 *   maintained over calls of this function (i.e., it is a reference to a
 *   value within the `fcontext` which is altered by this function).
 *
 * - `frame` is the next frame to output. This will be NULL if the last frame
 *   should be output again, to provide the requested apparent frequency.
 *
 *   Note that it is the caller's responsibility to free this with
 *   `free_h262_picture()`.
 *
 *   If an error or EOF is returned, this value is undefined.
 *
 * - `frames_seen` is the number of I and P frames found by this call of
 *   the function, including the item returned if appropriate.
 *
 * Returns 0 if it succeeds, EOF if end-of-file is read, or we've just read a
 * sequence end item, or the last call ended a picture on a sequence end
 * item, 1 if some error occurs.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int get_next_filtered_h262_frame(h262_filter_context_p  fcontext,
                                        int                    verbose,
                                        int                    quiet,
                                        h262_picture_p        *seq_hdr,
                                        h262_picture_p        *frame,
                                        int                   *frames_seen)
{
  int  err;

  // A picture is built up from several items - we start with none in hand
  h262_picture_p this_picture = NULL;

  *frames_seen = 0;

  if (!fcontext->filter)
  {
    print_err("### Calling get_next_filtered_h262_frame with a context"
              " set for stripping\n");
    return 1;
  }

  // Otherwise, look for something we want to keep
  for (;;)
  {
    if (es_command_changed(fcontext->h262->es))
    {
      *frame = *seq_hdr = NULL;
      return COMMAND_RETURN_CODE;
    }

    // If the picture is an I picture, we want it to contain an appropriate
    // AFD - so ask for that
    fcontext->h262->add_fake_afd = TRUE;
    
    err = get_next_h262_frame(fcontext->h262,verbose,quiet,&this_picture);
    if (err == EOF)
    {
      *frame = *seq_hdr = NULL;
      fcontext->h262->add_fake_afd = FALSE;
      return err;
    }
    else if (err)
    {
      print_err("### Error filtering H.262 frames\n");
      fcontext->h262->add_fake_afd = FALSE;
      return 1;
    }

    // Reinstate normal "only include actual AFDs"
    fcontext->h262->add_fake_afd = FALSE;

    // Now to filtering
    if (this_picture->is_picture)
    {
      fcontext->count ++;
      (*frames_seen) ++;

      fcontext->frames_seen ++;

      if (this_picture->picture_coding_type == 1 &&
          fcontext->count < fcontext->freq)
      {
        // It is an I picture, but it is too soon
        if (verbose)
        {
          fprint_msg("+++ %d/%d DROP: Too soon\n",fcontext->count,fcontext->freq);
        }
      }
      else if (this_picture->picture_coding_type != 1)
      {
        // It is not an I picture
        if (verbose)
        {
          fprint_msg("+++ %d/%d DROP: %s picture\n",fcontext->count,fcontext->freq,
                 H262_PICTURE_CODING_STR(this_picture->picture_coding_type));
        }
        // But do we want to pad with (i.e., repeat) the previous I picture?
        if (fcontext->freq > 0)
        {
          int pictures_wanted = fcontext->frames_seen / fcontext->freq;
          int repeat = pictures_wanted - fcontext->frames_written;
          if (repeat > 0 && fcontext->had_previous_picture)
          {
            if (verbose) print_msg(">>> output last picture again\n");
            free_h262_picture(&this_picture);
            *seq_hdr = NULL;
            *frame = NULL;
            fcontext->frames_written ++;
            return 0;
          }
        }
      }
      else
      {
        // It was an I picture, and not too soon
        if (verbose)
        {
          fprint_msg("+++ %d/%d KEEP\n",fcontext->count,fcontext->freq);
        }
        fcontext->count = 0;
        fcontext->had_previous_picture = TRUE;
        *seq_hdr = fcontext->last_seq_hdr;
        *frame = this_picture;

        fcontext->frames_written ++;
        return 0;
      }
      free_h262_picture(&this_picture);
    }
    else if (this_picture->is_sequence_header)
    {
      // We want to remember the sequence header for the next picture
      if (fcontext->last_seq_hdr != NULL)
        free_h262_picture(&fcontext->last_seq_hdr);
      fcontext->last_seq_hdr = this_picture;
    }
  }
}

// ============================================================
// Filtering H.264
// ============================================================
/*
 * Return the next IDR or I (and maybe any reference) frame from this H.264 ES.
 *
 * Note that the ES data being read should be video-only.
 *
 * - `fcontext` is the information that tells us what to filter and how
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `frame` is the next frame to output.
 *   Note that it is the caller's responsibility to free this with
 *   `free_access_unit()`.
 *   If an error or EOF is returned, this value is undefined.
 * - `frames_seen` is the number of frames found by this call
 *   of the function, including the frame returned.
 *
 * Returns 0 if it succeeds, EOF if end-of-file is read (or an an end of
 * stream NAL unit has been passed), 1 if some error occurs.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int get_next_stripped_h264_frame(h264_filter_context_p  fcontext,
                                        int                    verbose,
                                        int                    quiet,
                                        access_unit_p         *frame,
                                        int                   *frames_seen)
{
  int err = 0;
  int keep = FALSE;  // Should we keep the current access unit?
  access_unit_p  this_access_unit = NULL;

  *frames_seen = 0;
  
  for (;;)
  {
    if (es_command_changed(fcontext->access_unit_context->nac->es))
      return COMMAND_RETURN_CODE;

    if (verbose)
      print_msg("\n");

    err = get_next_h264_frame(fcontext->access_unit_context,quiet,verbose,
                              &this_access_unit);
    if (err == EOF)
      return err;
    else if (err)
      return 1;

    (*frames_seen) ++;
    
    if (this_access_unit->primary_start == NULL)
    {
      // We don't have a primary picture - no VCL NAL
      // There seems little point in keeping the access unit
      keep = FALSE;
      if (verbose)
        print_msg("++ DROP: no primary picture\n");
    }
    else if (this_access_unit->primary_start->nal_ref_idc == 0)
    {
      // This is not a reference frame, so it's of no interest
      keep = FALSE;
      if (verbose)
        print_msg("++ DROP: not reference\n");
    }
    else if (fcontext->allref)
    {
      // We want to keep all reference frames
      if (this_access_unit->primary_start->nal_unit_type == NAL_IDR ||
          this_access_unit->primary_start->nal_unit_type == NAL_NON_IDR)
      {
        keep = TRUE;
        if (verbose)
          print_msg("++ KEEP: reference picture\n");
      }
      else
      {
        keep = FALSE;
        if (verbose)
          print_msg("++ DROP: sequence or parameter set, etc.\n");
      }
    }
    else
    {
      // We only want to keep IDR and I frames
      if (this_access_unit->primary_start->nal_unit_type == NAL_IDR)
      {
        keep = TRUE;
        if (verbose)
          print_msg("++ KEEP: IDR picture\n");
      }
      else if (this_access_unit->primary_start->nal_unit_type == NAL_NON_IDR &&
               all_slices_I(this_access_unit))
      {
        keep = TRUE;
        if (verbose)
          print_msg("++ KEEP: all slices I\n");
      }
      else
      {
        keep = FALSE;
        if (verbose)
          print_msg("++ DROP: not IDR or all slices I\n");
      }
    }

    if (keep)
    {
      *frame = this_access_unit;
      return 0;
    }
    // We've no further use for this access unit
    free_access_unit(&this_access_unit);
  }
}

/*
 * Retrieve the next frame from the H.264 (MPEG-4/AVC) ES, aiming
 * for an "apparent" kept frequency as stated.
 *
 * Note that the ES data being read should be video-only.
 *
 * - `fcontext` is the information that tells us what to filter and how
 *   (including the desired frequency)
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * - `frame` is the next frame to output.
 *
 *   If the function succeeds and `frame` is NULL, it means that the
 *   last frame should be output again.
 *
 *   Note that it is the caller's responsibility to free this frame with
 *   `free_access_unit()`.
 *
 *   If an error or EOF is returned, this value is undefined.
 *
 * - `frames_seen` is the number of frames found by this call of the function,
 *   including the frame returned.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int get_next_filtered_h264_frame(h264_filter_context_p  fcontext,
                                        int                    verbose,
                                        int                    quiet,
                                        access_unit_p         *frame,
                                        int                   *frames_seen)
{
  int err = 0;
  int keep = FALSE;  // Should we keep the current access unit?
  access_unit_p  this_access_unit = NULL;

  *frames_seen = 0;
  
  for (;;)
  {
    if (es_command_changed(fcontext->access_unit_context->nac->es))
      return COMMAND_RETURN_CODE;

    if (verbose)
      print_msg("\n");

    err = get_next_h264_frame(fcontext->access_unit_context,quiet,verbose,
                              &this_access_unit);
    if (err == EOF)
      return err;
    else if (err)
      return 1;

    fcontext->count ++;
    (*frames_seen) ++;

    fcontext->frames_seen ++;
    
    if (this_access_unit->primary_start == NULL)
    {
      // We don't have a primary picture - no VCL NAL
      // There seems little point in keeping the access unit
      keep = FALSE;
      if (verbose)
        fprint_msg("++ %d/%d DROP: no primary picture\n",
                   fcontext->count,fcontext->freq);
    }
    else if (this_access_unit->primary_start->nal_ref_idc == 0)
    {
      // This is not a reference frame, so it's of no interest
      keep = FALSE;
      if (verbose)
        fprint_msg("++ %d/%d DROP: not a reference frame\n",
                   fcontext->count,fcontext->freq);
    }
    else if (this_access_unit->primary_start->nal_unit_type == NAL_IDR &&
             fcontext->last_accepted_was_not_IDR)
    {
      // This frame is an IDR, and the last frame kept was not, so
      // we'll output it regardless - we don't expect to get enough
      // IDR pictures that this will be a problem, and they're
      // valuable because they're the "limit" for other frames that
      // refer backwards
      // (should we reset the count to zero? - seems sensible)
      keep = TRUE;
      fcontext->not_had_IDR = FALSE;
      fcontext->skipped_ref_pic = FALSE;
      fcontext->last_accepted_was_not_IDR = FALSE;
      if (verbose)
        fprint_msg("++ %d/%d KEEP: IDR and last was not\n",
                   fcontext->count,fcontext->freq);
    }
    else if (this_access_unit->primary_start->nal_unit_type == NAL_IDR &&
             fcontext->not_had_IDR)
    {
      // We haven't had an IDR yet in this filter run, so we had better
      // output this one as a "good start"
      keep = TRUE;
      fcontext->skipped_ref_pic = FALSE;
      fcontext->last_accepted_was_not_IDR = FALSE;
      if (verbose)
        fprint_msg("++ %d/%d KEEP: IDR and first IDR of filter run\n",
                   fcontext->count,fcontext->freq);
    }
    else if (fcontext->count < fcontext->freq)
    {
      // It's too soon, so ignore it - but notice that we *have*
      // ignored a reference picture
      keep = FALSE;
      fcontext->skipped_ref_pic = TRUE;
      if (verbose)
        fprint_msg("++ %d/%d DROP: Too soon (skipping ref frame)\n",
                   fcontext->count,fcontext->freq);
    }
    else if (this_access_unit->primary_start->nal_unit_type == NAL_IDR)
    {
      // It's an IDR, so output it
      keep = TRUE;
      fcontext->skipped_ref_pic = FALSE;
      fcontext->last_accepted_was_not_IDR = FALSE;
      if (verbose)
        fprint_msg("++ %d/%d KEEP: IDR\n",fcontext->count,fcontext->freq);
    }
    else if (all_slices_I(this_access_unit))
    {
      // It is an I picture (either it has all of its slices
      // type "I", or it has a single slice which is of type "I")
      keep = TRUE;
      fcontext->last_accepted_was_not_IDR = TRUE;
      if (verbose)
        fprint_msg("++ %d/%d KEEP: I frame\n",fcontext->count,fcontext->freq);
    }
    else if (!fcontext->skipped_ref_pic && all_slices_I_or_P(this_access_unit))
    {
      // It is a P or I&P picture, but we know that we have output all
      // the reference pictures since the last IDR, so it is
      // safe to output it
      keep = TRUE;
      fcontext->last_accepted_was_not_IDR = TRUE;
      if (verbose)
        fprint_msg("++ %d/%d KEEP: P frame. no skipped ref frames\n",
                   fcontext->count,fcontext->freq);
    }
    else
    {
      keep = FALSE;
      fcontext->skipped_ref_pic = TRUE;
      if (verbose)
        fprint_msg("++ %d/%d DROP: ref frame skipped earlier\n",
                   fcontext->count,fcontext->freq);
    }

    if (keep)
    {
      *frame = this_access_unit;
      fcontext->had_previous_access_unit = TRUE;
      fcontext->frames_written ++;
      fcontext->count = 0;
      return 0;
    }
    else
    {
      if (fcontext->freq > 0)
      {
        int access_units_wanted = fcontext->frames_seen / fcontext->freq;
        int repeat = access_units_wanted - fcontext->frames_written;
        if (repeat > 0 && fcontext->had_previous_access_unit)
        {
          if (verbose) print_msg(">>> output last access unit again\n");
          free_access_unit(&this_access_unit);
          *frame = NULL;
          fcontext->frames_written ++;
          return 0;
        }
      }
      // We've no further use for this access unit
      free_access_unit(&this_access_unit);
    }
  }
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

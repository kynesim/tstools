/*
 * Support for reversing
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

 /* The reverse-data arrays are populated as a side-effect of processing the
 * file forwards. The "build a picture" routines in h262.c and accessunit.c
 * call the "remember" functions herein for appropriate pictures.
 *
 * Reversing is then handled by going backwards through those arrays, selecting
 * appropriate "rememebered" values. For H.264 pictures, and for H.262 sequence
 * headers, it is sufficient to read the appropriate "chunk" of memory back
 * in (from the ES data stream), and just output that without any further work.
 *
 * For H.262 pictures, life is a little more complex, as some of the frames
 * contain AFD user data, which overrides the aspect ratio in the preceding
 * sequence header. If one thus wants to precede each frame *with* a sequence
 * header (which is a Good Idea), then one also needs to ensure it has an
 * appropriate AFD entry (whether it did in the original data or not).
 *
 * Thus for H.262 pictures, one must re-read the picture data from the input
 * file (starting at the specified offset), and "invent" an AFD entry within
 * its item list, if necessary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <time.h>   // for ctime/time

#include "compat.h"
#include "misc_defns.h"
#include "printing_fns.h"
#include "es_fns.h"
#include "h262_fns.h"
#include "nalunit_fns.h"
#include "accessunit_fns.h"
#include "ts_fns.h"
#include "tswrite_fns.h"
#include "reverse_fns.h"

#define DEBUG 0

// ------------------------------------------------------------
// A useful macro to tell us if the `idx` entry in the reverse_data
// structure `rev` is a sequence header or not (or did you guess?)
#define SEQUENCE_HEADER_ENTRY(rev,idx)  (!(rev)->is_h264 && \
                                          (rev)->seq_offset[idx] == 0)


// ============================================================
// Remembering start/length information for reversing video sequences
// ============================================================

/*
 * Build the internal arrays to remember video sequence bounds in,
 * for reversing.
 *
 * Builds a new `reverse_data` datastructure. If `is_h264` is FALSE (i.e., the
 * data to be reversed is not MPEG-1 or MPEG-2), then this datastructure may
 * be smaller.
 *
 * To collect reversing data, attach this datastructure to an H.262 or access
 * unit context (with add_h262/access_unit_reverse_context), and then use
 * get_next_h262_frame() or get_next_h264_frame() to read through the data
 * stream - appropriate pictures/access units will be remembered
 * automatically.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_reverse_data(reverse_data_p *reverse_data,
                              int             is_h264)
{
  int newsize = REVERSE_ARRAY_START_SIZE;
  reverse_data_p  new = malloc(SIZEOF_REVERSE_DATA);
  if (new == NULL)
  {
    print_err("### Unable to allocate reverse data datastructure\n");
    return 1;
  }

  new->start_file = malloc(newsize*sizeof(offset_t));
  if (new->start_file == NULL)
  {
    print_err("### Unable to allocate reverse data array (start_file)\n");
    free(new);
    return 1;
  }

  new->start_pkt = malloc(newsize*sizeof(int32_t));
  if (new->start_pkt == NULL)
  {
    print_err("### Unable to allocate reverse data array (start_pkt)\n");
    free(new->start_file);
    free(new);
    return 1;
  }

  new->index = malloc(newsize*sizeof(uint32_t));
  if (new->index == NULL)
  {
    print_err("### Unable to allocate reverse data array (index)\n");
    free(new->start_file);
    free(new->start_pkt);
    free(new);
    return 1;
  }
  new->data_len = malloc(newsize*sizeof(int32_t));
  if (new->data_len == NULL)
  {
    print_err("### Unable to allocate reverse data array (data_len)\n");
    free(new->start_file);
    free(new->start_pkt);
    free(new->index);
    free(new);
    return 1;
  }

  if (is_h264)
  {
    new->seq_offset = NULL;
    new->afd_byte = NULL;
  }
  else
  {
    new->seq_offset = malloc(newsize);
    if (new->seq_offset == NULL)
    {
      print_err("### Unable to allocate reverse data array (seq offset)\n");
      free(new->start_file);
      free(new->start_pkt);
      free(new->index);
      free(new->data_len);
      free(new);
      return 1;
    }
    new->afd_byte = malloc(newsize);
    if (new->afd_byte == NULL)
    {
      print_err("### Unable to allocate reverse data array (AFD)\n");
      free(new->seq_offset);
      free(new->start_file);
      free(new->start_pkt);
      free(new->index);
      free(new->data_len);
      free(new);
      return 1;
    }
  }
  new->size = newsize;
  new->length = 0;
  new->num_pictures = 0;

  new->is_h264 = is_h264;
  new->pictures_written = 0;
  new->pictures_kept = 0;
  new->first_written = 0;
  new->last_written = 0;
  new->last_posn_added = 0;  // although undefined if `length` == 0
  new->output_sequence_headers = !is_h264;

  new->pid = DEFAULT_VIDEO_PID;
  new->stream_id = DEFAULT_VIDEO_STREAM_ID;
  
  *reverse_data = new;
  return 0;
}

/*
 * Set the video PID and stream id for TS output.
 *
 * This need only be called if reverse data *is* being output as TS,
 * and if the standard default values (DEFAULT_VIDEO_PID and
 * DEFAULT_VIDEO_STREAM_ID) are not correct.
 */
extern void set_reverse_pid(reverse_data_p  reverse_data,
                            uint32_t        pid,
                            byte            stream_id)
{
  reverse_data->pid = pid;
  reverse_data->stream_id = stream_id;
}

/*
 * Add a reversing context to an H.262 context (and vice versa).
 *
 * Does not check if there is one present already.
 *
 * Returns 0 if all is well, 1 if something goes wrong.
 */
extern int add_h262_reverse_context(h262_context_p  h262,
                                    reverse_data_p  reverse_data)
{
  if (reverse_data->is_h264)
  {
    print_err("### Cannot add an H.262 context to an H.264 reverse data"
              " context\n");
    return 1;
  }
  h262->reverse_data = reverse_data;
  reverse_data->h262 = h262;
  return 0;
}

/*
 * Add a reversing context to an access unit context (and vice versa).
 *
 * Does not check if there is one present already.
 *
 * Returns 0 if all is well, 1 if something goes wrong.
 */
extern int add_access_unit_reverse_context(access_unit_context_p  context,
                                           reverse_data_p reverse_data)
{
  if (!reverse_data->is_h264)
  {
    print_err("### Cannot add an H.264 access unit context to an"
              " H.262 reverse data context\n");
    return 1;
  }
  context->reverse_data = reverse_data;
  reverse_data->h264 = context;
  return 0;
}

/*
 * Free the datastructure we used to remember reversing data
 *
 * Sets `reverse_data` to NULL.
 */
extern void free_reverse_data(reverse_data_p  *reverse_data)
{
  reverse_data_p  this = *reverse_data;

  if (this == NULL)
    return;

  if (this->seq_offset != NULL)
  {
    free(this->seq_offset);
    this->seq_offset = NULL;
  }
  free(this->index);
  free(this->start_file);
  free(this->start_pkt);
  free(this->data_len);
  this->index = NULL;
  this->start_file = NULL;
  this->start_pkt = NULL;
  this->data_len = NULL;
  this->length = this->size = 0;
  free(this);
  *reverse_data = NULL;
}

/*
 * Compare an offset and two position components. `offset2` is composed
 * of file_posn2 and pkt_posn2.
 *
 * Returns -1 if offset1 < offset2, 0 if they are the same, and 1 if
 * offset1 > offset2.
 */
static inline int cmp_offsets(ES_offset  offset1,
                              offset_t   file_posn2,
                              int32_t    pkt_posn2)
{
  if (offset1.infile < file_posn2)
    return -1;
  else if (offset1.infile > file_posn2)
    return 1;
  else if (offset1.inpacket < pkt_posn2)
    return -1;
  else if (offset1.inpacket > pkt_posn2)
    return 1;
  else
    return 0;
}

static void debug_reverse_data_problem(reverse_data_p    reverse_data,
                                       uint32_t          index,
                                       ES_offset         start_posn,
                                       uint32_t          idx)
{
  FILE  *tempfile;
  char  *tempfilename = "tsserve_reverse_problem.txt";
  int    ii;

  tempfile = fopen(tempfilename,"a+");
  if (tempfile == NULL)
  {
    fprint_err("### Unable to open file %s - writing diagnostics"
               " to stderr instead\n",tempfilename);
    tempfile = stderr;
  }
  else
  {
    time_t  now;
    fprint_err("### Appending diagnostics to file %s\n",tempfilename);
    now = time(NULL);
    fprintf(tempfile,"** %s:\n",ctime(&now));
  }

  fprintf(tempfile,"Trying to add reverse data [%d] " OFFSET_T_FORMAT
          "/%d at index %d (again),\nbut previous entry was [%d] "
          OFFSET_T_FORMAT "/%d\n",
          index,start_posn.infile,start_posn.inpacket,idx,
          reverse_data->index[idx],reverse_data->start_file[idx],
          reverse_data->start_pkt[idx]);
  fprintf(tempfile,"Last posn added %d, length %d, index %d\n",
          reverse_data->last_posn_added,reverse_data->length,index);
  for (ii=0; ii<reverse_data->length; ii++)
    if (reverse_data->is_h264 || reverse_data->seq_offset[ii])
      fprintf(tempfile,"   %3d: %4d at " OFFSET_T_FORMAT "/%d for %d\n",
              ii,reverse_data->index[ii],
              reverse_data->start_file[ii],
              reverse_data->start_pkt[ii],
              reverse_data->data_len[ii]);
    else
      fprintf(tempfile,"   %3d: seqh at " OFFSET_T_FORMAT "/%d for %d\n",
              ii,
              reverse_data->start_file[ii],
              reverse_data->start_pkt[ii],
              reverse_data->data_len[ii]);
  if (tempfile != stderr)
  {
    fprintf(tempfile,"\n\n");
    fclose(tempfile);
  }
}

/*
 * Remember video sequence bounds for H.262 data
 *
 * - `reverse_data` is the datastructure we want to add our entry to
 * - `index` indicates which picture (counted from the start of the file)
 *   this one is (i.e., we're assuming that not all pictures will be stored).
 *   If the entry is an H.262 sequence header, then this is ignored.
 * - `start_posn` is the location of the start of the entry in the file,
 *   The entry will be ignored if `start_posn` comes before the last
 *   existing entry in the arrays.
 * - `length` is the number of bytes in the entry
 * - `seq_offset` should be 0 for a sequence header, and is otherwise the
 *    offset backwards to the previous nearest sequence header (i.e., 1 if
 *    the sequence header is the previous entry).
 * - `afd` is the effective AFD byte for this picture
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int remember_reverse_h262_data(reverse_data_p    reverse_data,
                                      uint32_t          index,
                                      ES_offset         start_posn,
                                      uint32_t          length,
                                      byte              seq_offset,
                                      byte              afd)
{
  if (reverse_data->length > 0 &&
      (reverse_data->last_posn_added + 1) < (uint32_t)reverse_data->length)
  {
    // We're repeating an entry we previously added - check it hasn't
    // changed (since the only obvious way for this to have happened
    // is if we've rewound and are then moving forwards again, it should
    // not be possible for the data to have changed at a particular index)
    int idx = reverse_data->last_posn_added + 1;
    int cmp = cmp_offsets(start_posn,
                          reverse_data->start_file[idx],
                          reverse_data->start_pkt[idx]);
    if (cmp == 0)
    {
#if DEBUG
      fprint_msg("++ Added [%d] " OFFSET_T_FORMAT "/%d again\n",
                 index,start_posn.infile,start_posn.inpacket);
#endif
      reverse_data->last_posn_added ++;
      return 0;
    }
    else
    {
      fprint_err("### Trying to add reverse data [%d] " OFFSET_T_FORMAT
                 "/%d at index %d (again),\n    but previous entry was [%d] "
                 OFFSET_T_FORMAT "/%d\n",
                 index,start_posn.infile,start_posn.inpacket,idx,
                 reverse_data->index[idx],reverse_data->start_file[idx],
                 reverse_data->start_pkt[idx]);
      debug_reverse_data_problem(reverse_data,index,start_posn,idx);
      return 1;
    }
  }
  
  if (reverse_data->size == reverse_data->length)
  {
    int newsize = reverse_data->size + REVERSE_ARRAY_INCREMENT_SIZE;
    reverse_data->index = realloc(reverse_data->index,
                                  newsize*sizeof(uint32_t));
    if (reverse_data->index == NULL)
    {
      print_err("### Unable to extend reverse data array (index)\n");
      return 1;
    }
    reverse_data->start_file = realloc(reverse_data->start_file,
                                       newsize*sizeof(offset_t));
    if (reverse_data->start_file == NULL)
    {
      print_err("### Unable to extend reverse data array (start_file)\n");
      return 1;
    }
    reverse_data->start_pkt = realloc(reverse_data->start_pkt,
                                      newsize*sizeof(int32_t));
    if (reverse_data->start_pkt == NULL)
    {
      print_err("### Unable to extend reverse data array (start_pkt)\n");
      return 1;
    }
    reverse_data->data_len = realloc(reverse_data->data_len,
                                     newsize*sizeof(int32_t));
    if (reverse_data->data_len == NULL)
    {
      print_err("### Unable to extend reverse data array (length)\n");
      return 1;
    }

    if (!reverse_data->is_h264)
    {
      reverse_data->seq_offset = realloc(reverse_data->seq_offset,newsize);
      if (reverse_data->seq_offset == NULL)
      {
        print_err("### Unable to extend reverse data array (seq offset)\n");
        return 1;
      }
      reverse_data->afd_byte = realloc(reverse_data->afd_byte,newsize);
      if (reverse_data->afd_byte == NULL)
      {
        print_err("### Unable to extend reverse data array (AFD)\n");
        return 1;
      }
    }
    reverse_data->size = newsize;
  }

  // If we're not an H.262 sequence header, remember our index
  if (seq_offset != 0)
  {
    reverse_data->num_pictures ++;
    reverse_data->index[reverse_data->length] = index;
    reverse_data->seq_offset[reverse_data->length] = seq_offset;
    reverse_data->afd_byte[reverse_data->length] = afd;
  }
  else
  {
    reverse_data->index[reverse_data->length] = 0;
    reverse_data->seq_offset[reverse_data->length] = 0;
    reverse_data->afd_byte[reverse_data->length] = 0;
  }

  reverse_data->start_file[reverse_data->length] = start_posn.infile;
  reverse_data->start_pkt[reverse_data->length] = start_posn.inpacket;
  reverse_data->data_len[reverse_data->length] = length;

  reverse_data->last_posn_added = reverse_data->length;
  reverse_data->length ++;
  return 0;
}

/*
 * Remember video sequence bounds for H.264 data
 *
 * - `reverse_data` is the datastructure we want to add our entry to
 * - `index` indicates which picture (counted from the start of the file)
 *   this one is (i.e., we're assuming that not all pictures will be stored).
 *   If the entry is an H.262 sequence header, then this is ignored.
 * - `start_posn` is the location of the start of the entry in the file,
 *   The entry will be ignored if `start_posn` comes before the last
 *   existing entry in the arrays.
 * - `length` is the number of bytes in the entry
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int remember_reverse_h264_data(reverse_data_p    reverse_data,
                                      uint32_t          index,
                                      ES_offset         start_posn,
                                      uint32_t          length)
{
  if (reverse_data->length > 0 &&
      (reverse_data->last_posn_added + 1) < (uint32_t)reverse_data->length)
  {
    // We're repeating an entry we previously added - check it hasn't
    // changed (since the only obvious way for this to have happened
    // is if we've rewound and are then moving forwards again, it should
    // not be possible for the data to have changed at a particular index)
    int idx = reverse_data->last_posn_added + 1;
    int cmp = cmp_offsets(start_posn,
                          reverse_data->start_file[idx],
                          reverse_data->start_pkt[idx]);
    if (cmp == 0)
    {
#if DEBUG
      fprint_msg("++ Added [%d] " OFFSET_T_FORMAT "/%d again\n",
                 index,start_posn.infile,start_posn.inpacket);
#endif
      reverse_data->last_posn_added ++;
      return 0;
    }
    else
    {
      fprint_err("### Trying to add reverse data [%d] " OFFSET_T_FORMAT
                 "/%d at index %d (again),\n    but previous entry was [%d] "
                 OFFSET_T_FORMAT "/%d\n",
                 index,start_posn.infile,start_posn.inpacket,idx,
                 reverse_data->index[idx],reverse_data->start_file[idx],
                 reverse_data->start_pkt[idx]);
      debug_reverse_data_problem(reverse_data,index,start_posn,idx);
      return 1;
    }
  }
  
  if (reverse_data->size == reverse_data->length)
  {
    int newsize = reverse_data->size + REVERSE_ARRAY_INCREMENT_SIZE;
    reverse_data->index = realloc(reverse_data->index,
                                  newsize*sizeof(uint32_t));
    if (reverse_data->index == NULL)
    {
      print_err("### Unable to extend reverse data array (index)\n");
      return 1;
    }
    reverse_data->start_file = realloc(reverse_data->start_file,
                                       newsize*sizeof(offset_t));
    if (reverse_data->start_file == NULL)
    {
      print_err("### Unable to extend reverse data array (start_file)\n");
      return 1;
    }
    reverse_data->start_pkt = realloc(reverse_data->start_pkt,
                                      newsize*sizeof(int32_t));
    if (reverse_data->start_pkt == NULL)
    {
      print_err("### Unable to extend reverse data array (start_pkt)\n");
      return 1;
    }
    reverse_data->data_len = realloc(reverse_data->data_len,
                                     newsize*sizeof(int32_t));
    if (reverse_data->data_len == NULL)
    {
      print_err("### Unable to extend reverse data array (length)\n");
      return 1;
    }
    reverse_data->size = newsize;
  }

  reverse_data->num_pictures ++;
  reverse_data->index[reverse_data->length] = index;
  reverse_data->start_file[reverse_data->length] = start_posn.infile;
  reverse_data->start_pkt[reverse_data->length] = start_posn.inpacket;
  reverse_data->data_len[reverse_data->length] = length;

  reverse_data->last_posn_added = reverse_data->length;
  reverse_data->length ++;
  return 0;
}

/*
 * Retrieve video sequence bounds for entry `which`
 *
 * - `reverse_data` is the datastructure we want to get our entry from
 * - `which` indicates which entry we'd like to retrieve. The first
 *   entry in the `reverse_data` is number 0.
 * - `index` indicates which picture (counted from the start of the file)
 *   this one is (i.e., we're assuming that not all pictures will be stored).
 *   `index` may be passed as NULL if the value is of no interest - i.e.,
 *   typically when the entry is for an H.262 sequence header.
 *   The first picture in the file has index 1.
 * - `start_posn` is the location of the start of the entry in the file,
 * - `length` is the number of bytes in the entry
 * - for H.262 data, if the entry is a picture, then `seq_offset` will
 *   be the offset backwards to the previous nearest sequence header
 *   (i.e., 1 if the sequence header is the previous entry), and if it is
 *   a sequence header, `seq_offset` will be 0. For H.264 data, the value
 *   will always be 0. `seq_offset` may be passed as NULL if the value is
 *   of no interest.
 * - for H.262 data, if the entry is a picture, then `afd` will be its
 *   (effective) AFD byte. Otherwise it will be 0. `afd` may be passed as NULL
 *   if the value if of no interest.
 *
 * To clarify, all of the following are legitimate calls::
 *
 *    err = get_reverse_data(reverse_data,10,&index,&start,&length,&offset,&afd);
 *    err = get_reverse_data(reverse_data,10,&index,&start,&length,NULL,NULL);
 *    err = get_reverse_data(reverse_data,10,NULL,&start,&length,NULL,NULL);
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int get_reverse_data(reverse_data_p    reverse_data,
                            int               which,
                            uint32_t         *index,
                            ES_offset        *start_posn,
                            uint32_t         *length,
                            byte             *seq_offset,
                            byte             *afd)
{
  if (which >= reverse_data->length || which < 0)
  {
    fprint_err("Requested reverse data index (%d) is out of range 0-%d\n",
               which,reverse_data->length-1);
    return 1;
  }

  if (index != NULL)
    *index = reverse_data->index[which];
  start_posn->infile = reverse_data->start_file[which];
  start_posn->inpacket = reverse_data->start_pkt[which];
  *length = reverse_data->data_len[which];
  if (seq_offset != NULL)
  {
    if (reverse_data->is_h264)
      *seq_offset = 0;
    else
      *seq_offset = reverse_data->seq_offset[which];
  }
  if (afd != NULL)
  {
    if (reverse_data->is_h264)
      *afd = 0;
    else
      *afd = reverse_data->afd_byte[which];
  }
  return 0;
}

// ============================================================
// Collecting pictures
// ============================================================
/*
 * Locate and remember sequence headers and I pictures, for later reversal.
 *
 * - `h262` is the H.262 stream reading context
 * - if `max` is non-zero, then collecting will stop after `max` pictures
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * and 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int collect_reverse_h262(h262_context_p h262,
                                int            max,
                                int            verbose,
                                int            quiet)
{
  int err = 0;
  // In order to stop after `max` items, we need to count pictures
  int  picture_count = 0;

  if (h262->reverse_data == NULL)
  {
    print_err("### Unable to collect reverse data for H.262 pictures\n");
    print_err("    H.262 context does not have reverse data"
              " information attached to it\n");
    return 1;
  }
  
  for (;;)
  {
    h262_picture_p  picture = NULL;

    if (es_command_changed(h262->es))
      return COMMAND_RETURN_CODE;
    
    err = get_next_h262_frame(h262,verbose,quiet,&picture);
    if (err == EOF)
      return EOF;
    else if (err)
      return 1;

    if (picture->is_picture)
      picture_count ++;

    free_h262_picture(&picture);
    if (max > 0 && picture_count >= max)
      break;
  }
  return 0;
}

/*
 * Find IDR and I slices, and remember their access units for later output
 * in reverse order.
 *
 * - `acontext` is the access unit reading context
 * - if `max` is non-zero, then collecting will stop after `max` access units
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * and 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int collect_reverse_access_units(access_unit_context_p  acontext,
                                        int             max,
                                        int             verbose,
                                        int             quiet)
{
  int err = 0;
  int access_unit_count = 0;

  if (acontext->reverse_data == NULL)
  {
    print_err("### Unable to collect reverse data for access units\n");
    print_err("    Access unit context does not have reverse data"
              " information attached to it\n");
    return 1;
  }

  for (;;)
  {
    access_unit_p  access_unit;

    if (es_command_changed(acontext->nac->es))
      return COMMAND_RETURN_CODE;

    if (verbose)
      print_msg("\n");

    err = get_next_h264_frame(acontext,quiet,verbose,&access_unit);
    if (err == EOF)
      return EOF;
    else if (err)
      return 1;

    access_unit_count ++;

    free_access_unit(&access_unit);

    if (!verbose && !quiet && (access_unit_count % 5000 == 0))
      fprint_msg("Scanned %d NAL units in %d frames,"
                 " remembered %d frames\n",
                 acontext->nac->count,access_unit_count,
                 acontext->reverse_data->length);

    // Did the logical stream end after the last access unit?
    if (acontext->end_of_stream)
    {
      if (!quiet) print_msg("Found End-of-stream NAL unit\n");
      break;
    }

    if (max > 0 && access_unit_count >= max)
    {
      if (verbose)
        fprint_msg("\nStopping because %d frames have been read\n",
                   access_unit_count);
      break;
    }
  }
  return 0;
}

/*
 * Write out packet data as ES or TS
 *
 * ``extern`` (but unadvertised in a header file) so that it can be used
 * internally in esreverse.c
 *
 * Note that the last two arguments (`pid` and `stream_id`) are only
 * used if the data `is_TS`.
 */
extern int write_packet_data(WRITER   output,
                             int      as_TS,
                             byte     data[],
                             int      data_len,
                             uint32_t pid,
                             byte     stream_id)
{
  int  err;
  if (as_TS)
  {
    // If we're writing TS data, then wrap the whole thing up as PES and
    // write it out as TS packets.
    // Unfortunately, it's not *quite* that simple, as a PES packet has
    // a length specified either as a 16 bit quantity, or as 0 (allowed
    // for video data). And it is known that some pictures are longer
    // than 65535 bytes.
    err = write_ES_as_TS_PES_packet(output.ts_output,data,data_len,
                                    pid,stream_id);
    if (err)
    {
      print_err("### Error writing data as TS PES packet\n");
      return 1;
    }
  }
  else
  {
    // Otherwise, just write it out as is
    size_t written = fwrite(data,1,data_len,output.es_output);
    if (written != data_len)
    {
      fprint_err("### Error writing out data: %s\n"
                 "    Wrote %d bytes instead of %d\n",
                 strerror(errno),(int)written,data_len);
      return 1;
    }
  }
  return 0;
}

/*
 * Write out H.262 picture data as ES or TS
 */
static int write_picture_data(WRITER          output,
                              int             as_TS,
                              h262_picture_p  picture,
                              uint32_t        pid)
{
  int  err;
  if (as_TS)
  {
    err = write_h262_picture_as_TS(output.ts_output,picture,pid);
    if (err)
    {
      print_err("### Error writing data as TS PES packet\n");
      return 1;
    }
  }
  else
  {
    err = write_h262_picture_as_ES(output.es_output,picture);
    if (err)
    {
      print_err("### Error writing data as ES\n");
      return 1;
    }
  }
  return 0;
}

/*
 * Read an H.262 picture from the given location
 *
 * Note that this only does the bare minimum necessary for our purposes
 * here - it is *not* an adequate basis for a general "seek in H.262" mechanism
 * (which is why it is here, rather than in h262.c).
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int read_h262_picture(h262_context_p   context,
                             ES_offset        where,
                             byte             afd,
                             int              verbose,
                             h262_picture_p  *picture)
{
  int  err;
  reverse_data_p reverse_data = NULL;

  // Ensure that the H.262 context doesn't think it has any ES items in hand
  // so that it will start building a picture from scratch
  if (context->last_item)
      free_h262_item(&context->last_item);

  err = seek_ES(context->es,where);
  if (err)
  {
    print_err("### Error seeking for H.262 picture to reverse\n");
    return 1;
  }

  // Hmm - we don't want this call to "remember" the picture for us,
  // since we've already done so. Thus we'll have to pretend that we
  // don't have a reverse data context whilst we're making the call...
  reverse_data = context->reverse_data;
  context->reverse_data = NULL;

  // But we *do* want to insist that the picture contain an AFD
  context->add_fake_afd = TRUE;
  context->last_afd = afd;  // the value to use if the picture doesn't have one

  err = get_next_h262_frame(context,verbose,TRUE,picture);

  context->reverse_data = reverse_data;

  if (err)
  {
    print_err("### Error reading H.262 picture when reversing\n");
    return 1;
  }
  return 0;
}

/*
 * Output an H.262 sequence header.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `as_TS` is true, then output as TS packets, not ES
 * - if `verbose` is true, then extra information will be output
 * - `seq_index` is the index of the sequence header in the `reverse_data`
 * - `reverse_data` contains the list of pictures/access units to reverse.
 */
static int output_sequence_header(ES_p            es,
                                  WRITER          output,
                                  int             as_TS,
                                  int             verbose,
                                  uint16_t        seq_index,
                                  reverse_data_p  reverse_data)
{
  int       err;
  ES_offset seq_posn;
  uint32_t  seq_len;
  byte     *seq_data = NULL;
  err = get_reverse_data(reverse_data,seq_index,NULL,&seq_posn,&seq_len,
                         NULL,NULL);
  if (err)
  {
    fprint_err("### Error retrieving sequence header location at %d\n",
               seq_index);
    return 1;
  }

  if (verbose)
    fprint_msg("Writing sequence header %2d from " OFFSET_T_FORMAT_08
               "/%04d for %5d\n",
               seq_index,seq_posn.infile,seq_posn.inpacket,seq_len);

  err = read_ES_data(es,seq_posn,seq_len,NULL,&seq_data);
  if (err)
  {
    fprint_err("### Error reading (sequence header) data"
               " from " OFFSET_T_FORMAT "/%d for %d\n",
               seq_posn.infile,seq_posn.inpacket,seq_len);
    return 1;
  }
  err = write_packet_data(output,as_TS,seq_data,seq_len,reverse_data->pid,
                          reverse_data->stream_id);
  free(seq_data);
  if (err)
  {
    print_err("### Error writing (sequence header) data as"
              " TS PES packet\n");
    return 1;
  }
  return 0;
}

/*
 * Output the last picture (or an earlier one) from the reverse arrays.
 *
 * This is expected to be used after the whole of the data stream has been
 * played, so that the last picture in the reverse arrays is the last I or
 * IDR picture in the data stream.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `as_TS` is true, then output as TS packets, not ES
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `offset` is the offset from the end of the array of the picture
 *   to output - so 0 means the last picture, 1 the picture before that,
 *   and so on. Sequence headers do not count for this purpose.
 * - `reverse_data` is the reverse data context.
 *
 * Returns 0 if all went well, or 1 if something went wrong.
 */
static int output_from_reverse_data(ES_p            es,
                                    WRITER          output,
                                    int             as_TS,
                                    int             verbose,
                                    int             quiet,
                                    uint32_t        offset,
                                    reverse_data_p  reverse_data)
{
  int       with_sequence_headers = (!reverse_data->is_h264 &&
                                     reverse_data->output_sequence_headers);
  uint32_t  which = reverse_data->length - 1; // the maximum picture index
  int       is_h262 = !reverse_data->is_h264;
  int       err;
  uint32_t  index;
  ES_offset start_posn;
  uint32_t  num_bytes;
  byte      seq_offset;
  byte      afd;
  uint32_t  uu;

  if (verbose)
    fprint_msg("\nGOING BACK: offset %u, max pic index %u\n",offset,which);
  
  // Check we have some data to work with, so we don't try to index
  // nonexistant entries in the arrays
  if (reverse_data->length == 0)
    return 0;

  // Start with the last non-sequence header, and work backwards
  if (which > 0 && SEQUENCE_HEADER_ENTRY(reverse_data,which))
    which --;

  if (verbose)
    fprint_msg("   last non-sequence header picture is %u\n",which);

  for (uu = 0; uu < offset; uu++)
  {
    if (which > 1)
    {
      which --;
      if (SEQUENCE_HEADER_ENTRY(reverse_data,which))
        which --;
    }
    if (verbose)
      fprint_msg("   back %u to %d\n",uu,which);
  }

  // And let's output that picture...
  err = get_reverse_data(reverse_data,which,&index,&start_posn,&num_bytes,
                         &seq_offset,&afd);
  if (err) return 1;

  if (verbose)
    fprint_msg("Picture [%03d] %4d from " OFFSET_T_FORMAT_08
               "/%04d for %5d\n",
               which,index,start_posn.infile,start_posn.inpacket,num_bytes);
    
  if (with_sequence_headers)
  {
    // Make sure we've output its sequence header
    err = output_sequence_header(es,output,as_TS,verbose,(uint16_t)(which - seq_offset),
                                 reverse_data);
    if (err)
    {
      fprint_err("### Error retrieving sequence header"
                 " for picture %d (offset %d)\n",which,seq_offset);
      return 1;
    }
  }

  if (is_h262)
  {
    h262_picture_p  picture;
    err = read_h262_picture(reverse_data->h262,start_posn,afd,verbose,
                            &picture);
    if (err)
    {
      fprint_err("### Error reading H.262 picture from "
                 OFFSET_T_FORMAT "/%d for %d\n",
                 start_posn.infile,start_posn.inpacket,num_bytes);
      return 1;
    }
    err = write_picture_data(output,as_TS,picture,reverse_data->pid);
    if (err)
    {
      print_err("### Error writing picture\n");
      free_h262_picture(&picture);
      return 1;
    }
    free_h262_picture(&picture);
  }
  else
  {
    byte     *data = NULL;
    uint32_t  data_len = 0;
    err = read_ES_data(es,start_posn,num_bytes,&data_len,&data);
    if (err)
    {
      fprint_err("### Error reading data from " OFFSET_T_FORMAT
                 "/%d for %d\n",
                 start_posn.infile,start_posn.inpacket,num_bytes);
      return 1;
    }
    err = write_packet_data(output,as_TS,data,num_bytes,reverse_data->pid,
                            reverse_data->stream_id);
    if (err)
    {
      print_err("### Error writing picture as TS PES packet\n");
      free(data);
      return 1;
    }
    free(data);
  }

  // And let our "outer" contexts know which picture that *is* in the
  // sequence of pictures
  if (reverse_data->is_h264)
    reverse_data->h264->access_unit_index = reverse_data->index[which];
  else
    reverse_data->h262->picture_index = reverse_data->index[which];

  // Remember that we are now that bit further "back" in the reverse data
  // arrays, for when we come to move forwards again
  // (we only do this for pictures that have actually been *read*, since
  // it's only then that our file positions, etc., will have changed.)
  reverse_data->last_posn_added = which;
  return 0;
}

/*
 * Output the last picture (or an earlier one) from the reverse arrays.
 * This version writes the data out as Transport Stream.
 *
 * This is expected to be used after the whole of the data stream has been
 * played, so that the last picture in the reverse arrays is the last I or
 * IDR picture in the data stream.
 *
 * - `es` is the input elementary stream
 * - `tswriter` is the transport stream writer
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `offset` is the offset from the end of the array of the picture
 *   to output - so 0 means the last picture, 1 the picture before that,
 *   and so on. Sequence headers do not count for this purpose.
 * - `reverse_data` is the reverse data context.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int output_from_reverse_data_as_TS(ES_p            es,
                                          TS_writer_p     tswriter,
                                          int             verbose,
                                          int             quiet,
                                          uint32_t        offset,
                                          reverse_data_p  reverse_data)
{
  WRITER  writer;
  writer.ts_output = tswriter;
  return output_from_reverse_data(es,writer,TRUE,verbose,quiet,offset,
                                  reverse_data);
}

/*
 * Output the last picture (or an earlier one) from the reverse arrays.
 * This version writes the data out as Elementary Stream.
 *
 * This is expected to be used after the whole of the data stream has been
 * played, so that the last picture in the reverse arrays is the last I or
 * IDR picture in the data stream.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `offset` is the offset from the end of the array of the picture
 *   to output - so 0 means the last picture, 1 the picture before that,
 *   and so on. Sequence headers do not count for this purpose.
 * - `reverse_data` is the reverse data context.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int output_from_reverse_data_as_ES(ES_p            es,
                                          FILE           *output,
                                          int             verbose,
                                          int             quiet,
                                          uint32_t        offset,
                                          reverse_data_p  reverse_data)
{
  WRITER  writer;
  writer.es_output = output;
  return output_from_reverse_data(es,writer,FALSE,verbose,quiet,offset,
                                  reverse_data);
}

/*
 * Output the H.262 pictures or H.264 access units we remembered earlier - but
 * in reverse order.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `as_TS` is true, then output as TS packets, not ES
 * - if `frequency` is non-zero, then attempt to produce the effect of
 *   keeping every <frequency>th picture (similar to reversing at a
 *   multiplication factor of `frequency`) If 0, just output all the
 *   pictures that were remembered.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `start_with` is the index at which to start outputting from the
 *   reverse data arrays. The value -1 may be used to indicate the most
 *   recent picture in the arrays. If `start_with` is less than -1 then this
 *   function will do nothing. If `start_with` is off the end of the
 *   arrays, then reversing will start from the end of the arrays.
 * - if `max` is non-zero, then output will stop after at least `max`
 *   pictures have been reversed past.
 * - `reverse_data` contains the list of pictures/access units to reverse.
 *
 * Returns 0 if all went well, COMMAND_RETURN_CODE if the current "command"
 * has changed, or 1 if something went wrong.
 */
static int output_in_reverse(ES_p            es,
                             WRITER          output,
                             int             as_TS,
                             int             frequency,
                             int             verbose,
                             int             quiet,
                             int32_t         start_with,
                             int             max,
                             reverse_data_p  reverse_data)
{
  int       ii;
  int       with_sequence_headers = reverse_data->output_sequence_headers;
  byte     *data = NULL;   // picture data, as a "chunk"
  uint32_t  data_len = 0;  // the current size of `data`
  h262_picture_p  picture = NULL;  // H.262 picture data as a "picture"
  uint32_t  last_seq_index = reverse_data->length; // impossible value
  int       max_pic_index = reverse_data->length-1;
  int       first_actual_picture_index = 0;  // the first *actual* picture
  int       is_h262 = !reverse_data->is_h264;

  uint32_t start_index;
  uint32_t final_index;
  uint32_t last_index;

  uint32_t last_num_bytes = 0;  // Number of bytes of last picture written
  
  reverse_data->pictures_written = 0;
  reverse_data->pictures_kept = 0;
  reverse_data->first_written = 0;
  reverse_data->last_written = 0;
  
  // Check we have some data to work with, so we don't try to index
  // nonexistant entries in the arrays
  if (reverse_data->length == 0)
  {
    if (!quiet)
      print_msg("No data to reverse\n");
    return 0;
  }

  // What's the earliest *actual* picture (not a sequence header)?
  while (SEQUENCE_HEADER_ENTRY(reverse_data,first_actual_picture_index))
    first_actual_picture_index ++;
  
  // Where did the user ask us to start?
  if (start_with < -1)
    return 0;
  else if (start_with == -1)
    start_index = reverse_data->last_posn_added;
  else if (start_with > max_pic_index)
    start_index = max_pic_index;
  else
    start_index = start_with;

  // Check that's not a sequence header - if it is, go back one
  while (start_index > 0 && SEQUENCE_HEADER_ENTRY(reverse_data,start_index))
    start_index --;

  // If that means there's nothing to output, then so be it
  if (start_index < (uint32_t)first_actual_picture_index)
    return 0;
  
  // Remember the index of the latest picture we're interested in
  final_index = reverse_data->index[start_index];
  // And the index of the last picture we output
  // - we carefully forge this so that the first (last) picture will be output
  last_index = final_index + frequency;

  reverse_data->first_written = start_index;

  if (verbose)
    fprint_msg("REVERSING: "
               "From index %d (picture %d) down to %d (%d), frequency %d, max %d\n",
               start_index,reverse_data->index[start_index],
               first_actual_picture_index,
               reverse_data->index[first_actual_picture_index],frequency,max);
  
  // If `frequency` is 0, we just want to output all the pictures, backwards.
  // Otherwise, we want to output the first picture we retrieve (i.e., the
  // last picture in the reverse data list), and then (effectively) output
  // a picture every `frequency` pictures. The reverse data `index` value
  // is the index of the picture as a picture of any type (i.e., including
  // the pictures that we didn't bother to remember).
  
  for (ii = start_index; ii >= first_actual_picture_index; ii--)
  {
    int       err;
    int       keep = FALSE;
    uint32_t  index;
    ES_offset start_posn;
    uint32_t  num_bytes;
    byte      seq_offset;
    byte      afd;
    uint32_t  seq_index;

    if (as_TS && tswrite_command_changed(output.ts_output))
    {
      if (data != NULL) free(data);
      if (picture != NULL) free_h262_picture(&picture);
      return COMMAND_RETURN_CODE;
    }
   
    err = get_reverse_data(reverse_data,ii,&index,&start_posn,&num_bytes,
                           &seq_offset,&afd);
    if (err) return 1;

    if (verbose)
      fprint_msg("\nPicture [%03d] %4d from " OFFSET_T_FORMAT_08
                 "/%04d for %5d\n",
                 ii,index,start_posn.infile,start_posn.inpacket,num_bytes);
    
    // Should we write this picture out?
    if (start_posn.infile < 0)
    {
      fprint_err("!!! Start position for reverse item %d does not make sense\n"
                 "    item %d, picture %d, start posn " OFFSET_T_FORMAT
                 "/%d, num bytes %d, seq offset %d\n",ii,ii,index,
                 start_posn.infile,start_posn.inpacket,num_bytes,seq_offset);
      print_err("    Ignoring item\n");
    }
    else if (is_h262 && seq_offset == 0)
    {
      // Sequence headers get output (if at all) when their pictures
      // are written out
      if (verbose) print_msg(".. Sequence header - no need to write\n");
    }
    else if (frequency != 0)
    {
      int  gap = last_index - index;  // gap since last picture output
      if (gap < frequency)
      {
        if (verbose) fprint_msg("++ %d/%d DROP: [%d] %d too soon\n",
                                gap,frequency,ii,index);
      }
      else
      {
        // It's not too soon - but do we need to up our output frequency
        // by repeating the last picture?
        int  pictures_seen   = final_index - index;
        int  pictures_wanted = pictures_seen / frequency;
        int  repeat = pictures_wanted - reverse_data->pictures_written;
        if (verbose)
          fprint_msg("** Pictures seen = %d, wanted = %d, written = %d"
                     " -> repeat = %d\n",pictures_seen,pictures_wanted,
                     reverse_data->pictures_written,repeat);
        if (repeat > 0 && ((is_h262 && picture != NULL) ||
                           (!is_h262 && data != NULL)))
        {
          int jj;
          if (verbose)
          {
            if (repeat == 1)
              print_msg(">> repeating last picture\n");
            else if (repeat > 1)
              fprint_msg(">> repeating last picture %d times\n",repeat);
          }
          for (jj=0; jj<repeat; jj++)
          {
            if (is_h262)
              err = write_picture_data(output,as_TS,picture,reverse_data->pid);
            else
              err = write_packet_data(output,as_TS,data,last_num_bytes,
                                      reverse_data->pid,reverse_data->stream_id);
            if (err)
            {
              print_err("### Error writing (picture) data\n");
              if (data != NULL) free(data);
              if (picture != NULL) free_h262_picture(&picture);
              return 1;
            }
            reverse_data->pictures_written ++;
          }
        }
        keep = TRUE;
        if (verbose) fprint_msg("++ %d/%d KEEP: writing out\n",gap,frequency);
        last_index = index;
      }
    }
    else
      keep = TRUE;  // i.e., because frequency == 0

    // *But* always output the *first* picture, since if we reach it we've
    // "run out" of pictures to present
    if (ii == first_actual_picture_index)
    {
      if (verbose && !keep)
        print_msg("++ but KEEP first picture regardless\n");
      keep = TRUE;
    }

    if (keep)
    {
      if (with_sequence_headers)
      {
        // Make sure we've output its sequence header
        seq_index = ii - seq_offset;
        if (seq_index != last_seq_index)
        {
          err = output_sequence_header(es,output,as_TS,verbose,(uint16_t)seq_index,
                                       reverse_data);
          if (err)
          {
            fprint_err("### Error retrieving sequence header"
                       " for picture %d (offset %d)\n",ii,seq_offset);
            if (data != NULL) free(data);
            if (picture != NULL) free_h262_picture(&picture);
            return 1;
          }
          last_seq_index = seq_index;
        }
      }

      if (verbose)
        fprint_msg("Writing picture [%03d] %4d from " OFFSET_T_FORMAT_08
                   "/%04d for %5d\n",
                   ii,index,start_posn.infile,start_posn.inpacket,num_bytes);

      if (is_h262)
      {
        if (picture != NULL) free_h262_picture(&picture);
        err = read_h262_picture(reverse_data->h262,start_posn,afd,
                                verbose,&picture);
        if (err)
        {
          fprint_err("### Error reading H.262 picture from "
                     OFFSET_T_FORMAT "/%d for %d\n",
                     start_posn.infile,start_posn.inpacket,num_bytes);
          return 1;
        }
        err = write_picture_data(output,as_TS,picture,reverse_data->pid);
        if (err)
        {
          print_err("### Error writing picture\n");
          free_h262_picture(&picture);
          return 1;
        }
      }
      else
      {
        err = read_ES_data(es,start_posn,num_bytes,&data_len,&data);
        if (err)
        {
          fprint_err("### Error reading data from " OFFSET_T_FORMAT
                     "/%d for %d\n",
                     start_posn.infile,start_posn.inpacket,num_bytes);
          if (data != NULL) free(data);
          return 1;
        }
        err = write_packet_data(output,as_TS,data,num_bytes,reverse_data->pid,
                                reverse_data->stream_id);
        if (err)
        {
          print_err("### Error writing picture\n");
          if (data != NULL) free(data);
          return 1;
        }
        last_num_bytes = num_bytes;
      }

      reverse_data->last_written = ii;
      reverse_data->pictures_written ++;
      reverse_data->pictures_kept ++;

      // And let our "outer" contexts know which picture that *is* in the
      // sequence of pictures
      if (reverse_data->is_h264)
        reverse_data->h264->access_unit_index = reverse_data->index[ii];
      else
        reverse_data->h262->picture_index = reverse_data->index[ii];
      // Remember that we are now that bit further "back" in the reverse data
      // arrays, for when we come to move forwards again
      // (we only do this for pictures that have actually been *read*, since
      // it's only then that our file positions, etc., will have changed.)
      reverse_data->last_posn_added = ii;

      if (verbose)
        fprint_msg("Last written [%03d], picture index %d, last_posn_added %d\n",
                   ii,reverse_data->index[ii],ii);
    }
   
    if (max != 0 && (int)(final_index - index + 1) >= max)
    {
      if (verbose)
        fprint_msg("Break: max %d, final_index %d, index %d\n",
                   max,final_index,index);
      break;
    }
  }
  if (data != NULL) free(data);
  if (picture != NULL) free_h262_picture(&picture);

  if (verbose)
    print_msg("END OF REVERSE\n");
  return 0;
}

/*
 * Output the H.262 pictures or H.264 access units we remembered earlier - but
 * in reverse order. This version writes the data out as Transport Stream.
 *
 * - `es` is the input elementary stream
 * - `tswriter` is the transport stream writer
 * - if `frequency` is non-zero, then attempt to produce the effect of
 *   keeping every <frequency>th picture (similar to reversing at a
 *   multiplication factor of `frequency`) If 0, just output all the
 *   pictures that were remembered.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `start_with` is the index at which to start outputting from the
 *   reverse data arrays. The value -1 may be used to indicate the most
 *   recent picture in the arrays. If `start_with` is less than -1 then this
 *   function will do nothing. If `start_with` is off the end of the
 *   arrays, then reversing will start from the end of the arrays.
 * - if `max` is non-zero, then output will stop after at least `max`
 *   pictures have been reversed past.
 * - `reverse_data` contains the list of pictures/access units to reverse.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int output_in_reverse_as_TS(ES_p            es,
                                   TS_writer_p     tswriter,
                                   int             frequency,
                                   int             verbose,
                                   int             quiet,
                                   int32_t         start_with,
                                   int             max,
                                   reverse_data_p  reverse_data)
{
  WRITER  writer;
  writer.ts_output = tswriter;
  return output_in_reverse(es,writer,TRUE,frequency,verbose,quiet,
                           start_with,max,reverse_data);
}

/*
 * Output the H.262 pictures or H.264 access units we remembered earlier - but
 * in reverse order. This version writes the data out as Elementary Stream.
 *
 * - `es` is the input elementary stream
 * - `output` is the stream to write to
 * - if `frequency` is non-zero, then attempt to produce the effect of
 *   keeping every <frequency>th picture (similar to reversing at a
 *   multiplication factor of `frequency`) If 0, just output all the
 *   pictures that were remembered.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `start_with` is the index at which to start outputting from the
 *   reverse data arrays. The value -1 may be used to indicate the most
 *   recent picture in the arrays. If `start_with` is less than -1 then this
 *   function will do nothing. If `start_with` is off the end of the
 *   arrays, then reversing will start from the end of the arrays.
 * - if `max` is non-zero, then output will stop after at least `max`
 *   pictures have been reversed past.
 * - `reverse_data` contains the list of pictures/access units to reverse.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
extern int output_in_reverse_as_ES(ES_p            es,
                                   FILE           *output,
                                   int             frequency,
                                   int             verbose,
                                   int             quiet,
                                   int32_t         start_with,
                                   int             max,
                                   reverse_data_p  reverse_data)
{
  WRITER  writer;
  writer.es_output = output;
  return output_in_reverse(es,writer,FALSE,frequency,verbose,quiet,
                           start_with,max,reverse_data);
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

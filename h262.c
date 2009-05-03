/*
 * Datastructures and prototypes for reading H.262 (MPEG-2) elementary streams.
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
#include "h262_fns.h"
#include "es_fns.h"
#include "ts_fns.h"
#include "reverse_fns.h"
#include "misc_fns.h"

#define DEBUG_GET_NEXT_PICTURE 0
#define DEBUG_AFD 0


/*
 * Print out information derived from the start code
 *
 * Note that if a "SYSTEM START" code is reported, then the data is
 * likely to be PES or Transport Stream data, not Elementary Stream.
 *
 * Similarly, if a "TRANSPORT STREAM sync byte" is reported, then
 * the stream is probably Transport Stream.
 *
 * If the stream is *not* Elementary Stream data, then it is possible
 * that some of the apparent start code prefixes are actually false
 * detections.
 */
extern void print_h262_start_code_str(byte   start_code)
{
  byte  number;
  char *str = NULL;
  switch (start_code)
  {
    // H.262 start codes
  case 0x00: str = "Picture"; break;
  case 0xB0: str = "Reserved"; break;
  case 0xB1: str = "Reserved"; break;
  case 0xB2: str = "User data"; break;
  case 0xB3: str = "SEQUENCE HEADER"; break;
  case 0xB4: str = "Sequence error"; break;
  case 0xB5: str = "Extension start"; break;
  case 0xB6: str = "Reserved"; break;
  case 0xB7: str = "SEQUENCE END"; break;
  case 0xB8: str = "Group start"; break;

    // System start codes - 13818-1 p32 Table 2-18 stream_id
    // If these occur, then we're seeing PES headers
    // - maybe we're looking at transport stream data?
  case 0xBC: str = "SYSTEM START: Program stream map"; break;
  case 0xBD: str = "SYSTEM START: Private stream 1"; break;
  case 0xBE: str = "SYSTEM START: Padding stream"; break;
  case 0xBF: str = "SYSTEM START: Private stream 2"; break;
  case 0xF0: str = "SYSTEM START: ECM stream"; break;
  case 0xF1: str = "SYSTEM START: EMM stream"; break;
  case 0xF2: str = "SYSTEM START: DSMCC stream"; break;
  case 0xF3: str = "SYSTEM START: 13522 stream"; break;
  case 0xF4: str = "SYSTEM START: H.222 A stream"; break;
  case 0xF5: str = "SYSTEM START: H.222 B stream"; break;
  case 0xF6: str = "SYSTEM START: H.222 C stream"; break;
  case 0xF7: str = "SYSTEM START: H.222 D stream"; break;
  case 0xF8: str = "SYSTEM START: H.222 E stream"; break;
  case 0xF9: str = "SYSTEM START: Ancillary stream"; break;
  case 0xFF: str = "SYSTEM START: Program stream directory"; break;
    
  default: str = NULL; break;
  }

  if (str != NULL)
    print_msg(str);
  else if (start_code == 0x47)
    print_msg("TRANSPORT STREAM sync byte");
  else if (start_code >= 0x01 && start_code <= 0xAF)
    fprint_msg("Slice, vertical posn %d",start_code);
  else if (start_code >= 0xC0 && start_code <=0xDF)
  {
    number = start_code & 0x1F;
    fprint_msg("SYSTEM START: Audio stream %02x",number);
  }
  else if (start_code >= 0xE0 && start_code <= 0xEF)
  {
    number = start_code & 0x0F;
    fprint_msg("SYSTEM START: Video stream %x",number);
  }
  else if (start_code >= 0xFC && start_code <= 0xFE)
    print_msg("SYSTEM START: Reserved data stream");
  else
    print_msg("SYSTEM START: Unrecognised stream id");
}

/*
 * Build a new MPEG2 item datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_h262_item(h262_item_p  *item)
{
  int     err;
  h262_item_p  new = malloc(SIZEOF_H262_ITEM);
  if (new == NULL)
  {
    print_err("### Unable to allocate MPEG2 item datastructure\n");
    return 1;
  }
  err = setup_ES_unit(&(new->unit));
  if (err)
  {
    print_err("### Unable to allocate MPEG2 item data buffer\n");
    free(new);
    return 1;
  }
  *item = new;
  return 0;
}

/*
 * Tidy up and free an MPEG2 item datastructure after we've finished with it.
 *
 * Empties the MPEG2 item datastructure, frees it, and sets `item` to NULL.
 *
 * If `item` is already NULL, does nothing.
 */
extern void free_h262_item(h262_item_p  *item)
{
  if (*item == NULL)
    return;
  clear_ES_unit(&(*item)->unit);
  free(*item);
  *item = NULL;
}

/*
 * Print out useful information about this MPEG2 item
 */
extern void report_h262_item(h262_item_p  item)
{
  fprint_msg(OFFSET_T_FORMAT_08 "/%04d: MPEG2 item %02x (",
             item->unit.start_posn.infile,
             item->unit.start_posn.inpacket,item->unit.start_code);
  print_h262_start_code_str(item->unit.start_code);
  print_msg(")");
  if (item->unit.start_code == 0)
    fprint_msg(" %d (%s)",item->picture_coding_type,
               H262_PICTURE_CODING_STR(item->picture_coding_type));
  fprint_msg(" size %d",item->unit.data_len);
  print_msg("\n");
}

// ------------------------------------------------------------
// MPEG2 item *data* stuff
// ------------------------------------------------------------
/*
 * Find and read in the next MPEG2 item.
 *
 * Be careful if using this in conjunction with reading H.262 pictures
 * via an `h262_context_p`, as it does not maintain the "last item read"
 * information therein.
 *
 * - `es` is the elementary stream we're reading from.
 * - `item` is the datastructure containing the MPEG2 item found, or NULL
 *   if there was none.
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read (i.e., there
 * is no next MPEG2 item), otherwise 1 if some error occurs.
 */
extern int find_next_h262_item(ES_p     es,
                               h262_item_p  *item)
{
  int    err;

  err = build_h262_item(item);
  if (err) return 1;

  err = find_next_ES_unit(es,&(*item)->unit);
  if (err) // 1 or EOF
  {
    free_h262_item(item);
    return err;
  }

  // If this is a picture, we can do a little more
  if ((*item)->unit.start_code == 0)
  {
    (*item)->picture_coding_type = ((*item)->unit.data[5] & 0x38) >> 3;
  }
  return 0;
}

/*
 * Build a new H.262 picture reading context.
 *
 * This acts as a "jacket" around the ES context, and is used when reading
 * H.262 pictures with get_next_h262_picture(). It "remembers" the last
 * item read, which is the first item that was not part of the picture.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_h262_context(ES_p            es,
                              h262_context_p *context)
{
  h262_context_p  new = malloc(SIZEOF_H262_CONTEXT);
  if (new == NULL)
  {
    print_err("### Unable to allocate H.262 context datastructure\n");
    return 1;
  }

  new->es = es;
  new->picture_index = 0;
  new->last_item = NULL;
  new->reverse_data = NULL;
  new->count_since_seq_hdr = 0;
  new->last_aspect_ratio_info = H262_UNSET_ASPECT_RATIO_INFO;
  new->last_afd = UNSET_AFD_BYTE;
  new->add_fake_afd = FALSE;

  *context = new;
  return 0;
}

/*
 * Free an H.262 picture reading context.
 *
 * Clears the datastructure, frees it, and returns `context` as NULL.
 *
 * Does not free any `reverse_data` datastructure.
 *
 * Does nothing if `context` is already NULL.
 */
extern void free_h262_context(h262_context_p *context)
{
  h262_context_p  cc = *context;

  if (cc == NULL)
    return;

  if (cc->last_item != NULL)
    free_h262_item(&cc->last_item);

  cc->reverse_data = NULL;
  
  free(*context);
  *context = NULL;
  return;
}

/*
 * Rewind a file being read as H.262 pictures
 *
 * This is a wrapper for `seek_ES` that also knows to unset things appropriate
 * to the H.262 picture reading context.
 *
 * If a reverse context is attached to this context, it also will
 * be "rewound" appropriately.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int rewind_h262_context(h262_context_p  context)
{
  ES_offset  start_of_file = {0,0};
  
  // First, forget where we are
  if (context->last_item)
      free_h262_item(&context->last_item);

  context->picture_index = 0;  // no pictures read from this file yet

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
// MPEG2 "pictures"
// ------------------------------------------------------------
/*
 * Add (the information from) an H.262 item to the given picture.
 *
 * Note that since this takes a copy of the ES unit data from within the item,
 * it is safe to free the original item.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int append_to_h262_picture(h262_picture_p  picture,
                                  h262_item_p     item)
{
  ES_unit_p  unit = &(item->unit);

  if (is_h262_extension_start_item(item))
  {
    byte  *data = unit->data;
    int    extension_start_code_id = (data[4] & 0xF0) >> 4;
    if (extension_start_code_id == 1)  // sequence extension
    {
      picture->progressive_sequence = data[5] & 0x08;
    }
    else if (extension_start_code_id == 8) // picture coding extension
    {
      picture->picture_structure = data[6] & 0x03;
    }
  }
  return append_to_ES_unit_list(picture->list,unit);
}

/*
 * Build a new H.262 "picture", starting with the given item (which is
 * copied, so may be freed after this call).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int build_h262_picture(h262_context_p   context,
                              h262_picture_p  *picture,
                              h262_item_p      item)
{
  int  err;
  ES_unit_p       unit = &(item->unit);
  byte           *data = unit->data;
  h262_picture_p  new  = malloc(SIZEOF_H262_PICTURE);
  if (new == NULL)
  {
    print_err("### Unable to allocate H.262 picture datastructure\n");
    return 1;
  }

  err = build_ES_unit_list(&(new->list));
  if (err)
  {
    print_err("### Unable to allocate internal list for H.262 picture\n");
    free(new);
    return 1;
  }

  // Deduce what we can from the first item of the "picture"
  if (is_h262_picture_item(item))
  {
    new->picture_coding_type = item->picture_coding_type;
    new->is_picture = TRUE;
    new->is_sequence_header = FALSE;
    new->temporal_reference = (data[4] << 2) | ((data[5] & 0xC0) >> 6);
    // Assume that our picture is a frame, until we're told otherwise
    // (MPEG-1 data will never tell us otherwise)
    new->picture_structure = 3;
    new->was_two_fields = FALSE;
    // Assume the last AFD and aspect ratio info until told otherwise
    new->afd = context->last_afd;
    new->aspect_ratio_info = context->last_aspect_ratio_info;
    new->is_real_afd = FALSE;
  }
  else if (is_h262_seq_header_item(item))
  {
    new->is_picture = FALSE;
    new->is_sequence_header = TRUE;
    new->picture_coding_type = 0; // Forbidden value, just in case
    new->aspect_ratio_info = (data[7] & 0xF0) >> 4;
    // Assume that we are only allowed progressive frames, until we're told
    // otherwise (MPEG-1 data will never tell us otherwise)
    new->progressive_sequence = 1;
  }
  else if (is_h262_seq_end_item(item))
  {
    new->is_picture = FALSE;
    new->is_sequence_header = FALSE;
    new->picture_coding_type = 0; // Forbidden value, just in case
  }
  else
  {
    fprint_err("!!! Building H.262 picture that starts with a %s (%02x)\n",
               H262_START_CODE_STR(item->unit.start_code),item->unit.start_code);
    new->is_picture = FALSE;
    new->is_sequence_header = FALSE;
    new->picture_coding_type = 0; // Forbidden value, just in case
  }

  err = append_to_h262_picture(new,item);
  if (err)
  {
    fprint_err("### Error appending first item to H.262 %s\n",
               H262_START_CODE_STR(item->unit.start_code));
    free_h262_picture(&new);
    return 1;
  }

  *picture = new;
  return 0;
}

/*
 * Build a "pretend" H262 item containing an AFD user data field, and
 * append it to the given picture.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int append_fake_afd(h262_picture_p  picture,
                           byte            afd)
{
  int          err;
  static h262_item_p item = NULL;

  if (item == NULL)
  {
    err = build_h262_item(&item);
    if (err)
    {
      print_err("### Error building 'fake' AFD for H.262 picture\n");
      return 1;
    }
    item->unit.data[0] = 0x00;
    item->unit.data[1] = 0x00;
    item->unit.data[2] = 0x01;
    item->unit.data[3] = 0xb2;
    item->unit.data[4] = 0x44;
    item->unit.data[5] = 0x54;
    item->unit.data[6] = 0x47;
    item->unit.data[7] = 0x31;
    item->unit.data[8] = 0x41;
    item->unit.data[9] = afd;
    item->unit.data_len = 10;
    item->unit.start_code = 0xb2;
  }
  else
    item->unit.data[9] = afd;

  // Remember, this *copies* the item, so we can use it again later on
  err = append_to_h262_picture(picture,item);
  if (err)
  {
    print_err("### Error appending 'fake' AFD to H.262 picture\n");
    return 1;
  }

  picture->afd = afd;
  picture->is_real_afd = FALSE;
  return 0;
}

/*
 * Merge two fields into one (frame) picture.
 *
 * - `picture1` is the first field.
 * - `picture2` is the second field, which will be merged into the first
 *   (thus `picture2` may be freed after this function succeeds).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int merge_fields(h262_picture_p  picture1,
                        h262_picture_p  picture2)
{
  int  ii;
  for (ii = 0; ii < picture2->list->length; ii++)
  {
    int err = append_to_ES_unit_list(picture1->list,
                                     &picture2->list->array[ii]);
    if (err)
    {
      print_err("### Error merging two H.262 field pictures\n");
      return 1;
    }
  }
  picture1->was_two_fields = TRUE;
  return 0;
}

/*
 * Free an H.262 "picture".
 *
 * Clears the datastructure, frees it, and returns `picture` as NULL.
 *
 * Does nothing if `picture` is already NULL.
 */
extern void free_h262_picture(h262_picture_p *picture)
{
  h262_picture_p  pic = *picture;

  if (pic == NULL)
    return;

  if (pic->list != NULL)
    free_ES_unit_list(&pic->list);

  free(*picture);
  *picture = NULL;
  return;
}

/*
 * Compare two H.262 pictures. The comparison does not include the start
 * position of the picture, but just the actual data - i.e., two pictures
 * read from different locations in the input stream may be considered the
 * same if their data content is identical.
 *
 * Returns TRUE if the lists contain identical content, FALSE otherwise.
 */
extern int same_h262_picture(h262_picture_p  picture1,
                             h262_picture_p  picture2)
{
  if (picture1 == picture2)
    return TRUE;
  else if (picture1 == NULL || picture2 == NULL)
    return FALSE;
  else
    return same_ES_unit_list(picture1->list,picture2->list);
}

/*
 * Remember a picture for future reversing, if it's an I picture or a
 * sequence header
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int maybe_remember_this_picture(h262_context_p  h262,
                                       int             verbose,
                                       h262_picture_p  this_picture)
{
  int        err;
  ES_offset  start_posn = {0,0};
  uint32_t   num_bytes = 0;
  if (this_picture->is_picture)
  {
    if (this_picture->picture_coding_type == 1)
    {
      // It's an I picture - we want to remember it in our reverse list
      (h262->count_since_seq_hdr) ++;

      err = get_ES_unit_list_bounds(this_picture->list,&start_posn,&num_bytes);
      if (err)
      {
        print_err("### Error working out position/size of H.262 picture\n");
        return 1;
      }
      
      err = remember_reverse_h262_data(h262->reverse_data,h262->picture_index,
                                       start_posn,num_bytes,
                                       h262->count_since_seq_hdr,
                                       this_picture->afd);
      if (err)
      {
        print_err("### Error remembering reversing data for H.262 item\n");
        return 1;
      }
      if (verbose)
        fprint_msg("REMEMBER I picture %5d at " OFFSET_T_FORMAT_08
                   "/%04d for %5d\n",h262->picture_index,
                   start_posn.infile,start_posn.inpacket,num_bytes);
    }
  }
  else if (this_picture->is_sequence_header)
  {
    // It's a sequence header - remember it for the next picture
    h262->count_since_seq_hdr = 0;
    err = get_ES_unit_list_bounds(this_picture->list,&start_posn,&num_bytes);
    if (err)
    {
      print_err("### Error working out position/size of H.262"
                " sequence header for reversing data\n");
      return 1;
    }
    err = remember_reverse_h262_data(h262->reverse_data,0,
                                     start_posn,num_bytes,0,0);
    if (err)
    {
      print_err("### Error remembering reversing data for H.262 item\n");
      return 1;
    }
    if (verbose)
      fprint_msg("REMEMBER Sequence header at " OFFSET_T_FORMAT_08
                 "/%04d for %5d\n",
                 start_posn.infile,start_posn.inpacket,num_bytes);
  }
  return 0;
}

/*
 * Given an MPEG-2 user data item containing an AFD (as indicated by the
 * ``is_h262_AFD_user_data_item`` macro), extract the actual AFD.
 *
 * NB: the whole byte containing the AFD is returned, including the top
 * '1111' bits.
 *
 * Returns 0 if all goes well, 1 if the AFD user data item is malformed
 * (in which case a message will have been written out to ``stderr``, but
 * the "apparent" AFD value will still be returned).
 */
static int extract_AFD(h262_item_p  item,
                       byte        *afd)
{
  if (item->unit.data[8] == 0x41)
  {
    // AFD flag set
    if (item->unit.data_len < 10)
    {
      fprint_err("!!! AFD too short (only %d bytes - AFD missing)\n",
                 item->unit.data_len);
      *afd = UNSET_AFD_BYTE;
      return 1;
    }
    *afd = item->unit.data[9];
    if ((item->unit.data[9] & 0xF0) != 0xF0)
    {
      fprint_err("### Bad AFD %02x (reserved bits not 1111)\n",
                 item->unit.data[9]);
      return 1;
    }
  }
  else if (item->unit.data[8] == 0x01)
  {
    *afd = UNSET_AFD_BYTE;  // no explicit AFD - use the default
  }
  else
  {
    fprint_err("### AFD datastructure malformed: flag byte is %02x"
               " instead of 0x41 or 0x01\n",item->unit.data[8]);
    if (item->unit.data_len == 9)
      *afd = UNSET_AFD_BYTE;
    else
      *afd = item->unit.data[9];
    return 1;
  }
  return 0;
}

#if DEBUG_GET_NEXT_PICTURE
/*
 * Print a representation of an item for debugging
 */
static void _show_item(h262_item_p  item)
{
  print_msg("__ ");
  if (item == NULL)
  {
    print_msg("<no item>\n");
    return;
  }
  if (is_h262_picture_item(item))
    fprint_msg("%s picture",H262_PICTURE_CODING_STR(item->picture_coding_type));
  else if (is_h262_slice_item(item))
    fprint_msg("slice %2x",item->unit.start_code);
  else
    fprint_msg("%s",H262_START_CODE_STR(item->unit.start_code));
  fprint_msg(" at " OFFSET_T_FORMAT "/%d for %d\n",
             item->unit.start_posn.infile,item->unit.start_posn.inpacket,
             item->unit.data_len);
}
#endif

/*
 * Retrieve the the next H.262 "picture".
 *
 * The H.262 "picture" returned can be one of:
 *
 * 1. A field or frame, including its slices.
 * 2. A sequence header, including its sequence extension, if any.
 * 3. A sequence end.
 *
 * - `context` is the H.262 picture reading context.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `picture` is the H.262 "picture", containing a field or frame picture,
 *   a sequence header or a sequence end
 *
 * Returns 0 if it succeeds, EOF if we reach the end of file, or 1 if some
 * error occurs.
 */
extern int get_next_h262_single_picture(h262_context_p  context,
                                        int             verbose,
                                        h262_picture_p *picture)
{
  int  err = 0;

  int  in_sequence_header = FALSE;
  int  in_sequence_end    = FALSE;
  int  in_picture         = FALSE;
  int  last_was_slice     = FALSE;
  int  had_afd            = FALSE;

  h262_item_p  item = context->last_item;

#if DEBUG_GET_NEXT_PICTURE
  int  num_slices = 0;
  int  had_slice = FALSE;
  int  last_slice_start_code = 0;
  if (verbose && context->last_item) print_msg("__ reuse last item\n");
#endif

  context->last_item = NULL;

  // Find the first item of our next "picture"
  for (;;)
  {
    if (item == NULL)
    {
      err = find_next_h262_item(context->es,&item);
      if (err) return err;
    }
    if (is_h262_picture_item(item))
    {
      in_picture = TRUE;
      break;
    }
    else if (is_h262_seq_header_item(item))
    {
      in_sequence_header = TRUE;
      break;
    }
    else if (is_h262_seq_end_item(item))
    {
      in_sequence_end = TRUE;
      break;
    }
#if DEBUG_GET_NEXT_PICTURE
    else if (verbose)
      _show_item(item);
#endif
    free_h262_item(&item);
  }

#if DEBUG_GET_NEXT_PICTURE
  if (verbose)
  {
    print_msg("__ --------------------------------- <start picture>\n");
    _show_item(item);
  }
#endif
  
  err = build_h262_picture(context,picture,item);
  if (err) return 1;

  free_h262_item(&item);

  if (in_sequence_end)
  {
    // A sequence end is a single item, so we're done
#if DEBUG_GET_NEXT_PICTURE
    if (verbose)
      print_msg("__ --------------------------------- <end picture>\n");
#endif
    return 0;
  }
  
  // Now find all the rest of the picture/sequence header
  for (;;)
  {
    err = find_next_h262_item(context->es,&item);
    if (err)
    {
      if (err != EOF)
        free_h262_picture(picture);
      return err;
    }

    if (in_picture)
    {
      // Have we just finished a picture?
      // We know we have if the last item was a slice, but this one isn't
      if (last_was_slice && !is_h262_slice_item(item))
        break;
      last_was_slice = is_h262_slice_item(item);
    }
    else if (in_sequence_header)
    {
      // Have we just finished a sequence header and its friends?
      // We know we have if we've hit something that isn't an
      // extension start or user data start code (perhaps we could
      // get away with just keeping the (in MPEG-2) sequence_extension,
      // but it's safer (and simpler) to keep the lot
      if (!is_h262_extension_start_item(item) &&
          !is_h262_user_data_item(item))
        break;
    }

    if (in_picture)
    {
      if (is_h262_AFD_user_data_item(item))
      {
        // We found a *real* AFD - remember it
        err = extract_AFD(item,&(*picture)->afd);
        if (err)
          fprint_err("!!! Assuming AFD %x at " OFFSET_T_FORMAT "/%d\n",
                     (*picture)->afd,
                     item->unit.start_posn.infile,item->unit.start_posn.inpacket);
        (*picture)->is_real_afd = TRUE;
#if DEBUG_AFD
        if ((*picture)->afd != context->last_afd)
        {
          print_msg("* ");
          report_h262_picture(stdout,*picture,FALSE);
        }
#endif
        context->last_afd = (*picture)->afd;
        had_afd = TRUE;
      }
      else if (context->add_fake_afd && !had_afd && is_h262_slice_item(item))
      {
        // We've been asked to fake AFDs for pictures that don't have them,
        // and this is the first slice of a picture, so now (i.e., before
        // said first slice) is the time to add in that faked AFD
        err = append_fake_afd(*picture,context->last_afd);
        if (err)
        {
          free_h262_picture(picture);
          return 1;
        }
        had_afd = TRUE;  // well, sort of
#if DEBUG_GET_NEXT_PICTURE
        if (verbose)
        {
          print_msg("__ fake AFD ");
          print_bits(4,(*picture)->afd);
          fprint_msg(", i.e., %s",SHORT_AFD_STR((*picture)->afd));
          print_msg("\n");
        }
#endif
      }
    }

#if DEBUG_GET_NEXT_PICTURE
    if (verbose)
    {
      if (!had_slice)
        _show_item(item);
      if (is_h262_slice_item(item))
      {
        num_slices ++;
        last_slice_start_code = item->unit.start_code;
        if (!had_slice)
          had_slice = TRUE;
      }
    }
#endif

    // Don't forget to remember the actual item
    err = append_to_h262_picture(*picture,item);
    if (err)
    {
      print_err("### Error adding item to H.262 sequence header\n");
      free_h262_picture(picture);
      return 1;
    }
    free_h262_item(&item);
  }

  if (in_picture)
    context->picture_index ++;
  else
    context->last_aspect_ratio_info = (*picture)->aspect_ratio_info;

  context->last_item = item;
#if DEBUG_GET_NEXT_PICTURE
  if (verbose)
  {
    if (in_picture)
    {
      if (num_slices > 1)
      {
        ES_unit_p  unit = &(*picture)->list->array[(*picture)->list->length-1];
        print_msg("__ ...\n");
        fprint_msg("__ slice %2x",last_slice_start_code);
        fprint_msg(" at " OFFSET_T_FORMAT "/%d for %d\n",
                   unit->start_posn.infile,unit->start_posn.inpacket,
                   unit->data_len);
      }
      fprint_msg("__ (%2d slices)\n",num_slices);
    }
    print_msg("__ --------------------------------- <end picture>\n");
    if (in_picture || in_sequence_header)
      _show_item(item);
  }
#endif
  return 0;
}

/*
 * Try for the next field of a pair, and return a frame formed thereof
 *
 * - `context` is the H.262 picture reading context.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - if `first_time` is true, then we will try to match a second field
 *   with a third, if the second field has a different temporal reference
 *   than the first. If it is false, we will not (thus stopping us from
 *   trying forever...)
 * - `picture` starts out at the first field of our (hoped for) pair, and
 *   will end up as the merged result of our two fields. If the input stream
 *   is awry (or we are misaligned with respect to it), this might instead be
 *   replaced by a "proper" frame, or even a sequence header.
 *
 * Returns 0 if it succeeds, EOF if we reach the end of file or have
 * read the sequence_end_code, or 1 if some error occurs.
 */
static int get_next_field_of_pair(h262_context_p  context,
                                  int             verbose,
                                  int             quiet,
                                  int             first_time,
                                  h262_picture_p *picture)
{
  int  err;
  h262_picture_p  second;

  if (verbose)
    fprint_msg("@@ Looking for second field (%s time)\n",
               (first_time?"first":"second"));
  
  // We assume (hope) the next picture will be our second half
  err = get_next_h262_single_picture(context,verbose,&second);
  if (err)
  {
    if (err != EOF)
      print_err("### Trying to read second field\n");
    return err;
  }

  if (!is_h262_field_picture(second))
  {
    // But it was either a frame or a sequence header - oh dear
    if (!quiet)
      fprint_err("!!! Field followed by a %s - ignoring the field\n",
                 (second->is_picture?"frame":"sequence header"));
    free_h262_picture(picture);
    *picture = second;
    // and pretend to success
  }
  else if ((*picture)->temporal_reference == second->temporal_reference)
  {
    // They appear to be matching fields - make a frame from them
    if (verbose) print_msg("@@ Merging two fields\n");
    err = merge_fields(*picture,second);
    if (err)
    {
      free_h262_picture(&second);
      return 1;
    }
    free_h262_picture(&second);
  }
  else if (first_time)
  {
    if (!quiet)
      fprint_err("!!! Field with temporal ref %d (%x) followed by"
                 " field with temporal ref %d (%x) - ignoring first field\n",
                 (*picture)->temporal_reference,(*picture)->temporal_reference,
                 second->temporal_reference,second->temporal_reference);

    // Try again
    free_h262_picture(picture);
    *picture = second;
    err = get_next_field_of_pair(context,verbose,quiet,FALSE,picture);
    if (err) return 1;
  }
  else
  {
    print_err("### Adjacent fields do not share temporal references"
              " - unable to match fields up\n");
    return 1;
  }
  return 0;
}

/*
 * Retrieve the the next H.262 "picture".
 *
 * The H.262 "picture" returned can be one of:
 *
 * 1. A frame, including its slices. This may be the concatenation of two
 *    adjacent field pictures.
 * 2. A sequence header, including its sequence extension, if any.
 * 3. A sequence end.
 *
 * Specifically, the next H.262 "picture" is retrieved from the input stream.
 *
 * If that "picture" represents a sequence header or a frame, it is returned.
 *
 * If it represents a field, then the *following* "picture" is retrieved, and
 * if that is the second field of its frame, it is merged into the first,
 * and the resultant frame is returned.
 *
 * If a field with temporal reference A is followed by a field with temporal
 * reference B, it is assumed that synchronisation has been lost. In this
 * case, the first field (frame A) will be discarded, and an attempt made to
 * read the second field of frame B.
 *
 * Similarly, if a frame or sequence header is found instead of the second
 * field, the first field will be discarded and the frame returned.
 *
 *   Note that if the context is associated with a reverse context,
 *   then appropriate frames/sequence headers will automatically be
 *   remembered therein.
 *
 *   Also note that it is assumed that the AFD for adjacent fields will be
 *   the same.
 *
 * - `context` is the H.262 picture reading context.
 * - if `verbose` is true, then extra information will be output
 * - if `quiet` is true, then only errors will be reported
 * - `picture` is the H.262 "picture", containing a frame picture,
 *   a sequence header or a sequence end
 *
 * Returns 0 if it succeeds, EOF if we reach the end of file, or 1 if some
 * error occurs.
 */
extern int get_next_h262_frame(h262_context_p  context,
                               int             verbose,
                               int             quiet,
                               h262_picture_p *picture)
{
  int  err;

  err = get_next_h262_single_picture(context,verbose,picture);
  if (err) return err;

  if (is_h262_field_picture(*picture))
  {
    // We assume (hope) the next picture will be our second half
    // - let's try to get it, and merge it into our current picture
    err = get_next_field_of_pair(context,verbose,quiet,TRUE,picture);
    if (err)
    {
      free_h262_picture(picture);
      return 1;
    }
  }

  if (context->reverse_data)
  {
    err = maybe_remember_this_picture(context,verbose,*picture);
    if (err)
    {
      free_h262_picture(picture);
      return 1;
    }
  }

  return 0;
}

/*
 * Write out an H.262 picture as TS
 *
 * - `tswriter` is TS the output stream
 * - `picture` is the picture to write out
 * - `pid` is the PID to use for the TS packets
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_h262_picture_as_TS(TS_writer_p     tswriter,
                                    h262_picture_p  picture,
                                    uint32_t        pid)
{
  int ii;
  ES_unit_list_p  list;

  if (picture == NULL || picture->list == NULL)
    return 0;
  
  list = picture->list;
  
  for (ii = 0; ii < list->length; ii++)
  {
    int  err;
    ES_unit_p  unit = &(list->array[ii]);

    err = write_ES_as_TS_PES_packet(tswriter,unit->data,unit->data_len,pid,
                                    DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error writing out picture list to TS\n");
      return err;
    }
  }
  return 0;
}

/*
 * Write out a picture (as stored in an ES unit list) as ES
 *
 * - `output` is the ES output file
 * - `picture` is the picture to write out
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int write_h262_picture_as_ES(FILE           *output,
                                    h262_picture_p  picture)
{
  int ii;
  ES_unit_list_p  list;

  if (picture == NULL || picture->list == NULL)
    return 0;
  
  list = picture->list;
  
  for (ii = 0; ii < list->length; ii++)
  {
    int  err;
    ES_unit_p  unit = &(list->array[ii]);
    err = write_ES_unit(output,unit);
    if (err)
    {
      print_err("### Error writing out picture list to ES\n");
      return err;
    }
  }
  return 0;
}

/*
 * Report on an H.262 picture's contents.
 *
 * - `stream` is where to write the information
 * - `picture` is the picture to report on
 * - if `report_data`, then the component ES units will be printed out as well
 */
extern void report_h262_picture(h262_picture_p  picture,
                                int             report_data)
{
  if (picture->is_picture)
  {
    fprint_msg("%s %s #%02d",
               H262_PICTURE_CODING_STR(picture->picture_coding_type),
               H262_PICTURE_STRUCTURE_STR(picture->picture_structure),
               picture->temporal_reference);

    if (picture->was_two_fields)
      print_msg(" (merged)");

    fprint_msg(" %s",H262_ASPECT_RATIO_INFO_STR(picture->aspect_ratio_info));

    if (picture->is_real_afd)
      print_msg(" AFD ");
    else
      print_msg(" afd ");
    print_bits(4,picture->afd);
    fprint_msg(", i.e., %s",SHORT_AFD_STR(picture->afd));
    print_msg("\n");
  }
  else if (picture->is_sequence_header)
  {
    print_msg("Sequence header: ");

    switch (picture->progressive_sequence)
    {
    case 0: print_msg("frames and fields"); break;
    case 1: print_msg("progressive frames only"); break;
    default:
      fprint_msg("progressive_sequence=%d",
                 picture->progressive_sequence);
      break;
    }
    fprint_msg(", aspect ratio %s",
               H262_ASPECT_RATIO_INFO_STR(picture->aspect_ratio_info));
    print_msg("\n");
  }
  else
  {
    print_msg("Sequence end\n");
  }
  if (report_data)
    report_ES_unit_list("ES units",picture->list);
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

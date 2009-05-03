/*
 * Utilities for reading H.264 elementary streams.
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
#ifdef _WIN32
#include <io.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "printing_fns.h"
#include "misc_fns.h"
#include "pes_fns.h"
#include "tswrite_fns.h"
#include "es_fns.h"
#include "printing_fns.h"

#define DEBUG 0

// A lone forwards reference
static inline int get_more_data(ES_p  es);

// ------------------------------------------------------------
// Basic functions
// ------------------------------------------------------------
/*
 * Open an ES file and build an elementary stream datastructure to read
 * it with.
 *
 * - `filename` is the ES files name. As a special case, if this is NULL
 *   then standard input (STDIN_FILENO) will be read from.
 *
 * Opens the file for read, builds the datastructure, and reads the first 3
 * bytes of the input file (this is done to prime the triple-byte search
 * mechanism).
 *
 * Returns 0 if all goes well, 1 otherwise.
 */
extern int open_elementary_stream(char  *filename,
                                  ES_p  *es)
{
  int err;
  int input;

  if (filename == NULL)
    input = STDIN_FILENO;
  else
  {
    input = open_binary_file(filename,FALSE);
    if (input == -1) return 1;
  }
  
  err = build_elementary_stream_file(input,es);
  if (err)
  {
    fprint_err("### Error building elementary stream for file %s\n", filename);
    return 1;
  }
  return 0;
}

static int setup_readahead(ES_p  es)
{
  int err;

  es->read_ahead_len = 0;
  es->read_ahead_posn = 0;
  
  es->data = NULL;
  es->data_end = NULL;
  es->data_ptr = NULL;

  es->last_packet_posn = 0;
  es->last_packet_es_data_len = 0;

  // Try to get the first chunk of data from the file
  err = get_more_data(es);
  if (err) return err;

  if (es->reading_ES)
  {
    if (es->read_ahead_len < 3)
    {
      fprint_err("### File only contains %d byte%s\n",
                 es->read_ahead_len,(es->read_ahead_len==1?"":"s"));
      return 1;
    }
  }
  else
  {
    if (es->reader->packet->es_data_len < 3)
    {
      fprint_err("### File PES packet only contains %d byte%s\n",
                 es->reader->packet->es_data_len,
                 (es->reader->packet->es_data_len==1?"":"s"));
      return 1;
    }
  }

  if (DEBUG)
    fprint_msg("File starts %02x %02x %02x\n",es->data[0],es->data[1],es->data[2]);

  // Despite (maybe) reporting the above, we haven't actually read anything
  // yet
  es->prev2_byte = es->prev1_byte = es->cur_byte = 0xFF;
  es->posn_of_next_byte.infile = 0;
  es->posn_of_next_byte.inpacket = 0;

  return 0;
}

/*
 * Build an elementary stream datastructure attached to an input file.
 * This is intended for reading ES data files.
 *
 * - `input` is the file stream to read from.
 *
 * Builds the datastructure, and reads the first 3 bytes of the input
 * file (this is done to prime the triple-byte search mechanism).
 *
 * Returns 0 if all goes well, 1 otherwise.
 */
extern int build_elementary_stream_file(int    input,
                                        ES_p  *es)
{
  ES_p new = malloc(SIZEOF_ES);
  if (new == NULL)
  {
    print_err("### Unable to allocate elementary stream datastructure\n");
    return 1;
  }

  new->reading_ES = TRUE;
  new->input = input;
  new->reader = NULL;

  setup_readahead(new);

  *es = new;
  return 0;
}

/*
 * Build an elementary stream datastructure for use with a PES reader.
 * Reads the first (or next) three bytes of the ES.
 *
 * This reads data from the PES video data, ignoring any audio data.
 *
 * - `reader` is the PES reader we want to use to read our TS or PS data.
 *
 * The caller must explicitly close the PES reader as well as closing the
 * elementary stream (closing the ES does not affect the PES reader).
 *
 * Returns 0 if all goes well, 1 otherwise.
 */
extern int build_elementary_stream_PES(PES_reader_p  reader,
                                       ES_p         *es)
{
  ES_p new = malloc(SIZEOF_ES);
  if (new == NULL)
  {
    print_err("### Unable to allocate elementary stream datastructure\n");
    return 1;
  }

  new->reading_ES = FALSE;
  new->input = -1;
  new->reader = reader;

  setup_readahead(new);

  *es = new;
  return 0;
}

/*
 * Tidy up the elementary stream datastructure after we've finished with it.
 *
 * Specifically:
 *
 * - free the datastructure
 * - set `es` to NULL
 *
 * No return status is given, since there's not much one can do if anything
 * *did* go wrong, and if something went wrong and the program is continuing,
 * it's bound to show up pretty soon.
 */
extern void free_elementary_stream(ES_p  *es)
{
  (*es)->input = -1;  // "forget" our input
  free(*es);
  *es = NULL;
}

/*
 * Tidy up the elementary stream datastructure after we've finished with it.
 *
 * Specifically:
 *
 * - close the input file (if its stream is set, and if it's not STDIN)
 * - call `free_elementary_stream()`
 *
 * No return status is given, since there's not much one can do if anything
 * *did* go wrong, and if something went wrong and the program is continuing,
 * it's bound to show up pretty soon.
 */
extern void close_elementary_stream(ES_p  *es)
{
  int input;
  if (*es == NULL)
    return;
  input = (*es)->input;
  if (input != -1 && input != STDIN_FILENO)
    (void) close_file(input);
  free_elementary_stream(es);
}

/*
 * Ask an ES context if changed input is available.
 *
 * This is a convenience wrapper to save querying the ES context to see
 * if it is (a) reading from PES, (b) automatically writing the PES packets
 * out via a TS writer, and (c) if said TS writer has a changed command.
 *
 * Calls `tswrite_command_changed()` on the TS writer associated with this ES.
 *
 * Returns TRUE if there is a changed command.
 */
extern int es_command_changed(ES_p  es)
{
  if (es->reading_ES)
    return FALSE;

  if (es->reader->tswriter == NULL)
    return FALSE;

  return tswrite_command_changed(es->reader->tswriter);
}

// ------------------------------------------------------------
// Handling elementary stream data units
// ------------------------------------------------------------
/*
 * Prepare the contents of a (new) ES unit datastructure.
 *
 * Allocates a new data array, and unsets the counts.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int setup_ES_unit(ES_unit_p  unit)
{
  unit->data = malloc(ES_UNIT_DATA_START_SIZE);
  if (unit->data == NULL)
  {
    print_err("### Unable to allocate ES unit data buffer\n");
    return 1;
  }
  unit->data_len = 0;
  unit->data_size = ES_UNIT_DATA_START_SIZE;
  unit->start_posn.infile = 0;
  unit->start_posn.inpacket = 0;

  unit->PES_had_PTS = FALSE;    // See the header file
  return 0;
}

/*
 * Tidy up an ES unit datastructure after we've finished with it.
 *
 * (Frees the internal data array, and unsets the counts)
 */
extern void clear_ES_unit(ES_unit_p  unit)
{
  if (unit->data != NULL)
  {
    free(unit->data);
    unit->data = NULL;
    unit->data_size = 0;
    unit->data_len = 0;
  }
}

/*
 * Build a new ES unit datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_ES_unit(ES_unit_p  *unit)
{
  int err;
  ES_unit_p  new = malloc(SIZEOF_ES_UNIT);
  if (new == NULL)
  {
    print_err("### Unable to allocate ES unit datastructure\n");
    return 1;
  }
  err = setup_ES_unit(new);
  if (err)
  {
    free(new);
    return 1;
  }
  *unit = new;
  return 0;
}

/*
 * Build a new ES unit datastructure, from a given data array.
 *
 * Takes a copy of 'data'. Sets 'start_code' appropriately,
 * sets 'start_posn' to (0,0), and 'PES_had_PTS' to FALSE.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_ES_unit_from_data(ES_unit_p  *unit,
                                   byte       *data,
                                   uint32_t    data_len)
{
  ES_unit_p  new = malloc(SIZEOF_ES_UNIT);
  if (new == NULL)
  {
    print_err("### Unable to allocate ES unit datastructure\n");
    return 1;
  }
  new->data = malloc(data_len);
  if (new->data == NULL)
  {
    print_err("### Unable to allocate ES unit data buffer\n");
    return 1;
  }
  (void) memcpy(new->data, data, data_len);
  new->data_len  = data_len;
  new->data_size = data_len;
  new->start_code = data[3];
  new->start_posn.infile = 0;
  new->start_posn.inpacket = 0;
  new->PES_had_PTS = FALSE;    // See the header file
  *unit = new;
  return 0;
}

/*
 * Tidy up and free an ES unit datastructure after we've finished with it.
 *
 * Empties the ES unit datastructure, frees it, and sets `unit` to NULL.
 *
 * If `unit` is already NULL, does nothing.
 */
extern void free_ES_unit(ES_unit_p  *unit)
{
  if (*unit == NULL)
    return;
  clear_ES_unit(*unit);
  free(*unit);
  *unit = NULL;
}

/*
 * Print out some information this ES unit, on normal or error output
 */
extern void report_ES_unit(int        is_msg,
                           ES_unit_p  unit)
{
  byte s = unit->start_code;
  fprint_msg_or_err(is_msg,
                    OFFSET_T_FORMAT_08 "/%4d: ES unit (%02x '%d%d%d%d %d%d%d%d')",
                    unit->start_posn.infile,unit->start_posn.inpacket,s,
                    (s&0x80)>>7,(s&0x40)>>6,(s&0x20)>>5,(s&0x10)>>4,
                    (s&0x08)>>3,(s&0x04)>>2,(s&0x02)>>1,(s&0x01));

  // Show the data bytes - but we don't need to show the first 4,
  // since we know they're 00 00 01 <start-code>
  if (unit->data_len > 0)
  {
    int ii;
    int data_len = unit->data_len - 4;
    int show_len = (data_len>10?10:data_len);
    fprint_msg_or_err(is_msg," %6d:",data_len);
    for (ii = 0; ii < show_len; ii++)
      fprint_msg_or_err(is_msg," %02x",unit->data[4+ii]);
    if (show_len < data_len)
      fprint_msg_or_err(is_msg,"...");
  }
  fprint_msg_or_err(is_msg,"\n");
}

// ------------------------------------------------------------
// ES unit *data* stuff
// ------------------------------------------------------------
/*
 * A wrapper for `read_next_PES_ES_packet()`, to save us forgetting things
 * we need to do when we call it.
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read, otherwise
 * 1 if some error occurs.
 */
static inline int get_next_pes_packet(ES_p  es)
{
  int  err;
  PES_reader_p  reader = es->reader;

  // Before reading the *next* packet, remember where the last one was
  if (reader->packet == NULL)
  {
    // What can we do if there was no last packet?
    es->last_packet_posn = 0;
    es->last_packet_es_data_len = 0;
  }
  else
  {
    es->last_packet_posn = reader->packet->posn;
    es->last_packet_es_data_len = reader->packet->es_data_len;
  }

  err = read_next_PES_ES_packet(es->reader);
  if (err) return err;

  // Point to our (new) data buffer
  es->data = reader->packet->es_data;
  es->data_end = es->data + reader->packet->es_data_len;
  es->data_ptr = es->data;

  es->posn_of_next_byte.infile = reader->packet->posn;
  es->posn_of_next_byte.inpacket = 0;
  return 0;
}

/*
 * Read some more data into our read-ahead buffer. For a "bare" file,
 * reads the next buffer-full in, and for PES based data, reads the
 * next PES packet that contains ES data.
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read, otherwise
 * 1 if some error occurs.
 */
static inline int get_more_data(ES_p  es)
{
  if (es->reading_ES)
  {
    // Call `read` directly - we don't particularly mind if we get a "short"
    // read, since we'll just catch up later on
#ifdef _WIN32
    int len = _read(es->input,&es->read_ahead,ES_READ_AHEAD_SIZE);
#else
    ssize_t  len = read(es->input,&es->read_ahead,ES_READ_AHEAD_SIZE);
#endif
    if (len == 0)
      return EOF;
    else if (len == -1)
    {
      fprint_err("### Error reading next bytes: %s\n",strerror(errno));
      return 1;
    }
    es->read_ahead_posn += es->read_ahead_len;  // length of the *last* buffer
    es->read_ahead_len = len;
    es->data = es->read_ahead;     // should be done in the setup function
    es->data_end = es->data + len; // one beyond the last byte
    es->data_ptr = es->data;
    return 0;
  }
  else
  {
    return get_next_pes_packet(es);
  }
}

/*
 * Find the start of the next ES unit - i.e., a 00 00 01 start code prefix.
 *
 * Doesn't move the read position if we're already *at* the start of
 * an ES unit.
 *
 * ((new scheme: Leaves the data_ptr set to read the *next* byte, since
 * we know that we've "used up" the 00 00 01 at the start of this unit.))
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read, otherwise
 * 1 if some error occurs.
 */
static int find_ES_unit_start(ES_p       es,
                              ES_unit_p  unit)
{
  int   err;
  byte  prev1 = es->prev1_byte;
  byte  prev2 = es->prev2_byte;

  // In almost all cases (hopefully, except for the very start of the file),
  // a previous call to find_ES_unit_end will already have positioned us
  // "over" the start of the next unit
  for (;;)
  {
    byte  *ptr;
    for (ptr = es->data_ptr; ptr < es->data_end; ptr++)
    {
      if (prev2 == 0x00 && prev1 == 0x00 && *ptr == 0x01)
      {
        es->prev1_byte = es->prev2_byte = 0x00;
        es->cur_byte = 0x01;
        if (es->reading_ES)
        {
          unit->start_posn.infile = es->read_ahead_posn + (ptr - es->data) - 2;
        }
        else
        {
          unit->start_posn.infile = es->reader->packet->posn;
          unit->start_posn.inpacket = (ptr - es->data) - 2;
          if (unit->start_posn.inpacket < 0)
          {
            unit->start_posn.infile = es->last_packet_posn;
            unit->start_posn.inpacket += es->last_packet_es_data_len;
          }
          // Does the PES packet that we are starting in have a PTS?
          unit->PES_had_PTS = es->reader->packet->has_PTS;
        }
        es->data_ptr = ptr + 1; // the *next* byte to read
        unit->data[0] = 0x00;   // i.e., the values we just read
        unit->data[1] = 0x00;
        unit->data[2] = 0x01;
        unit->data_len = 3;
        return 0;
      }
      prev2 = prev1;
      prev1 = *ptr;
    }

    // We've run out of data - get some more
    err = get_more_data(es);
    if (err) return err;
  }
}

/*
 * Find (read to) the end of the current ES unit.
 *
 * Reads to just before the next 00 00 01 start code prefix.
 *
 *    H.264 rules would also allow us to read to just before a 00 00 00
 *    sequence, but we shall ignore this ability, because when reading
 *    ES we want to ensure that there are no bytes "left out" of the ES
 *    units, so that the sum of the lengths of all the ES units we read will
 *    be the same as the length of data we have read. This is desirable
 *    because it makes handling ES via PES much easier (we can, for instance,
 *    position to the first ES unit of a picture, and then just read in N
 *    bytes, where N is the sum of the lengths of the ES units making up the
 *    picture, which is much more efficient than having to read in individual
 *    ES units, and takes less room to remember than having to remember the
 *    end position (offset of PES packet in file + offset of end of ES unit in
 *    PES packet)).
 *
 * ((new scheme: Leaves the data_ptr set to read the current byte again,
 * since we know that, in general, we want to detect it in find_ES_unit_start
 * as the 01 following on from a 00 and a 00.))
 *
 * Returns 0 if it succeeds, otherwise 1 if some error occurs.
 *
 * Note that finding end-of-file is not counted as an error - it is
 * assumed that it is just the natural end of the ES unit.
 */
static int find_ES_unit_end(ES_p       es,
                            ES_unit_p  unit)
{
  int   err;
  byte  prev1 = es->cur_byte;
  byte  prev2 = es->prev1_byte;
  for (;;)
  {
    byte  *ptr;
    for (ptr = es->data_ptr; ptr < es->data_end; ptr++)
    {
      // Have we reached the end of our unit?
      // We know we are if we've found the next 00 00 01 start code prefix.
      // (as stated in the header comment above, we're ignoring the H.264
      // ability to end if we've found a 00 00 00 sequence)
      if (prev2 == 0x00 && prev1 == 0x00 && *ptr == 0x01)
      {
        es->data_ptr = ptr;     // remember where we've got to
        es->prev2_byte = 0x00;  // we know prev1_byte is already 0
        es->cur_byte = 0x01;
        // We've read two 00 bytes we don't need into our data buffer...
        unit->data_len -= 2;

        if (es->reading_ES)
        {
          es->posn_of_next_byte.infile = es->read_ahead_posn +
            (ptr - es->data) - 2;
        }
        else
        {
          es->posn_of_next_byte.infile = es->reader->packet->posn;
          es->posn_of_next_byte.inpacket = (ptr - es->data) - 2;
        }
        return 0;
      }

      // Otherwise, it's a data byte
      if (unit->data_len == unit->data_size)
      {
        int newsize = unit->data_size + ES_UNIT_DATA_INCREMENT;
        unit->data = realloc(unit->data,newsize);
        if (unit->data == NULL)
        {
          print_err("### Unable to extend ES unit data array\n");
          return 1;
        }
        unit->data_size = newsize;
      }
      unit->data[unit->data_len++] = *ptr;

      prev2 = prev1;
      prev1 = *ptr;
    }

    // We've run out of data (ptr == es->data_end) - get some more
    err = get_more_data(es);
    if (err == EOF)
    {
      // Reaching the end of file is a legitimate way of stopping!
      es->data_ptr = ptr;     // remember where we've got to
      es->prev2_byte = prev2;
      es->prev1_byte = prev1;
      es->cur_byte = 0xFF;    // the notional byte off the end of the file
      //es->cur_byte   = *ptr;

      // Pretend there's a "next byte"
      if (es->reading_ES)
      {
        es->posn_of_next_byte.infile = es->read_ahead_posn + (ptr - es->data);
      }
      else
      {
        es->posn_of_next_byte.inpacket = (ptr - es->data);
      }
      return 0;
    }
    else if (err)
      return err;

    if (!es->reading_ES)
    {
      // If we update this now, it will be correct when we return,
      // even if we return because of a later EOF
      es->posn_of_next_byte.infile = es->reader->packet->posn;

      // Does the PES packet that we have just read in have a PTS?
      // If it does, then there's a very good chance (subject to a 00 00 01
      // being split between PES packets) that our ES unit has a PTS "around"
      // it
      if (es->reader->packet->has_PTS)
        unit->PES_had_PTS = TRUE;
    }
  }
}

/*
 * Find and read in the next ES unit.
 *
 * In general, unless there are compelling reasons, use
 * `find_and_build_next_ES_unit()` instead.
 *
 * - `es` is the elementary stream we're reading from.
 * - `unit` is the datastructure into which to read the ES unit
 *   - any previous content will be lost.
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read (i.e., there
 * is no next ES unit), otherwise 1 if some error occurs.
 */
extern int find_next_ES_unit(ES_p       es,
                             ES_unit_p  unit)
{
  int err;
  
  err = find_ES_unit_start(es,unit);
  if (err) return err;  // 1 or EOF

  err = find_ES_unit_end(es,unit);
  if (err) return err;

  // The first byte after the 00 00 01 prefix tells us what sort of thing
  // we've found - we'll be friendly and extract it for the user
  unit->start_code = unit->data[3];

  return 0;
}

/*
 * Find and read the next ES unit into a new datastructure.
 *
 * - `es` is the elementary stream we're reading from.
 * - `unit` is the datastructure containing the ES unit found, or NULL
 *   if there was none.
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read (i.e., there
 * is no next ES unit), otherwise 1 if some error occurs.
 */
extern int find_and_build_next_ES_unit(ES_p        es,
                                       ES_unit_p  *unit)
{
  int err;

  err = build_ES_unit(unit);
  if (err) return 1;

  err = find_next_ES_unit(es,*unit);
  if (err)
  {
    free_ES_unit(unit);
    return err;
  }
  return 0;
}

/*
 * Write (copy) the current ES unit to the output stream.
 *
 * Note that it writes out all of the data for this ES unit,
 * including its 00 00 01 start code prefix.
 *
 * - `output` is the output stream (file descriptor) to write to
 * - `unit` is the ES unit to write
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int write_ES_unit(FILE      *output,
                         ES_unit_p  unit)
{
  size_t written = fwrite(unit->data,1,unit->data_len,output);
  if (written != unit->data_len)
  {
    fprint_err("### Error writing out ES unit data: %s\n"
               "    Wrote %ld bytes instead of %d\n",
               strerror(errno),(long int)written,unit->data_len);
    return 1;
  }
  else
    return 0;
}

// ------------------------------------------------------------
// Arbitrary reading from ES data
// ------------------------------------------------------------
/*
 * Seek within PES underlying ES.
 *
 * This should only be used to seek to data that starts with 00 00 01.
 *
 * "Unsets" the triple byte context.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int seek_in_PES(ES_p       es,
                       ES_offset  where)
{
  int  err;

  if (es->reader == NULL)
  {
    print_err("### Attempt to seek in PES for an ES reader that"
              " is not attached to a PES reader\n");
    return 1;
  }

  // Force the reader to forget its current packet
  if (es->reader->packet != NULL)
    free_PES_packet_data(&es->reader->packet);

  // Seek to the right packet in the PES data
  err = set_PES_reader_position(es->reader,where.infile);
  if (err)
  {
    fprint_err("### Error seeking for PES packet at " OFFSET_T_FORMAT
               "\n",where.infile);
    return 1;
  }
  // Read the PES packet containing ES (ignoring packets we don't care about)
  err = get_next_pes_packet(es);
  if (err)
  {
    fprint_err("### Error reading PES packet at " OFFSET_T_FORMAT "/%d\n",
               where.infile,where.inpacket);
    return 1;
  }

  // Now sort out the byte offset
  if (where.inpacket > es->reader->packet->es_data_len)
  {
    fprint_err("### Error seeking PES packet at " OFFSET_T_FORMAT "/%d: "
               " packet ES data is only %d bytes long\n",where.infile,
               where.inpacket,es->reader->packet->es_data_len);
    return 1;
  }
  es->posn_of_next_byte = where;
  return 0;
}

/*
 * Update our current position information after a seek or direct read.
 */
static inline void deduce_correct_position(ES_p   es)
{
  // We don't know what the previous three bytes were, but we (strongly)
  // assume that they were not 00 00 01
  es->cur_byte = 0xff;
  es->prev1_byte = 0xff;
  es->prev2_byte = 0xff;

  if (es->reading_ES)
  {
    // For ES data, we want to force new data to be read in from the file
    es->data_ptr = es->data_end = NULL;
    es->read_ahead_len = 0;  // to stop the read ahead posn being incremented
    es->read_ahead_posn = es->posn_of_next_byte.infile;
  }
  else
  {
    // For PES data, we have whatever is left in the current packet
    PES_packet_data_p packet = es->reader->packet;
    es->data     = packet->es_data;
    es->data_ptr = packet->es_data + es->posn_of_next_byte.inpacket;
    es->data_end = packet->es_data + packet->es_data_len;
    // And, of course, we have no idea about the *previous* packet in the file
    es->last_packet_posn = es->last_packet_es_data_len = 0;
  }
}

/*
 * "Seek" to the given position in the ES data, which is assumed to
 * be an offset ready to read a 00 00 01 sequence.
 *
 * If the ES reader is using PES to read its data, then both fields
 * of `where` are significant, but if the underlying file *is* just a file,
 * only `where.infile` is used.
 *
 * Returns 0 if all went well, 1 is something went wrong
 */
extern int seek_ES(ES_p       es,
                   ES_offset  where)
{
  int err;
  if (es->reading_ES)
  {
    err = seek_file(es->input,where.infile);
    if (err)
    {
      print_err("### Error seeking within ES file\n");
      return 1;
    }
  }
  else
  {
    err = seek_in_PES(es,where);
    if (err)
    {
      fprint_err("### Error seeking within ES over PES (offset " OFFSET_T_FORMAT
                 "/%d)\n",where.infile,where.inpacket);
      return 1;
    }
  }

  // And make it look as if we reached this position sensibly
  es->posn_of_next_byte = where;
  deduce_correct_position(es);
  return 0;
}

/*
 * Retrieve ES bytes from PES as requested
 *
 * Leaves the PES reader set to read on after this data.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int read_bytes_from_PES(ES_p     es,
                               byte    *data,
                               uint32_t num_bytes)
{
  int     err;
  int     offset = 0;
  int     num_bytes_wanted = num_bytes;
  int32_t from = es->posn_of_next_byte.inpacket;
  int32_t num_bytes_left = es->reader->packet->es_data_len - from;
  
  for (;;)
  {
    if (num_bytes_left < num_bytes_wanted)
    {
      memcpy(&(data[offset]),&(es->reader->packet->es_data[from]),
             num_bytes_left);
      offset += num_bytes_left;
      num_bytes_wanted -= num_bytes_left;
      err = get_next_pes_packet(es);
      if (err) return err;
      from = 0;
      num_bytes_left = es->reader->packet->es_data_len;
    }
    else
    {
      memcpy(&(data[offset]),&(es->reader->packet->es_data[from]),
             num_bytes_wanted);
      from += num_bytes_wanted;
      break;
    }
  }
  es->posn_of_next_byte.inpacket = from;
  //es->posn_of_next_byte.infile   = es->reader->packet->posn;
  return 0;
}

/*
 * Read in some ES data from disk.
 *
 * Suitable for use when reading in a set of ES units whose bounds
 * (start offset and total number of bytes) have been remembered.
 *
 * "Seeks" to the given position in the ES data, which is assumed to
 * be an offset ready to read a 00 00 01 sequence, and reads data thereafter.
 *
 * After this function, the triple byte context is set to FF FF FF, and the
 * position of said bytes are undefined, but the next position to read a byte
 * from *is* defined.
 *
 * The intent is to allow the caller to have a data array (`data`) that
 * always contains the last data read, and is of the required size, and
 * need only be freed when no more data is needed.
 *
 * - `es` is where to read our data from
 * - `start_posn` is the file offset to start reading at
 * - `num_bytes` is how many bytes we want to read
 * - `data_len` may be NULL or a pointer to a value.
 *   If it is NULL, then the data array will be reallocated to size
 *   `num_bytes` regardless. If it is non-NULL, it should be passed *in*
 *    as the size that `data` *was*, and will be returned as the size
 *    that `data` is when the function returns.
 * - `data` is the data array to read into. If this is NULL, or if `num_bytes`
 *   is NULL, or if `num_bytes` is greater than `data_len`, then it will be
 *   reallocated to size `num_bytes`.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int read_ES_data(ES_p       es,
                        ES_offset  start_posn,
                        uint32_t   num_bytes,
                        uint32_t  *data_len,
                        byte     **data)
{
  int  err;
  if (*data == NULL || data_len == NULL || num_bytes > *data_len)
  {
    *data = realloc(*data,num_bytes);
    if (*data == NULL)
    {
      print_err("### Unable to reallocate data space\n");
      return 1;
    }
    if (data_len != NULL)
      *data_len = num_bytes;
  }
  err = seek_ES(es,start_posn);
  if (err) return err;
  if (es->reading_ES)
  {
    err = read_bytes(es->input,num_bytes,*data);
    if (err)
    {
      if (err == EOF)
      {
        fprint_err("### Error (EOF) reading %d bytes\n",num_bytes);
        return 1;
      }
      else
        return err;
    }
    es->posn_of_next_byte.infile = start_posn.infile + num_bytes;
  }
  else
  {
    err = read_bytes_from_PES(es,*data,num_bytes);
    if (err)
    {
      fprint_err("### Error reading %d bytes from PES\n",num_bytes);
      return 1;
    }
  }
  // Make it look as if we "read" to this position by the normal means,
  // but ensure that we have no data left "in hand"
  //
  // We could leave it up to our caller to do this, on the assumption that
  // they're likely to call us several times when, for example, reversing,
  // without wanting to read onwards on all but the last of those occasions.
  // That would, indeed, save some time each time we are called, but it would
  // also allow our caller to forget to do this, with rather bad results.
  //
  // So, since it shouldn't really take very long...
  deduce_correct_position(es);
  return 0;
}

/*
 * Retrieve ES data from the end of a PES packet. It is assumed (i.e, things
 * will go wrong if it is not true) that at least one ES unit has been read
 * from the PES data stream via the ES reader.
 *
 * - `es` is our ES reader. It must be reading ES from PES packets.
 * - `data` is the ES data remaining (to be read) in the current PES packet.
 *   It is up to the caller to free this data.
 * - `data_len` is the length of said data. If this is 0, then `data`
 *   will be NULL.
 *
 * Returns 0 if all goes well, 1 if an error occurs.
 */
extern int get_end_of_underlying_PES_packet(ES_p        es,
                                            byte      **data,
                                            int        *data_len)
{
  int32_t offset;

  if (es->reading_ES)
  {
    fprint_err("### Cannot retrieve end of PES packet - the ES data"
               " is direct ES, not ES read from PES\n");
    return 1;
  }
  if (es->reader->packet == NULL)
  {
    // This is naughty, but we'll pretend to cope
    *data = NULL;
    *data_len = 0;
    return 0;
  }

  // The offset (in this packet) of the next ES byte to read.
  // We assume that this must also be the offset of the first byte
  // of the next ES unit (or, at least, of one of the 00 bytes that
  // come before it).
  offset = es->posn_of_next_byte.inpacket;

  // The way that we read (using our "triple byte" mechanism) means that
  // we will generally already have read the start of the next ES unit.
  // Life gets interesting if the 00 00 01 triplet (or, possibly, 00 00 00
  // triplet - but we're not supporting that option for the moment - see
  // find_ES_unit_end for details) is split over a PES packet boundary.

  // So we know we 00 00 01 to "start" a new ES unit and end the previous
  // one. (In fact, even if it was 00 00 00, the relevant values are held in
  // our triple byte memory, so we don't particularly care which it is.)
  //
  // If offset is 0, then then next byte to read is the first byte of
  // this packet's ES data, so we need to "pretend" to have all three
  // of the triple bytes "in front of" the actual ES data for this PES
  // packet.
  //
  // If offset is 1, then presumably the cur_byte was at offset 0, and
  // we have two "dangling" bytes in the previous packet.
  //
  // If offset is 2, then there would only be one "dangling" byte.
  //
  // Finally, if offset is 3 or more, we know there was room for the
  // 00 00 01 or 00 00 00 before the next byte we'll read, so we don't
  // need to bluff at all.

  // So, to calculation - we must remember to leave room for those
  // three bytes at the start of the data we return
  *data_len = es->reader->packet->es_data_len - offset + 3;
  *data = malloc(*data_len);
  if (*data == NULL)
  {
    print_err("### Cannot allocate space for rest of PES packet\n");
    return 1;
  }
  (*data)[0] = es->prev2_byte; // Hmm - should be 0x00
  (*data)[1] = es->prev1_byte; // Hmm - should be 0x00
  (*data)[2] = es->cur_byte;   // Hmm - should be 0x01 (see above)
  memcpy( &((*data)[3]),
          &(es->reader->packet->es_data[offset]),
          (*data_len) - 3);

  return 0;
}

// ------------------------------------------------------------
// Lists of ES units
// ------------------------------------------------------------
/*
 * Build a new list-of-ES-units datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_ES_unit_list(ES_unit_list_p  *list)
{
  ES_unit_list_p  new = malloc(SIZEOF_ES_UNIT_LIST);
  if (new == NULL)
  {
    print_err("### Unable to allocate ES unit list datastructure\n");
    return 1;
  }
  
  new->length = 0;
  new->size = ES_UNIT_LIST_START_SIZE;
  new->array = malloc(SIZEOF_ES_UNIT*ES_UNIT_LIST_START_SIZE);
  if (new->array == NULL)
  {
    free(new);
    print_err("### Unable to allocate array in ES unit list datastructure\n");
    return 1;
  }
  *list = new;
  return 0;
}

/*
 * Add a copy of an ES unit to the end of the ES unit list
 *
 * Note that since this takes a copy of the ES unit's data, it is safe
 * to free the original ES unit.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int append_to_ES_unit_list(ES_unit_list_p  list,
                                  ES_unit_p       unit)
{
  ES_unit_p  ptr;
  if (list->length == list->size)
  {
    int newsize = list->size + ES_UNIT_LIST_INCREMENT;
    list->array = realloc(list->array,newsize*SIZEOF_ES_UNIT);
    if (list->array == NULL)
    {
      print_err("### Unable to extend ES unit list array\n");
      return 1;
    }
    list->size = newsize;
  }
  ptr = &list->array[list->length++];
  // Some things can be copied directly
  *ptr = *unit;
  // But some need adjusting
  ptr->data = malloc(unit->data_len);
  if (ptr->data == NULL)
  {
    print_err("### Unable to copy ES unit data array\n");
    return 1;
  }
  memcpy(ptr->data,unit->data,unit->data_len);
  ptr->data_size = unit->data_len;
  return 0;
}

/*
 * Tidy up an ES unit list datastructure after we've finished with it.
 */
static inline void clear_ES_unit_list(ES_unit_list_p  list)
{
  if (list->array != NULL)
  {
    int ii;
    for (ii=0; ii<list->length; ii++)
    {
      clear_ES_unit(&list->array[ii]);
    }
    free(list->array);
    list->array = NULL;
  }
  list->length = 0;
  list->size = 0;
}

/*
 * Reset (empty) an ES unit list.
 */
extern void reset_ES_unit_list(ES_unit_list_p  list)
{
  if (list->array != NULL)
  {
    int ii;
    for (ii=0; ii<list->length; ii++)
    {
      clear_ES_unit(&list->array[ii]);
    }
    // We *could* also shrink it - as it is, it will never get smaller
    // than its maximum size. Is that likely to be a problem?
  }
  list->length = 0;
}

/*
 * Tidy up and free an ES unit list datastructure after we've finished with it.
 *
 * Clears the datastructure, frees it and returns `list` as NULL.
 *
 * Does nothing if `list` is already NULL.
 */
extern void free_ES_unit_list(ES_unit_list_p  *list)
{
  if (*list == NULL)
    return;
  clear_ES_unit_list(*list);
  free(*list);
  *list = NULL;
}

/*
 * Report on an ES unit list's contents.
 *
 * - `name` is the name of the list (used in the header)
 * - `list` is the list to report on
 */
extern void report_ES_unit_list(char              *name,
                                ES_unit_list_p     list)
{
  fprint_msg("ES unit list '%s': ",name);
  if (list->array == NULL)
    print_msg("<empty>\n");
  else
  {
    int ii;
    fprint_msg("%d item%s (size %d)\n",list->length,
               (list->length==1?"":"s"),list->size);
    for (ii=0; ii<list->length; ii++)
    {
      print_msg("    ");
      report_ES_unit(TRUE,&(list->array[ii]));
    }
  }
}

/*
 * Retrieve the bounds of this ES unit list in the file it was read from.
 *
 * - `list` is the ES unit list we're interested in
 * - `start` is its start position (i.e., the location at which to start
 *   reading to retrieve all of the data for the list)
 * - `length` is the total length of the ES units within this list
 *
 * Returns 0 if all goes well, 1 if the ES unit list has no content.
 */
extern int get_ES_unit_list_bounds(ES_unit_list_p   list,
                                   ES_offset       *start,
                                   uint32_t        *length)
{
  int ii;
  if (list->array == NULL || list->length == 0)
  {
    print_err("### Cannot determine bounds of an ES unit list with no content\n");
    return 1;
  }

  *start = list->array[0].start_posn;
  *length = 0;

  // Maybe we should precalculate, or even cache, the total length...
  for (ii=0; ii<list->length; ii++)
    (*length) += list->array[ii].data_len;

  return 0;
}

/*
 * Compare two ES unit lists. The comparison does not include the start
 * position of the unit data, but just the actual data - i.e., two unit lists
 * read from different locations in the input stream may be considered the
 * same if their data content is identical.
 *
 * - `list1` and `list2` are the two ES unit lists to compare.
 *
 * Returns TRUE if the lists contain identical content, FALSE otherwise.
 */
extern int same_ES_unit_list(ES_unit_list_p  list1,
                             ES_unit_list_p  list2)
{
  int ii;
  if (list1 == list2)
    return TRUE;

  if (list1->array == NULL)
    return (list2->array == NULL);

  if (list1->length != list2->length)
    return FALSE;

  for (ii = 0; ii < list1->length; ii++)
  {
    ES_unit_p  unit1 = &list1->array[ii];
    ES_unit_p  unit2 = &list2->array[ii];

    if (unit1->data_len != unit2->data_len)
      return FALSE;

    if (memcmp(unit1->data,unit2->data,unit1->data_len))
      return FALSE;
  }

  return TRUE;
}

/*
 * Compare two ES offsets
 *
 * Returns -1 if offset1 < offset2, 0 if they are the same, and 1 if
 * offset1 > offset2.
 */
extern int compare_ES_offsets(ES_offset  offset1,
                              ES_offset  offset2)
{
  if (offset1.infile < offset2.infile)
    return -1;
  else if (offset1.infile > offset2.infile)
    return 1;
  else if (offset1.inpacket < offset2.inpacket)
    return -1;
  else if (offset1.inpacket > offset2.inpacket)
    return 1;
  else
    return 0;
}

// ============================================================
// Simple file type guessing
// ============================================================
/*
 * Is an ES unit H.262 or H.264 (or not sure?)
 *
 * Return 0 if all goes well, 1 if things go wrong.
 */
static int try_to_guess_video_type(ES_unit_p   unit,
                                   int         show_reasoning,
                                   int        *maybe_h264,
                                   int        *maybe_h262,
                                   int        *maybe_avs)
{

  byte nal_ref_idc = 0;
  byte nal_unit_type = 0;

  if (show_reasoning)
    fprint_msg("Looking at ES unit with start code %02X\n",unit->start_code);

  // The following are *not allowed*
  //
  // - In AVS:   B4, B8     and B9..FF are system start codes
  // - In H.262: B0, B1, B6 and B9..FF are system start codes
  // - In H.264: Anything with top bit set
  
  if (unit->start_code == 0xBA)   // PS pack header
  {
    print_err("### ES unit start code is 0xBA, which looks like a PS pack"
              " header\n    i.e., data may be PS\n");
    return 1;
  }

  if (unit->start_code >= 0xB9)   // system start code - probably PES
  {
    fprint_err("### ES unit start code %02X is more than 0xB9, which is probably"
               " a PES system start code\n    i.e., data may be PES, "
               "and is thus probably PS or TS\n",
               unit->start_code);
    return 1;
  }

  if (unit->start_code & 0x80)    // top bit set means not H.264
  {
    if (*maybe_h264)
    {
      if (show_reasoning) fprint_msg("  %02X has top bit set, so not H.264,\n",unit->start_code);
      *maybe_h264 = FALSE;
    }

    if (unit->start_code == 0xB0 ||
        unit->start_code == 0xB1 ||
        unit->start_code == 0xB6)
    {
      *maybe_h262 = FALSE;
      if (show_reasoning)
        fprint_msg("  Start code %02X is reserved in H.262, so not H.262\n",
                   unit->start_code);
    }
    else if (unit->start_code == 0xB4 ||
             unit->start_code == 0xB8)
    {
      *maybe_avs = FALSE;
      if (show_reasoning)
        fprint_msg("  Start code %02X is reserved in AVS, so not AVS\n",
                   unit->start_code);
    }
  }
  else if (*maybe_h264)
  {
    if (show_reasoning)
      print_msg("  Top bit not set, so might be H.264\n");

    // If we don't have that top bit set, then we need to work a bit harder
    nal_ref_idc = (unit->start_code & 0x60) >> 5;
    nal_unit_type = (unit->start_code & 0x1F);  

    if (show_reasoning)
      fprint_msg("  Interpreting it as nal_ref_idc %d, nal_unit_type %d\n",
                 nal_ref_idc,nal_unit_type);

    if (nal_unit_type > 12 && nal_unit_type < 24)
    {
      if (show_reasoning)
        fprint_msg("  H.264 reserves nal_unit_type %02X,"
                   " so not H.264\n",nal_unit_type);
      *maybe_h264 = FALSE;
    }
    else if (nal_unit_type > 23)
    {
      if (show_reasoning)
        fprint_msg("  H.264 does not specify nal_unit_type %02X,"
                   " so not H.264\n",nal_unit_type);
      *maybe_h264 = FALSE;
    }
    else if (nal_ref_idc == 0)
    {
      if (nal_unit_type == 5 || // IDR picture
          nal_unit_type == 7 || // sequence parameter set
          nal_unit_type == 8)   // picture parameter set
      {
        if (show_reasoning)
          fprint_msg("  H.264 does not allow nal_ref_idc 0 and nal_unit_type %d,"
                     " so not H.264\n",nal_unit_type);
        *maybe_h264 = FALSE;
      }
    }
    else // nal_ref_idc is NOT 0
    {
      // Which means it should *not* be:
      if (nal_unit_type ==  6 || // SEI
          nal_unit_type ==  9 || // access unit delimiter
          nal_unit_type == 10 || // end of sequence
          nal_unit_type == 11 || // end of stream
          nal_unit_type == 12)   // fille
      {
        if (show_reasoning)
          fprint_msg("  H.264 insists nal_ref_idc shall be 0 for nal_unit_type %d,"
                     " so not H.264\n",nal_unit_type);
        *maybe_h264 = FALSE;
      }
    }
  }
  return 0;
}

/*
 * Look at the start of an elementary stream to try to determine its
 * video type.
 *
 * "Eats" the ES units that it looks at, and doesn't rewind the stream
 * afterwards.
 *
 * - `es` is the ES file
 * - if `print_dots` is true, print a dot for each ES unit that is inspected
 * - if `show_reasoning` is true, then output messages explaining how the
 *   decision is being made
 * - `video_type` is the final decision -- one of VIDEO_H264, VIDEO_H262,
 *   VIDEO_AVS, or VIDEO_UNKNOWN.
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int decide_ES_video_type(ES_p  es,
                                int   print_dots,
                                int   show_reasoning,
                                int  *video_type)
{
  int  err;
  int  ii;
  int  maybe_h262 = TRUE;
  int  maybe_h264 = TRUE;
  int  maybe_avs  = TRUE;
  int  decided = FALSE;

  struct ES_unit  unit;

  *video_type = VIDEO_UNKNOWN;

  err = setup_ES_unit(&unit);
  if (err)
  {
    print_err("### Error trying to setup ES unit before"
              " working out video type\n");
    return 1;
  }

  // Otherwise, look at the first 500 packets to see if we can tell
  //
  // Basically, if we find anything with the top byte as B, then it is
  // not H.264. Since H.262 allows up to AF (175) slices (which start with
  // 01..AF), and AVS the same (or perhaps one more) and each "surrounds" those
  // with entities with top byte B, it's rather hard to see how we could go
  // very far without finding something with the top bit of the high byte set
  // (certainly not as far as 500 units). So if we *do* go that far, we can be
  // *very* sure it is not H.262 or AVS. And if the only other choice is H.264,
  // then...
  if (show_reasoning)
    print_msg("Looking through first 500 ES units to try to decide video type\n");
  for (ii=0; ii<500; ii++)
  {
    if (print_dots)
    {
      print_msg(".");
      fflush(stdout);
    }
    else if (show_reasoning)
      fprint_msg("%d: ",ii+1);

    err = find_next_ES_unit(es,&unit);
    if (err == EOF)
    {
      if (print_dots) print_msg("\n");
      if (show_reasoning) fprint_msg("End of file, trying to read ES unit %d\n",ii+2);
      break;
    }
    else if (err)
    {
      if (print_dots) print_msg("\n");
      fprint_err("### Error trying to find 'unit' %d in ES whilst"
                 " working out video type\n",ii+2);
      clear_ES_unit(&unit);
      return 1;
    }
    err = try_to_guess_video_type(&unit,show_reasoning,
                                  &maybe_h264,&maybe_h262,&maybe_avs);
    if (err)
    {
      if (print_dots) print_msg("\n");
      print_err("### Whilst trying to work out video_type\n");
      clear_ES_unit(&unit);
      return 1;
    }

    if (maybe_h264 && !maybe_h262 && !maybe_avs)
    {
      if (show_reasoning) print_msg("  Which leaves only H.264\n");
      *video_type = VIDEO_H264;
      decided = TRUE;
    }
    else if (!maybe_h264 && maybe_h262 && !maybe_avs)
    {
      if (show_reasoning) print_msg("  Which leaves only H.262\n");
      *video_type = VIDEO_H262;
      decided = TRUE;
    }
    else if (!maybe_h264 && !maybe_h262 && maybe_avs)
    {
      if (show_reasoning) print_msg("  Which leaves only AVS\n");
      *video_type = VIDEO_AVS;
      decided = TRUE;
    }
    else
    {
      if (show_reasoning)
        print_msg("  It is not possible to decide from that start code\n");
    }
    if (decided)
      break;
  }
  if (print_dots) print_msg("\n");
  clear_ES_unit(&unit);
  return 0;
}

/*
 * Look at the start of an elementary stream to try to determine it's
 * video type.
 *
 * Note that it is easier to prove something is H.262 (or AVS) than to prove
 * that it is H.264, and that the result of this routine is a best-guess, not a
 * guarantee.
 *
 * Rewinds back to the original position in the file after it has finished.
 *
 * - `input` is the file to look at
 * - if `print_dots` is true, print a dot for each ES unit that is inspected
 * - if `show_reasoning` is true, then output messages explaining how the
 *   decision is being made
 * - `video_type` is the final decision -- one of VIDEO_H264, VIDEO_H262,
 *   VIDEO_AVS, or VIDEO_UNKNOWN.
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int decide_ES_file_video_type(int   input,
                                     int   print_dots,
                                     int   show_reasoning,
                                     int  *video_type)
{
  offset_t  start_posn;
  int       err;
  ES_p      es = NULL;

  start_posn = tell_file(input);
  if (start_posn == -1)
  {
    print_err("### Error remembering start position in file before"
              " working out video type\n");
    return 1;
  }

  err = seek_file(input,0);
  if (err)
  {
    print_err("### Error rewinding file before working out video type\n");
    return 1;
  }

  err = build_elementary_stream_file(input,&es);
  if (err)
  {
    print_err("### Error starting elementary stream before"
              " working out video type\n");
    return 1;
  }

  err = decide_ES_video_type(es,print_dots,show_reasoning,video_type);
  if (err)
  {
    print_err("### Error deciding video type of file\n");
    free_elementary_stream(&es);
    return 1;
  }
  
  free_elementary_stream(&es);

  err = seek_file(input,start_posn);
  if (err)
  {
    print_err("### Error returning to start position in file after"
              " working out video type\n");
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

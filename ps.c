/*
 * Utilities for reading program stream data.
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
#include <stddef.h>
#include <io.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "ps_fns.h"
#include "ts_fns.h"
#include "pes_fns.h"
#include "pidint_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"

#define DEBUG 0
#define DEBUG_AC3 0

// How many bytes to look through for a pack header, before giving up
#define PACK_HEADER_SEARCH_DISTANCE  100000

// If we lose where we are, should we look for the next PS pack header?
// (instead of just giving up)
#define RECOVER_BROKEN_PS 1

// ============================================================
// PS to TS datastructures
// ============================================================

// Data we need to write PAT/PMT and otherwise manage our program streams
//
// DVD allows one video stream and up to 8 audio streams,
// but we're only interested in a single video and a single audio stream
struct program_data
{
  // Information supplied by the user
  uint32_t transport_stream_id;
  uint32_t program_number;
  uint32_t pmt_pid;      // PID to use for the PMT
  uint32_t pcr_pid;      // PID to use for the PCR
  uint32_t video_pid;    // PID to use for our single stream of video
  uint32_t audio_pid;    // PID to use for our single stream of audio
  int      video_stream; // Which stream id our video is (-1 means use first)
  int      audio_stream; // Which stream id our audio is (-1 means use first)
  int      want_ac3;     // True means audio_stream is private_1/AC3 substream
  int      audio_substream;  // Which substream id if DVD and want_ac3
  int      video_type;   // Is our video H.264, H.262, etc. (user must decide)
  int      is_dvd;       // Is our data DVD program stream (ditto)
  int      output_dolby_as_dvb; // Output Dolby (AC-3) audio as DVB or ATSC?
  // Information derived from the data
  // PAT and PMT data
  pidint_list_p prog_list;
  pmt_p         pmt;
};



// ============================================================
// Read ahead support
// ============================================================
/*
 * Read some more data into our read-ahead buffer.
 *
 * Returns 0 if it succeeds, EOF if the end-of-file is read, otherwise
 * 1 if some error occurs.
 */
static inline int get_more_data(PS_reader_p   ps)
{
  // Call `read` directly - we don't particularly mind if we get a "short"
  // read, since we'll just catch up later on
#ifdef _WIN32
  int len = _read(ps->input,&ps->data,PS_READ_AHEAD_SIZE);
#else
  ssize_t  len = read(ps->input,&ps->data,PS_READ_AHEAD_SIZE);
#endif
  if (len == 0)
    return EOF;
  else if (len == -1)
  {
    fprint_err("### Error reading next bytes: %s\n",strerror(errno));
    return 1;
  }
  ps->data_posn += ps->data_len;  // length of the *last* buffer
  ps->data_len = len;
  ps->data_end = ps->data + len;  // one beyond the last byte
  ps->data_ptr = ps->data;        // start at the beginning
  return 0;
}

/*
 * Build a program stream context attached to an input file. This handles
 * read-ahead buffering for the PS.
 *
 * - `input` is the file stream to read from.
 * - If `quiet`, then don't report on ignored bytes at the start of the file
 * - `ps` is the new PS context
 *
 * Returns 0 if all goes well, 1 otherwise.
 */
extern int build_PS_reader(int           input,
                           int           quiet,
                           PS_reader_p  *ps)
{
  int  err;
  PS_reader_p new = malloc(SIZEOF_PS_READER);
  if (new == NULL)
  {
    print_err("### Unable to allocate program stream read context\n");
    return 1;
  }

  new->input = input;
  new->data_posn = 0;
  new->data_len  = 0;
  new->start     = 0;

  err = get_more_data(new);
  if (err)
  {
    print_err("### Unable to start reading from new PS read context\n");
    free(new);
    return 1;
  }

  // And look for the first pack header
  err = find_PS_pack_header_start(new,FALSE,PACK_HEADER_SEARCH_DISTANCE,
                                  &(new->start));
  if (err)
  {
    fprint_err("### File does not appear to be PS\n"
               "    Cannot find PS pack header in first %d bytes of file\n",
               PACK_HEADER_SEARCH_DISTANCE);
    free(new);
    return 1;
  }

  // Seeking won't work on standard input, so don't even try
  if (input != STDIN_FILENO)
  {
    // But we don't *really* want to have read its start yet
    err = seek_using_PS_reader(new,new->start);
    if (err)
    {
      print_err("### Error seeking to start of first pack header\n");
      free(new);
      return 1;
    }
  }

  if (!quiet && new->start != 0)
    fprint_err("!!! PS file does not start with pack header\n"
               "    First PS pack header is at " OFFSET_T_FORMAT "\n",new->start);

  *ps = new;
  return 0;
}

/*
 * Tidy up the PS read-ahead context after we've finished with it.
 *
 * Specifically:
 *
 * - free the datastructure
 * - set `ps` to NULL
 *
 * Does not close the associated file.
 */
extern void free_PS_reader(PS_reader_p  *ps)
{
  if (*ps != NULL)
  {
    (*ps)->input = -1;  // "forget" our input
    free(*ps);
    *ps = NULL;
  }
}

/*
 * Open a PS file for reading.
 *
 * - `name` is the name of the file. If this is NULL, then standard input
 *   is used.
 * - If `quiet`, then don't report on ignored bytes at the start of the file
 * - `ps` is the new PS context
 *
 * Returns 0 if all goes well, 1 otherwise.
 */
extern int open_PS_file(char         *name,
                        int           quiet,
                        PS_reader_p  *ps)
{
  int  f;

  if (name == NULL)
    f = STDIN_FILENO;
  else
  {
    f = open_binary_file(name,FALSE);
    if (f == -1) return 1;
  }
  return build_PS_reader(f,quiet,ps);
}

/*
 * Close a PS file, and free the reader context
 *
 * (Doesn't close the file if it was standard input)
 *
 * Returns 0 if all goes well, 1 otherwise.
 */
extern int close_PS_file(PS_reader_p   *ps)
{
  if ((*ps)->input != STDIN_FILENO)
  {
    int err = close_file((*ps)->input);
    if (err) return 1;
  }
  free_PS_reader(ps);
  return 0;
}

/*
 * Given a program stream, attempt to determine if it holds H.262 or H.264
 * data.
 *
 * Leaves the PS rewound to its "start".
 *
 * NOTE: It is probably better to use determine_PS_video_type().
 *
 * - `ps` is the program stream to check (assumed just to have been
 *   opened/built). This cannot be standard input, as it must be
 *   seekable.
 * - `is_h264` is the result
 *
 * Returns 0 if all goes well, 1 if there was an error (including the
 * stream not appearing to be either).
 */
extern int determine_if_PS_is_h264(PS_reader_p  ps,
                                   int         *is_h264)
{
  int  err;
  PES_reader_p  reader;

  // Try to decide what sort of data stream we have
  // The simplest (albeit clumsy) way to do this is to use technology
  // that already does this for us - i.e., build a temporary PES reader
  // around our file

  // It's then safe to build the temporary PES reader
  err = build_PS_PES_reader(ps,FALSE,FALSE,&reader);
  if (err)
  {
    print_err("### Error trying to determine PS stream type\n");
    return 1;
  }
  // Which knows the file type
  *is_h264 = reader->is_h264;
  (void) free_PES_reader(&reader);

  // And then make sure that our file position is where we think it should be
  err = rewind_program_stream(ps);
  if (err)
  {
    print_err("### Error rewinding PS file after determining its type\n");
    return 1;
  }
  return 0;
}

/*
 * Given a program stream, attempt to determine what type of video data it
 * contains.
 *
 * Leaves the PS rewound to its "start".
 *
 * - `ps` is the program stream to check (assumed just to have been
 *   opened/built). This cannot be standard input, as it must be
 *   seekable.
 * - `video_type` is the result. Calls determine_PES_video_type().
 *
 * Returns 0 if all goes well, 1 if there was an error (including the
 * stream not appearing to be either).
 */
extern int determine_PS_video_type(PS_reader_p  ps,
                                   int         *video_type)
{
  int  err;
  PES_reader_p  reader;

  // Try to decide what sort of data stream we have
  // The simplest (albeit clumsy) way to do this is to use technology
  // that already does this for us - i.e., build a temporary PES reader
  // around our file

  // It's then safe to build the temporary PES reader
  err = build_PS_PES_reader(ps,FALSE,FALSE,&reader);
  if (err)
  {
    print_err("### Error trying to determine PS stream type\n");
    return 1;
  }
  // Which thinks it knows the file type
  *video_type = reader->video_type;
  (void) free_PES_reader(&reader);

  // And then make sure that our file position is where we think it should be
  err = rewind_program_stream(ps);
  if (err)
  {
    print_err("### Error rewinding PS file after determining its type\n");
    return 1;
  }
  return 0;
}

/*
 * Seek within the PS file.
 *
 * Note that if the intent is to *rewind* to the start of the PS data,
 * then `rewind_program_stream` should be used instead, as offset 0 is
 * not necessarily the same as the start of the program stream.
 *
 * - `ps` is the PS read-ahead context
 * - `posn` is the file offset to seek to
 *
 * Return 0 if all goes well, 1 if something goes wrong
 */
extern int seek_using_PS_reader(PS_reader_p  ps,
                                offset_t     posn)
{
  int err = seek_file(ps->input,posn);
  if (err) return 1;

  ps->data_posn = posn;
  ps->data_len = 0;

  return get_more_data(ps);
}

/*
 * Rewind the PS context to the remembered "start of data"
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int rewind_program_stream(PS_reader_p  ps)
{
  return seek_using_PS_reader(ps,ps->start);
}

/*
 * Retrieve the next N bytes from the program stream, into an existing array.
 *
 * - `ps` is the PS read-ahead context
 * - `num_bytes` is how many bytes to read
 * - `buffer` is the buffer to read them into
 * - `posn` is the offset of said data in the file (NULL if the value is not
 *   wanted).
 *
 * Returns 0 if all goes well, EOF if end-of-file is encountered before all
 * of the bytes have been read, 1 if some other error occurred.
 */
static int read_PS_bytes(PS_reader_p ps,
                         int         num_bytes,
                         byte       *buffer,
                         offset_t   *posn)
{
  int  err;
  int  offset = 0;
  int  num_bytes_wanted = num_bytes;
  int  num_bytes_left = ps->data_end - ps->data_ptr;

  if (posn != NULL)
    *posn = ps->data_posn + (ps->data_ptr - ps->data);

  for (;;)
  {
    if (num_bytes_left < num_bytes_wanted)
    {
      memcpy(&(buffer[offset]),ps->data_ptr,num_bytes_left);
      offset += num_bytes_left;
      num_bytes_wanted -= num_bytes_left;
      err = get_more_data(ps);
      if (err) return err;
      num_bytes_left = ps->data_len;
    }
    else
    {
      memcpy(&(buffer[offset]),ps->data_ptr,num_bytes_wanted);
      ps->data_ptr += num_bytes_wanted;
      break;
    }
  }
  return 0;
}

// ============================================================
// Primitives
// ============================================================
/*
 * Print out a stream id in a manner consistent with the PS usages
 * of the stream id values.
 */
extern void print_stream_id(int    is_msg,
                            byte   stream_id)
{
  byte  number;
  char *str = NULL;
  switch (stream_id)
  {
    // H.222 Program stream specific codes
  case 0xB9: str = "PS MPEG_program_end_code"; break;
  case 0xBA: str = "PS Pack header start code"; break;
  case 0xBB: str = "PS System header start code"; break;
  case 0xBC: str = "PS Program stream map"; break;
  case 0xFF: str = "PS Program stream directory"; break;

    // Other "simple" values from H.222 Table 2-18, page 32
  case 0xBD: str = "Private stream 1"; break;
  case 0xBE: str = "Padding stream"; break;
  case 0xBF: str = "Private stream 2"; break;
  case 0xF0: str = "ECM stream"; break;
  case 0xF1: str = "EMM stream"; break;
  case 0xF2: str = "DSMCC stream"; break;
  case 0xF3: str = "13522 stream"; break;
  case 0xF4: str = "H.222.1 A stream"; break;
  case 0xF5: str = "H.222.1 B stream"; break;
  case 0xF6: str = "H.222.1 C stream"; break;
  case 0xF7: str = "H.222.1 D stream"; break;
  case 0xF8: str = "H.222.1 E stream"; break;
  case 0xF9: str = "Ancillary stream"; break;

  case 0x00: str = "H.262 Picture"; break;
  case 0xB2: str = "H.262 User data"; break;
  case 0xB3: str = "H.262 Sequence header"; break;
  case 0xB4: str = "H.262 Sequence error"; break;
  case 0xB5: str = "H.262 Extension"; break;
  case 0xB7: str = "H.262 Sequence end"; break;
  case 0xB8: str = "H.262 Group start"; break;

  default: str = NULL; break;
  }

  if (str != NULL)
    fprint_msg_or_err(is_msg,str);
  else if (stream_id >= 0xC0 && stream_id <=0xDF)
  {
    number = stream_id & 0x1F;
    fprint_msg_or_err(is_msg,"Audio stream 0x%02X",number);
  }
  else if (stream_id >= 0xE0 && stream_id <= 0xEF)
  {
    number = stream_id & 0x0F;
    fprint_msg_or_err(is_msg,"Video stream 0x%X",number);
  }
  else if (stream_id >= 0xFC && stream_id <= 0xFE)
    fprint_msg_or_err(is_msg,"Reserved data stream");
  else
    fprint_msg_or_err(is_msg,"Unrecognised stream id");
}

/*
 * Look for the start (the first 4 bytes) of the next program stream packet.
 *
 * Assumes that (for some reason) alignment has been lost, and thus it is
 * necessary to scan forwards to find the next 00 00 01 prefix.
 *
 * Otherwise equivalent to a call of `read_PS_packet_start`.
 *
 * - `ps` is the PS read-ahead context we're reading from
 * - if `verbose`, then we want to explain what we're doing
 * - if `max` is non-zero, then it is the maximum number of bytes
 *   to scan before giving up.
 * - `posn` is the file offset of the start of the packet
 * - `stream_id` is the identifying byte, after the 00 00 01 prefix. Note
 *   that this is set correctly if MPEG_program_end_code was read, and is
 *   0 if an error occurred.
 *
 * Returns:
 *   * 0 if it succeeds,
 *   * EOF if EOF is read, or an MPEG_program_end_code is read, or
 *   * 1 if some error (including the first 3 bytes not being 00 00 01) occurs.
 */
extern int find_PS_packet_start(PS_reader_p ps,
                                int         verbose,
                                uint32_t    max,
                                offset_t   *posn,
                                byte       *stream_id)
{
  int      err;
  byte     prev1 = 0xff;
  byte     prev2 = 0xff;
  byte     prev3 = 0xff;
  uint32_t count = 0;

  *stream_id = 0;
  for (;;)
  {
    byte *ptr;
    for (ptr = ps->data_ptr; ptr < ps->data_end; ptr++)
    {
      if (prev3 == 0x00 && prev2 == 0x00 && prev1 == 0x01)
      {
        if (*ptr == 0xB9) // MPEG_program_end_code
        {
          if (verbose)
            print_msg("Stopping at MPEG_program_end_code\n");
          *stream_id = 0xB9;
          return EOF;
        }
        else
        {
          *stream_id = *ptr;
          *posn = ps->data_posn + (ptr - ps->data) - 3;
          ps->data_ptr = ptr + 1;
          return 0;
        }
      }
      if (max > 0)
      {
        count ++;
        if (count > max)
        {
          fprint_err("### No PS packet start found in %d bytes\n",max);
          return 1;
        }
      }
      prev3 = prev2;
      prev2 = prev1;
      prev1 = *ptr;
    }
    // We've run out of data - get some more
    err = get_more_data(ps);
    if (err) return err;
  }
}

/*
 * Look for the next PS pack header.
 *
 * Equivalent to calling `find_PS_packet_start` until `stream_id` is 0xBA
 * (in other words, equivalent to having read the pack header start with
 * `read_PS_packet_start`).
 *
 * If you want to call `read_PS_packet_start` to read this pack header start
 * in again, then call ``seek_using_PS_reader(ps,posn)`` to reposition ready
 * to read it.
 *
 * - `ps` is the PS read-ahead context we're reading from
 * - if `verbose`, then the 00 00 01 XX sequences found will be logged
 *   to stderr, indicating the progress of our search
 * - if `max` is non-zero, then it is the maximum number of bytes
 *   to scan before giving up.
 * - `posn` is the file offset of the start of the packet found
 *
 * Returns:
 *   * 0 if it succeeds,
 *   * EOF if EOF is read, or an MPEG_program_end_code is read, or
 *   * 1 if some error (including the first 3 bytes not being 00 00 01) occurs.
 */
extern int find_PS_pack_header_start(PS_reader_p ps,
                                     int         verbose,
                                     uint32_t    max,
                                     offset_t   *posn)
{
  int   err;
  byte  stream_id = 0;

  while (stream_id != 0xBA)
  {
    err = find_PS_packet_start(ps,verbose,max,posn,&stream_id);
    if (err)
    {
      print_err("### Error looking for PS pack header (0xBA)\n");
      return 1;
    }
    if (verbose)
    {
      fprint_err("    Found: stream id %02X at " OFFSET_T_FORMAT " (",
                 stream_id,*posn);
      print_stream_id(FALSE,stream_id);
      print_err(")\n");
    }
  }
  return 0;
}

/*
 * Read in (the rest of) a PS packet according to its length.
 *
 * Suitable for use reading PS PES packets and PS system header packets.
 *
 * NOTE that the `data` buffer in the `packet` is realloc'ed by this
 * function. It is thus important to ensure that the `packet` datastructure
 * contains a NULL pointer for said buffer before the first call of this
 * function.
 *
 * - `ps` is the PS read-ahead context we're reading from
 * - `stream_id` identifies what sort of packet it is
 * - `packet` is the packet we're reading the PES packet into.
 *
 * Returns 0 if it succeeds, EOF if it unexpectedly reads end-of-file, and 1
 * if some other error occurs. `packet->data` will be NULL if EOF is returned.
 */
extern int read_PS_packet_body(PS_reader_p  ps,
                               byte         stream_id,
                               PS_packet_p  packet)
{
  int     err;
  byte    buf[2];

  // First, the packet length
  err = read_PS_bytes(ps,2,buf,NULL);
  if (err)
  {
    fprint_err("### %s reading PS packet length\n",
               (err==EOF?"Unexpected end of file":"Error"));
    if (packet->data!=NULL) free(packet->data);
    packet->data = NULL;
    return err;
  }

  packet->packet_length = (buf[0] << 8) | buf[1];

#if DEBUG
  fprint_msg("Packet length %d\n",packet->packet_length);
#endif

  // Remember that the packet length is the length of data
  // *after* the packet length field. Also, it is only allowed
  // to be 0 within a Transport Stream, so it should never be 0 for us
  // - but let's check anyway
  if (packet->packet_length == 0)
  {
    print_err("### Packet has length 0 - not allowed in PS\n");
    if (packet->data!=NULL) free(packet->data);
    packet->data = NULL;
    return 1;
  }

  // Since we are, in general, expecting to write the packet out again
  // at some point, it is convenient to also store the leading bytes
#if 0   // XXX naughty stuff
  packet->data = realloc(packet->data,packet->packet_length + 6 + 10);
#else
  packet->data = realloc(packet->data,packet->packet_length + 6);
#endif
  if (packet->data == NULL)
  {
    print_err("### Unable to allocate PS packet data buffer\n");
    return 1;
  }
  packet->data_len = packet->packet_length + 6;

  // So let us reestablish said leading bytes
  packet->data[0] = 0;
  packet->data[1] = 0;
  packet->data[2] = 1;
  packet->data[3] = stream_id;
  packet->data[4] = buf[0];
  packet->data[5] = buf[1];

  // And now we can read in the rest of the packet's data
  err = read_PS_bytes(ps,packet->packet_length,&(packet->data[6]),NULL);
  if (err)
  {
    fprint_err("### %s reading rest of PS packet\n",
               (err==EOF?"Unexpected end of file":"Error"));
    if (packet->data!=NULL) free(packet->data);
    packet->data = NULL;
    return err;
  }

#if 0   // XXX naughty stuff - add some trailing zero bytes
  packet->data[packet->data_len + 0] = 0;
  packet->data[packet->data_len + 1] = 0;
  packet->data[packet->data_len + 2] = 0;
  packet->data[packet->data_len + 3] = 0;
  packet->data[packet->data_len + 4] = 0;
  packet->data[packet->data_len + 5] = 0;
  packet->data[packet->data_len + 6] = 0;
  packet->data[packet->data_len + 7] = 0;
  packet->data[packet->data_len + 8] = 0;
  packet->data[packet->data_len + 9] = 0;
  packet->packet_length += 10;
  packet->data_len += 10;
#endif
  return 0;
}

/*
 * Read in the body of the pack header (but *not* the system header packets
 * therein).
 *
 * - `ps` is the PS read-ahead context we're reading from
 * - `hdr` is the packet we've read
 *
 * Returns 0 if it succeeds, EOF if it unexpectedly reads end-of-file, and
 * 1 if some other error occurs.
 */
extern int read_PS_pack_header_body(PS_reader_p       ps,
                                    PS_pack_header_p  hdr)
{
  int   err;
  byte  dummy[8];  // a 3 bit length means no more than 7 stuffing bytes

  // Read just the first 8 bytes, in case it's an MPEG-1 pack header
  err = read_PS_bytes(ps,8,hdr->data,NULL);
  if (err)
  {
    fprint_err("### %s reading body of PS pack header\n",
               (err==EOF?"Unexpected end of file":"Error"));
    return err;
  }

  if ((hdr->data[0] & 0xF0) == 0x20)
  {
#if DEBUG
    print_msg("ISO/IEC 11171-1/MPEG-1 pack header\n");
    print_data(TRUE,"Pack header",hdr->data,8,8);
#endif
    hdr->pack_stuffing_length = 0;          // since it doesn't exist
    hdr->scr =
      (((uint64_t)(hdr->data[0] & 0x09)) << 29) |
      (((uint64_t) hdr->data[1]        ) << 22) |
      (((uint64_t)(hdr->data[2] & 0xFE)) << 14) |
      (((uint64_t) hdr->data[3]        ) <<  7) |
      (((uint64_t)(hdr->data[4] & 0xFE)) >>  1);
    hdr->program_mux_rate =
      (((uint32_t)(hdr->data[5] & 0x7F)) << 15) |
      (((uint32_t) hdr->data[6]        ) <<  7) |
      (((uint32_t)(hdr->data[7] & 0xFE)) >>  1);
    // In MPEG-1,   SCR = NINT(SysClockFreq * t[i]) % 2**33
    //              where SysClockFreq = 90,000 Hz
    //
    // In H.222.0,  SCR = SCRbase[i] * 300 + SCRext[i]
    //                  = (((SysClockFreq * t[i]) DIV 300) % 2**33) * 300 +
    //                    ((SysClockFreq * t[i]) DIV 1) % 300
    //              where SysClockFreq = 27,000,000 Hz
    // Fudge these to match H.222.0 in case anyone tries to use them later on
    hdr->scr = hdr->scr * 300;
    hdr->scr_base = hdr->scr / 300;
    hdr->scr_extn = 0;                  // i.e., hdr->scr % 300
  }
  else
  {
#if DEBUG
    print_msg("ISO/IEC 13818-1/H.222.0 pack header\n");
#endif
    err = read_PS_bytes(ps,2,&(hdr->data[8]),NULL);
    if (err)
    {
      fprint_err("### %s reading last 2 bytes of body of PS pack header\n",
                 (err==EOF?"Unexpected end of file":"Error"));
      return err;
    }
#if DEBUG
    print_data(TRUE,"Pack header",hdr->data,10,10);
#endif
    hdr->scr_base  =
      (((uint64_t)(hdr->data[0] & 0x38)) << 27) |
      (((uint64_t)(hdr->data[0] & 0x03)) << 28) |
      (((uint64_t) hdr->data[1]        ) << 20) |
      (((uint64_t)(hdr->data[2] & 0xF8)) << 12) |
      (((uint64_t)(hdr->data[2] & 0x03)) << 13) |
      (((uint64_t) hdr->data[3]        ) <<  5) |
      (((uint64_t)(hdr->data[4] & 0xF8)) >>  3);
    hdr->scr_extn =
      (((uint32_t)(hdr->data[4] & 0x03)) << 7) |
      (((uint32_t) hdr->data[5]        ) >> 1);
    hdr->scr = hdr->scr_base * 300 + hdr->scr_extn;
    hdr->program_mux_rate =
      (((uint32_t)hdr->data[6] << 14)) |
      (((uint32_t)hdr->data[7] <<  6)) |
      (((uint32_t)hdr->data[8] >>  2));
    hdr->pack_stuffing_length = hdr->data[9] & 0x07;
  }

#if DEBUG   // XXX
  fprint_msg("Pack header body: scr_base " LLU_FORMAT ", scr_extn %u, scr "
             LLU_FORMAT ", mux rate %u, stuffing %d\n",
             hdr->scr_base,hdr->scr_extn,hdr->scr,hdr->program_mux_rate,
             hdr->pack_stuffing_length);
#endif

  // And ignore that many stuffing bytes...
  if (hdr->pack_stuffing_length > 0)
  {
    err = read_PS_bytes(ps,hdr->pack_stuffing_length,dummy,NULL);
    if (err)
    {
      fprint_err("### %s reading PS pack header stuffing bytes\n",
                 (err==EOF?"Unexpected end of file":"Error"));
      return err;
    }
  }
  return 0;
}

/*
 * Clear the contents of a PS packet datastructure. Frees the internal
 * `data` array.
 */
extern void clear_PS_packet(PS_packet_p  packet)
{
  if (packet->data != NULL)
  {
    free(packet->data);
    packet->data = NULL;
    packet->data_len = 0;
  }
  packet->packet_length = 0;
}

/*
 * Tidy up and free a PS packet datastructure after we've finished with it.
 *
 * Empties the PS packet datastructure, frees it, and sets `unit` to NULL.
 *
 * If `unit` is already NULL, does nothing.
 */
extern void free_PS_packet(PS_packet_p  *packet)
{
  if (*packet == NULL)
    return;
  clear_PS_packet(*packet);
  free(*packet);
  *packet = NULL;
}

/*
 * Read in the start (the first 4 bytes) of the next program stream packet.
 *
 * If the bytes read don't appear to be valid (i.e., they do not start with
 * the 00 00 01 prefix), then the next pack header will be sought and read in.
 *
 * Note that sequences of 00 bytes before the 00 00 01 will be ignored.
 *
 * - `ps` is the PS read-ahead context we're reading from
 * - if `verbose`, then we want to explain what we're doing
 * - `posn` is the file offset of the start of the packet
 * - `stream_id` is the identifying byte, after the 00 00 01 prefix. Note
 *   that this is set correctly if MPEG_program_end_code was read, and is
 *   0 if an error occurred.
 *
 * Returns:
 *   * 0 if it succeeds,
 *   * EOF if EOF is read, or an MPEG_program_end_code is read,
 *   * 2 if the bytes read are not 00 00 01 `stream_id`, or
 *   * 1 if some other error occurs.
 */
extern int read_PS_packet_start(PS_reader_p ps,
                                int         verbose,
                                offset_t   *posn,
                                byte       *stream_id)
{
  int   err;
  byte  buf[4];

  *stream_id = 0;

  err = read_PS_bytes(ps,4,buf,posn);
  if (err == EOF)
    return EOF;
  else if (err)
  {
    print_err("### Error reading start of PS packet\n");
    return 1;
  }

  // It's not uncommon to get a sequence of 00 bytes between packs
  // (in particular, after an audio pack). We don't really want to grumble
  // about such things, but just to cope
  if (buf[0] == 0 && buf[1] == 0 && buf[2] == 0)
  {
#if 0   // XXX
    fprint_msg("// %02x %02x %02x %02x\n",buf[0],buf[1],buf[2],buf[3]);
#endif
    while (buf[2] == 0)     // we already know buf[0] and buf[1] are zero
    {
      buf[2] = buf[3];
      err = read_PS_bytes(ps,1,&(buf[3]),posn);
      if (err == EOF)
        return EOF;
      else if (err)
      {
        print_err("### Error skipping 00 bytes before start of PS packet\n");
        return 1;
      }
    }
#if 0   // XXX
    fprint_msg("\\\\ %02x %02x %02x %02x\n",buf[0],buf[1],buf[2],buf[3]);
#endif
  }

  if (buf[0] != 0 || buf[1] != 0 || buf[2] != 1)
  {
    fprint_err("!!! PS packet at " OFFSET_T_FORMAT " should start "
               "00 00 01, but instead found %02X %02X %02X\n",
               *posn,buf[0],buf[1],buf[2]);
#if RECOVER_BROKEN_PS
    print_err("!!! Attempting to find next PS pack header\n");
    err = find_PS_pack_header_start(ps,TRUE,0,posn);
    if (err == EOF)
      return EOF;
    else if (err)
    {
      print_err("### Error trying to find start of next pack header\n");
      return 1;
    }
    fprint_err("!!! Continuing with PS pack header at " OFFSET_T_FORMAT
               "\n",*posn);
    *stream_id = 0xBA;
    return 0;
#else
    return 2;
#endif
  }

  *stream_id = buf[3];

#if DEBUG
  fprint_msg("Packet at " OFFSET_T_FORMAT ", stream id %02X (",*posn,*stream_id);
  print_stream_id(TRUE,*stream_id);
  print_msg(")\n");
#endif

  if (buf[3] == 0xB9)  // MPEG_program_end_code
  {
    if (verbose)
      print_msg("Stopping at MPEG_program_end_code\n");
    return EOF;
  }
  else
    return 0;
}

/* Determine the details of an AC3 stream, by looking at its start
 *
 * Naughtily assume that `data` is long enough...
 */
static inline void determine_ac3_details(byte *data, int verbose,
                                         byte *bsmod, byte *acmod)
{
  // The end of the syncinfo
  int fscod       = (data[4] & 0xC0) >> 6;
  int frmsizecode = (data[4] & 0x3F);
  // The start of the bit stream info
  int bsid        = (data[5] & 0xF8) >> 3;
  *bsmod          = (data[5] & 0x07);
  *acmod          = (data[6] & 0xC0) >> 6;
  if (verbose)
  {
    fprint_msg("    fscod       %x (sample rate %skHz)\n",fscod,
               (fscod==0?"48":fscod==1?"44.1":fscod==2?"32":"??"));
    fprint_msg("    frmsizecode %x\n",frmsizecode);
    fprint_msg("    bsid        %x (%s)\n",bsid,
               (bsid==8?"standard":bsid==6?"A52a alternate":
                bsid<8?"standard subset":"???"));
    fprint_msg("    bsmod       %x (%s)\n",*bsmod,BSMOD_STR(*bsmod,*acmod));
    fprint_msg("    acmod       %x (%s)\n",*acmod,ACMOD_STR(*acmod));
  }
}

/*
 * Inspect the given PS packet, and determine if it contains AC3 or DTS audio data.
 *
 * - `packet` is the packet's data, already established as private_data_1
 * - `is_dvd` is true if the data should be interpreted as DVD data
 * - if `verbose`, report on the details of what we find out
 * - `substream_index` returns the substream's index, taken from the low
 *   nibble of the substream id, and adjusted to start at 0. This will be
 *   a value in the range 0-7 for DTS, AC3 and LPCM, and in the range 0-1F
 *   (0-31) for subpictures.
 * - for AC3, `bsmod` and `acmod` return the appropriate quantities,
 *   otherwise they are 0.
 *
 * Returns one of the SUBSTREAM_* values.
 */
extern int identify_private1_data(struct PS_packet *packet,
                                  int               is_dvd,
                                  int               verbose,
                                  int              *substream_index,
                                  byte             *bsmod,
                                  byte             *acmod)
{
  // If this packet contains the start of a data packet, then
  // try to determine if it is AC-3
  int   PES_header_data_length = packet->data[6+2];
  int   what = SUBSTREAM_OTHER;
  byte *data;

#if 0  // Hmm - the data never does seem to have the pusi bit set
  if (verbose)
  {
    int   data_alignment_indicator = (packet->data[6] & 0x04);
    if (data_alignment_indicator)
      print_msg("*** Data aligned\n");
    else
      print_msg("--- Data not aligned\n");
    fprint_msg("*** PES header data length 0x%x (%d)\n",
               PES_header_data_length,PES_header_data_length);
  }
#endif

  *substream_index = 0;
  *bsmod = 0;
  *acmod = 0;

  // This *should* be the start of the underlying packet
  // Note that PES_header_data_length should not be 0 for
  // non-video streams...
  data = packet->data + 6 + 3 + PES_header_data_length;

  // DVD data has some structure within private_stream_1
  if (is_dvd)
  {
    int substream_id, frame_count, offset;
    substream_id = data[0];
    frame_count  = data[1];
    offset       = (data[2] << 8) | data[3];
    if (0x20 <= substream_id && substream_id <= 0x3F)
    {
      what = SUBSTREAM_SUBPICTURES;
      *substream_index = substream_id - 0x20;
    }
    else if (0x80 <= substream_id && substream_id <= 0x87)
    {
      what = SUBSTREAM_AC3;
      *substream_index = substream_id - 0x80;
    }
    else if (0x88 <= substream_id && substream_id <= 0x8F)
    {
      what = SUBSTREAM_DTS;
      *substream_index = substream_id - 0x88;
    }
    else if (0xA0 <= substream_id && substream_id <= 0xA7)
    {
      what = SUBSTREAM_LPCM;
      *substream_index = substream_id - 0xA0;
    }
    if (verbose)
    {
      fprint_msg(">>> substream_id  %02x (%s index %d)\n",substream_id,
                 (what==SUBSTREAM_AC3?"AC3":
                  what==SUBSTREAM_DTS?"DTS":
                  what==SUBSTREAM_LPCM?"LPCM":
                  what==SUBSTREAM_SUBPICTURES?"subpictures":"???"),
                 *substream_index);
      fprint_msg(">>> frame_count   %02x (%d)\n",frame_count,frame_count);
      fprint_msg(">>> offset      %04x (%d)\n",offset,offset);
    }
    // For AC3 and DTS, it's easy to check that it *does* appear to be what it
    // says, so let's do so
    if (what == SUBSTREAM_AC3 || what == SUBSTREAM_DTS)
    {
      int   packet_length = (packet->data[4] << 8) | packet->data[5];
      byte *sub_data = data + 3 + offset;
      // Roughly check if the offset looks plausible
      // TODO: make this check more robust!
      if (sub_data >= packet->data + packet_length) // leave off the + 6
      {
        // Looks like it's a silly offset, so probably NOT what we want
        if (verbose)
          fprint_msg("*** expected %s, but data at %p is beyond"
                     " packet->end at %p\n",(what==SUBSTREAM_DTS?"DTS":"AC3"),
                     sub_data,packet->data+6+packet_length);
        what = SUBSTREAM_ERROR;  // we definitely mustn't try to interpret it!
      }
      else if (what == SUBSTREAM_AC3 &&
               !(sub_data[0] == 0x0B && sub_data[1] == 0x77))
      {
        fprint_msg("*** expected AC3 sync 0x0B77, but found 0x%02x%02x\n",
                   sub_data[0],sub_data[1]);
        what = SUBSTREAM_ERROR;
      }
      else if (what == SUBSTREAM_DTS &&
               !(sub_data[0] == 0x7F && sub_data[1] == 0xFE &&
                 sub_data[2] == 0x80 && sub_data[3] == 0x01))
      {
        fprint_msg("*** expected DTS sync 0x7FFE8001,"
                   " but found 0x%02x%02x%02x%02x\n",
                   sub_data[0],sub_data[1],sub_data[2],sub_data[3]);
        what = SUBSTREAM_ERROR;
      }
      if (what == SUBSTREAM_AC3)
        determine_ac3_details(sub_data,verbose,bsmod,acmod);
    }
  }
  else
  {
    // For non-DVD data, we have to decide for ourselves what we've got
    if (data[0] == 0x0B && data[1] == 0x77)
    {
      what = SUBSTREAM_AC3;
      determine_ac3_details(data,verbose,bsmod,acmod);
    }
    else if (data[0] == 0x7F && data[1] == 0xFE &&
             data[2] == 0x80 && data[3] == 0x01)
      what = SUBSTREAM_DTS;
  }
  if (verbose)
  {
    switch (what)
    {
    case SUBSTREAM_AC3:
      print_msg("*** Looks like AC3\n");
      break;
    case SUBSTREAM_DTS:
      print_msg("*** Looks like DTS\n");
      break;
    case SUBSTREAM_LPCM:
      print_msg("*** Looks like LPCM\n");
      break;
    case SUBSTREAM_SUBPICTURES:
      print_msg("*** Looks like sub-pictures\n");
      break;
    case SUBSTREAM_OTHER:
      fprint_msg("*** Other substream: %02x %02x %02x %02x\n",
                 data[0],data[1],data[2],data[3]);
      break;
    default:
      fprint_msg("*** Error recognising substream: %02x %02x %02x %02x\n",
                 data[0],data[1],data[2],data[3]);
      break;
    }
  }
  return what;
}

// ============================================================
// PS to TS functions
// ============================================================
/*
 * Write out a video packet
 *
 * - `output` is the transport stream we're writing to
 * - `header` is the data from the PS pack header, including its SCR data
 * - `stream_id` is the stream id of the PS packet we're writing
 * - `packet` is the PS packet itself
 * - `prog_data` is the programming information we're using
 * - `num_video_ignored` will be be updated if we ignore this video packet
 * - `num_video_written` will be updated if we don't
 * - if `verbose` then we want to output diagnostic information
 * - if `quiet` then we want to be as quiet as we can
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_video(TS_writer_p            output,
                       struct PS_pack_header *header,
                       byte                   stream_id,
                       struct PS_packet      *packet,
                       struct program_data   *prog_data,
                       int                   *num_video_ignored,
                       int                   *num_video_written,
                       int                    verbose,
                       int                    quiet)
{
  int  err;

  // Unless the user has requested a particular video stream, we want
  // to use the first we find...
  // (For DVD this should also be the only stream, since DVD only allows
  // one video stream, but that's not for us to check.)

  if (prog_data->video_stream == -1)
  {
    prog_data->video_stream = stream_id;
  }
  else if (stream_id != prog_data->video_stream)
  {
    static int ignored_stream[NUMBER_VIDEO_STREAMS] = {0};
    int this_stream = stream_id & 0x0F;
    if (!ignored_stream[this_stream])
    {
      ignored_stream[this_stream] = TRUE;
      if (!quiet)
        fprint_msg("Ignoring video stream 0x%x (%d)\n",this_stream,this_stream);
    }
    (*num_video_ignored) ++;
    return 0;
  }

  if (*num_video_written == 0)
  {
    if (!quiet)
      fprint_msg("Video: stream %d, PID 0x%03x, stream type 0x%02x\n"
                 "       %s\n",
                 stream_id & 0x0F,prog_data->video_pid,prog_data->video_type,
                 h222_stream_type_str(prog_data->video_type));

    err = add_stream_to_pmt(prog_data->pmt,prog_data->video_pid,
                            prog_data->video_type,0,NULL);
    if (err) return 1;
    prog_data->pmt->version_number ++;

    // And it makes sense to write out our (updated) program
    // information before we write out the first packet of
    // the new video (!)
    err = write_pat_and_pmt(output,
                            prog_data->transport_stream_id,
                            prog_data->prog_list,
                            prog_data->pmt_pid,
                            prog_data->pmt);
    if (err)
    {
      print_err("### Error writing TS program data before video packet\n");
      return 1;
    }
  }

  // This is our video stream - output it as such
  err = write_PES_as_TS_PES_packet(output,packet->data,packet->data_len,
                                   prog_data->video_pid,
                                   DEFAULT_VIDEO_STREAM_ID,
                                   TRUE,header->scr_base,header->scr_extn);
  if (err) return 1;

  (*num_video_written) ++;
  if (verbose)
  {
    print_msg("v");
    flush_msg();
  }
  return 0;
}

/*
 * Write out the data for our DVD private_stream_1 audio packet
 *
 * - `output` is the transport stream we're writing to
 * - `packet` is the PS packet itself
 * - `prog_data` is the programming information we're using
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_DVD_AC3_data(TS_writer_p          output,
                              struct PS_packet    *packet,
                              struct program_data *prog_data)
{
  // DVD private_stream_1 is packaged as substreams - we need to unpack
  // it before we output it
  // Basically, looking at the substream offset:
  //  0 means there is no first frame in this packet (i.e., the data
  //    is the middle/end of a frame
  //  1 means the data *starts* with the first frame (to which any
  //    PTS applies)
  //  N means that the frame to which the PTS applies starts at
  //    offset N-1
  int   PES_header_data_length = packet->data[6+2];
  byte *data = packet->data + 6 + 3 + PES_header_data_length;
  int   data_len = packet->data_len - 6 - 3 - PES_header_data_length;
  //int substream_id = data[0];
  int   frame_count  = data[1];  // frames starting in this packet
  int   offset       = (data[2] << 8) | data[3];

  // If there is no PTS in this packet, do we need to split, even
  // if the offset is > 1? I'll assume not...
  int     got_pts;
  uint64_t pts;
  int     PES_packet_length = (packet->data[4] << 8) | packet->data[5];

  int err = find_PTS_in_PES(packet->data,packet->data_len,&got_pts,&pts);
  if (err)
  {
    print_err("### Error looking for PTS in PES packet\n");
    return 1;
  }

#if DEBUG_AC3
  fprint_msg(".. frame_count=%d, offset=%4d, got_pts=%d ",
             frame_count,offset,got_pts);
  if (frame_count > 0 && offset > 0)
  {
    if (data[offset+3]==0x0B && data[offset+4]==0x77)
      print_msg("(frame is AC3)\n");
    else
      fprint_msg("(frame appears to start %02x %02x)\n",
                 data[offset+3],data[offset+4]);
  }
#endif

  if (frame_count == 0 || offset <= 1 || !got_pts)
  {
    // We can output the data from this packet "unsplit".
    // However, first we need to create a new packet that does not
    // contain the DVD substream header - this is most easily done
    // by (a) copying the audio data bytes "down" over the header
    // we want to lose, and (b) adjusting the various packet length
    // fields appropriately

#if DEBUG_AC3
    fprint_msg("move data down 4, leaving %4d\n",data_len-4);
#endif

    (void) memmove(data,data+4,data_len-4);   // 4 bytes of substream header

    // After copying, need to remember to adjust the packet length
    PES_packet_length -= 4;       // still 4 bytes of substream header
    packet->data[4] = (PES_packet_length & 0xFF00) >> 8;
    packet->data[5] = (PES_packet_length & 0x00FF);

    // And we then have something suitable for outputting as-is
    err = write_PES_as_TS_PES_packet(output,packet->data,packet->data_len-4,
                                     prog_data->audio_pid,
                                     PRIVATE1_AUDIO_STREAM_ID,
                                     FALSE,0,0);
    if (err) return 1;
  }
  else
  {
    // We need to output the data *before* the "first" packet
    // in a plain PES packet, by itself, and then output the
    // packet to which the PTS applies as a separate PES packet.

#if DEBUG_AC3
    fprint_msg("write first %4d bytes, then move data down %4d, leaving %4d\n",
               offset-1,3+offset,data_len-3-offset);
#endif

    // First, the part before the first packet...
    err = write_ES_as_TS_PES_packet(output,data+4,offset-1,
                                    prog_data->audio_pid,
                                    PRIVATE1_AUDIO_STREAM_ID);
    if (err) return 1;

    // And we can then do the "move the data down" trick
    // Remember that offset 1 means the first byte after the substream
    // header, which we would normally expect to be offset 0...
    (void) memmove(data,data+3+offset,data_len-3-offset);

    // After copying, need to remember to adjust the packet length
    PES_packet_length -= 3 + offset;
    packet->data[4] = (PES_packet_length & 0xFF00) >> 8;
    packet->data[5] = (PES_packet_length & 0x00FF);

    // And we then have something suitable for outputting as-is
    err = write_PES_as_TS_PES_packet(output,packet->data,
                                     packet->data_len-3-offset,
                                     prog_data->audio_pid,
                                     PRIVATE1_AUDIO_STREAM_ID,
                                     FALSE,0,0);
    if (err) return 1;
  }
  return 0;
}

/*
 * Write out an audio packet
 *
 * - `output` is the transport stream we're writing to
 * - `stream_id` is the stream id of the PS packet we're writing
 * - `packet` is the PS packet itself
 * - `prog_data` is the programming information we're using
 * - `num_audio_ignored` will be be updated if we ignore this audio packet
 * - `num_audio_written` will be updated if we don't
 * - if `verbose` then we want to output diagnostic information
 * - if `quiet` then we want to be as quiet as we can
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_audio(TS_writer_p            output,
                       byte                   stream_id,
                       struct PS_packet      *packet,
                       struct program_data   *prog_data,
                       int                   *num_audio_ignored,
                       int                   *num_audio_written,
                       int                    verbose,
                       int                    quiet)
{
  int  err;
  int  substream_index = -1;
  byte bsmod = 0xFF;
  byte asmod = 0xFF;
  int  is_h222_pes = IS_H222_PES(packet->data);

  // For MPEG audio, unless the user has requested a particular audio stream,
  // we want to use the first we find.
  // For AC3, we only want audio from the requested substream
  // (DVD allows up to 8 audio streams, but we're only outputting one.
  //
  // Note that we assume that any audio data using private stream 1 to
  // transmit DVD-style audio packets will also be using PES (H.222.0)
  // packeting - i.e., *not* using MPEG-1 packets. This allows the code
  // to "dissect" DVD audio packets to be simpler, and seems a reasonable
  // assumption.

  if (prog_data->audio_stream == -1)  // take the first audio stream we find...
  {
    if (stream_id == PRIVATE1_AUDIO_STREAM_ID && is_h222_pes)
    {
      // Find out what type of data this is
      int  what = identify_private1_data(packet,prog_data->is_dvd,verbose,
                                         &substream_index,&bsmod,&asmod);
      if (what != SUBSTREAM_AC3)
      {
        // We're not interested in it
        return 0;
      }
      prog_data->audio_stream = stream_id;
      prog_data->audio_substream = substream_index; // only meaningful for DVD
    }
    else                        // some other ("normal") audio stream
      prog_data->audio_stream = stream_id;
  }
  else if (stream_id != prog_data->audio_stream)
  {
    if (!quiet)
    {
      static int ignored_stream[NUMBER_AUDIO_STREAMS] = {0};
      int this_stream = stream_id & 0x1F;
      if (!ignored_stream[this_stream])
      {
        ignored_stream[this_stream] = TRUE;
        fprint_msg("Ignoring audio stream 0x%x (%d)\n",this_stream,this_stream);
      }
    }
    (*num_audio_ignored) ++;
    return 0;
  }
  else if (prog_data->is_dvd &&
           stream_id == PRIVATE1_AUDIO_STREAM_ID &&
           prog_data->audio_stream == PRIVATE1_AUDIO_STREAM_ID &&
           is_h222_pes)
  {
    // Check if this is the right substream
    int  what = identify_private1_data(packet,prog_data->is_dvd,verbose,
                                       &substream_index,&bsmod,&asmod);
    if (what != SUBSTREAM_AC3)
    {
      if (!quiet)
      {
#define MAX_IGNORED_NON_AC3  10 // report the first 10 substreams we're ignoring
        static uint32_t ignored_non_AC3[MAX_IGNORED_NON_AC3] = {0};
        uint32_t lookfor = (what << 16) | substream_index;
        int ii;
        for (ii = 0; ii < MAX_IGNORED_NON_AC3; ii++)
        {
          if (ignored_non_AC3[ii] == lookfor)
            break; // we've reported it before
          else if (ignored_non_AC3[ii] == 0)
          {
            // We've not reported it before, and have room to remember it
            ignored_non_AC3[ii] = lookfor;
            fprint_msg("Ignoring %sprivate_stream_1 substream 0x%x (%d)"
                       " containing %s\n",
                       (SUBSTREAM_IS_AUDIO(what)?"":"non-audio "),
                       substream_index,substream_index,SUBSTREAM_STR(what));
            break;
          }
        }
      }
      if (SUBSTREAM_IS_AUDIO(what))
        (*num_audio_ignored) ++;
      return 0;
    }
    else if (substream_index != prog_data->audio_substream)
    {
      if (!quiet)
      {
        static int ignored_ac3_substream[NUMBER_AC3_SUBSTREAMS] = {0};
        if (!ignored_ac3_substream[substream_index])
        {
          ignored_ac3_substream[substream_index] = TRUE;
          fprint_msg("Ignoring private_stream_1 substream 0x%x (%d) "
                     "containing AC3\n",substream_index,substream_index);
        }
      }
      (*num_audio_ignored) ++;
      return 0;
    }
  }

  if (*num_audio_written == 0)
  {
    byte audio_stream_type;
    if (stream_id == PRIVATE1_AUDIO_STREAM_ID)
    {
      if (prog_data->output_dolby_as_dvb)
        audio_stream_type = DVB_DOLBY_AUDIO_STREAM_TYPE;
      else
        audio_stream_type = ATSC_DOLBY_AUDIO_STREAM_TYPE;
    }
    else
      audio_stream_type = MPEG2_AUDIO_STREAM_TYPE;

    if (!quiet)
    {
      if (stream_id == PRIVATE1_AUDIO_STREAM_ID)
      {
        print_msg("Audio: private stream 1,");
        if (prog_data->is_dvd && is_h222_pes)
          fprint_msg(" substream %d,",prog_data->audio_substream);
        fprint_msg(" PID 0x%03x, AC-3 (Dolby)\n",prog_data->audio_pid);
        fprint_msg("       %s\n       audio coding mode %s\n",
                   BSMOD_STR(bsmod,asmod),ACMOD_STR(asmod));
      }
      else
      {
        fprint_msg("Audio: stream %d, PID 0x%03x, stream type 0x%02x = %s\n",
                   stream_id & 0x1F,prog_data->audio_pid,audio_stream_type,
                   h222_stream_type_str(audio_stream_type));
      }
    }

    if (audio_stream_type == DVB_DOLBY_AUDIO_STREAM_TYPE)
    {
      byte desc[] = {0x6A, 0x01, 0x00};
      int  desc_len = 3;
      err = add_stream_to_pmt(prog_data->pmt,prog_data->audio_pid,
                              audio_stream_type,desc_len,desc);
    }
    else if (audio_stream_type == ATSC_DOLBY_AUDIO_STREAM_TYPE)
    {
      byte desc[] = {0x05, 0x04, 0x41, 0x43, 0x2D, 0x33};
      int  desc_len = 6;
      err = add_stream_to_pmt(prog_data->pmt,prog_data->audio_pid,
                              audio_stream_type,desc_len,desc);
    }
    else
      err = add_stream_to_pmt(prog_data->pmt,prog_data->audio_pid,
                              audio_stream_type,0,NULL);
    if (err) return 1;
    prog_data->pmt->version_number ++;

    // And it makes sense to write out our (updated) program
    // information before we write out the first packet of
    // the new audio
    err = write_pat_and_pmt(output,
                            prog_data->transport_stream_id,
                            prog_data->prog_list,
                            prog_data->pmt_pid,
                            prog_data->pmt);
    if (err)
    {
      print_err("### Error writing TS program data before audio packet\n");
      return 1;
    }
  }

  if (prog_data->is_dvd && stream_id == PRIVATE1_AUDIO_STREAM_ID &&
      is_h222_pes)
  {
    // Unpack the DVD substreams before outputting them
    err = write_DVD_AC3_data(output,packet,prog_data);
    if (err) return 1;
  }
  else
  {
    err = write_PES_as_TS_PES_packet(output,packet->data,packet->data_len,
                                     prog_data->audio_pid,
                                     (prog_data->want_ac3?
                                      PRIVATE1_AUDIO_STREAM_ID:
                                      DEFAULT_AUDIO_STREAM_ID),
                                     FALSE,0,0);
    if (err) return 1;
  }

  (*num_audio_written) ++;
  if (verbose)
  {
    print_msg("a");
    flush_msg();
  }
  return 0;
}

/*
 * Read program stream and write transport stream
 *
 * - `ps` is the program stream
 * - `output` is the transport stream
 * - `pad_start` is the number of filler TS packets to start the output
 *   with.
 * - `program_repeat` is how often (after how many PS packs) to repeat
 *   the program information (PAT/PMT)
 * - `is_dvd` should be true if this input represents DVD data; i.e., with
 *   private_stream_1 used for AC-3/DTS/etc., and with substream headers
 *   therein.
 * - `video_stream` indicates which video stream we want - i.e., the stream
 *   with id 0xE0 + <video_stream>. -1 means the first encountered.
 * - `audio_stream` indicates which audio stream we want. If `want_ac3_audio`
 *   is false, then this will be the stream with id 0xC0 + <audio_stream>,
 *   or -1 for the first audio stream encountered.
 * - if `want_ac3_audio` is true, then if `is_dvd` is true, then we want
 *   audio from private_stream_1 (0xBD) with substream id <audio_stream>,
 *   otherwise we ignore `audio_stream` and assume that all data in
 *   private_stream_1 is the audio we want.
 * - `output_dolby_as_dvb` should be true if Dolby (AC-3) audio (if selected) should
 *   be output using the DVB stream type 0x06, false if using the ATSC stream
 *   type 0x81. This is ignored if the audio being output is not Dolby.
 * - `pmt_pid` is the PID of the PMT to write
 * - `pcr_pid` is the PID of the TS unit containing the PCR
 * - `video_pid` is the PID for the video we write
 * - `keep_audio` is true if the audio stream should be output, false if
 *   it should be ignored
 * - `audio_pid` is the PID for the audio we write
 * - if `max` is non-zero, then we want to stop reading after we've read
 *   `max` packs
 * - if `verbose` then we want to output diagnostic information
 * - if `quiet` then we want to be as quiet as we can
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int _ps_to_ts(PS_reader_p          ps,
                     TS_writer_p          output,
                     struct program_data *prog_data,
                     int                  pad_start,
                     int                  program_repeat,
                     int                  keep_audio,
                     int                  max,
                     int                  verbose,
                     int                  quiet)
{
  int      ii, err;
  offset_t posn = 0;  // The location in the input file of the current packet
  int   count = 0;    // Number of PS packets
  byte  stream_id;    // The packet's stream id
  int   end_of_file = FALSE;

  // Summary data
  int  num_packs = 0;
  int  num_audio_written = 0;
  int  num_video_written = 0;
  int  num_video_ignored = 0;
  int  num_audio_ignored = 0;

  struct PS_packet      packet = {0};
  struct PS_pack_header header = {0};

  // Start off our output with some null packets - this is in case the
  // reader needs some time to work out its byte alignment before it starts
  // looking for 0x47 bytes
  for (ii=0; ii<pad_start; ii++)
  {
    err = write_TS_null_packet(output);
    if (err) return 1;
  }

  if (!quiet)
    fprint_msg("Writing transport stream id 1, PMT PID 0x%02x, PCR PID 0x%02x\n",
               prog_data->pmt_pid,prog_data->pcr_pid);

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
  count ++;

  if (stream_id != 0xba)
  {
    print_err("### Program stream does not start with pack header\n");
    fprint_err("    First packet has stream id %02X (",stream_id);
    print_stream_id(FALSE,stream_id);
    print_err(")\n");
    return 1;
  }

  // But given that, we can now happily loop reading in packs

  // I *think* using this macro makes the code marginally more readable,
  // and it helps emphasise that the code *is* identical each time
#define READ_NEXT_PS_PACKET_START \
  err = read_PS_packet_start(ps,FALSE,&posn,&stream_id);      \
  if (err == EOF)                                             \
  {                                                           \
    end_of_file = TRUE;                                       \
    break;                                                    \
  }                                                           \
  else if (err)                                               \
    return 1;                                                 \
  count ++;


  for (;;)
  {
    int  num_system_headers = 0;

    if (max > 0 && num_packs >= max)
    {
      if (verbose)
        fprint_msg("Stopping after %d packs\n",num_packs);
      return 0;
    }

    num_packs ++;

    // Write out our program data every so often, to give the reader
    // a chance to resynchronise with our program stream
    if (num_packs % program_repeat == 0)
    {
      if (verbose)
      {
        print_msg("PGM");
        flush_msg();
      }
      err = write_pat_and_pmt(output,
                              prog_data->transport_stream_id,
                              prog_data->prog_list,
                              prog_data->pmt_pid,
                              prog_data->pmt);
      if (err)
      {
        print_err("### Error writing out TS program data\n");
        return 1;
      }
    }

    err = read_PS_pack_header_body(ps,&header);
    if (err)
    {
      fprint_err("### Error reading data for pack header starting at "
                 OFFSET_T_FORMAT "\n",posn);
      return 1;
    }

    // Look at the start of the next packet
    READ_NEXT_PS_PACKET_START;

    // If it's a system header, ignore it
    if (stream_id == 0xbb)
    {
      err = read_PS_packet_body(ps,stream_id,&packet);
      if (err)
      {
        fprint_err("### Error reading system header starting at "
                   OFFSET_T_FORMAT "\n",posn);
        return 1;
      }
      num_system_headers ++;

      READ_NEXT_PS_PACKET_START;
    }

    if (end_of_file)
      break;

    // Then read the data packets
    while (stream_id != 0xba)  // i.e., until the start of the next pack
    {
      err = read_PS_packet_body(ps,stream_id,&packet);
      if (err)
      {
        fprint_err("### Error reading PS packet starting at "
                   OFFSET_T_FORMAT "\n",posn);
        return 1;
      }

      if (IS_AUDIO_STREAM_ID(stream_id))
      {
        if (keep_audio)
        {
          err = write_audio(output,stream_id,&packet,prog_data,
                            &num_audio_ignored,&num_audio_written,
                            verbose,quiet);
          if (err)
          {
            fprint_err("### Error writing audio packet at "
                       OFFSET_T_FORMAT " to TS\n",posn);
            return 1;
          }
        }
      }
      else if (IS_VIDEO_STREAM_ID(stream_id))
      {
        err = write_video(output,&header,stream_id,&packet,prog_data,
                          &num_video_ignored,&num_video_written,verbose,quiet);
        if (err)
        {
          fprint_err("### Error writing video packet at " OFFSET_T_FORMAT
                     " to TS\n",posn);
          return 1;
        }
      }
      else if (verbose)
      {
        // For the moment, we ignore program stream map (0xBC) and
        // program stream directory (0xFF), and indeed everything else
      }

      READ_NEXT_PS_PACKET_START;
    }
    if (end_of_file)
      break;
  }

  clear_PS_packet(&packet);

  if (verbose) print_msg("\n");
  if (!quiet)
  {
    fprint_msg("Packets (total):            %6d\n",count);
    fprint_msg("Packs:                      %6d\n",num_packs);
    fprint_msg("Video packets written:      %6d\n",num_video_written);
    fprint_msg("Audio packets written:      %6d\n",num_audio_written);

    if (num_video_ignored > 0)
      fprint_msg("Video packets ignored:      %6d\n",num_video_ignored);
    if (num_audio_ignored > 0)
      fprint_msg("Audio packets ignored:      %6d\n",num_audio_ignored);
  }
  return 0;
}

/*
 * Read program stream and write transport stream
 *
 * - `ps` is the program stream
 * - `output` is the transport stream
 * - `pad_start` is the number of filler TS packets to start the output
 *   with.
 * - `program_repeat` is how often (after how many PS packs) to repeat
 *   the program information (PAT/PMT)
 * - `video_type` indicates what type of video is being transferred. It should
 *   be VIDEO_H264, VIDEO_H262, etc.
 * - `is_dvd` should be true if this input represents DVD data; i.e., with
 *   private_stream_1 used for AC-3/DTS/etc., and with substream headers
 *   therein.
 * - `video_stream` indicates which video stream we want - i.e., the stream
 *   with id 0xE0 + <video_stream>. -1 means the first encountered.
 * - `audio_stream` indicates which audio stream we want. If `want_ac3_audio`
 *   is false, then this will be the stream with id 0xC0 + <audio_stream>,
 *   or -1 for the first audio stream encountered.
 * - if `want_ac3_audio` is true, then if `is_dvd` is true, then we want
 *   audio from private_stream_1 (0xBD) with substream id <audio_stream>,
 *   otherwise we ignore `audio_stream` and assume that all data in
 *   private_stream_1 is the audio we want.
 * - `output_dolby_as_dvb` should be true if Dolby (AC-3) audio (if selected) should
 *   be output using the DVB stream type 0x06, false if using the ATSC stream
 *   type 0x81. This is ignored if the audio being output is not Dolby.
 * - `pmt_pid` is the PID of the PMT to write
 * - `pcr_pid` is the PID of the TS unit containing the PCR
 * - `video_pid` is the PID for the video we write
 * - `keep_audio` is true if the audio stream should be output, false if
 *   it should be ignored
 * - `audio_pid` is the PID for the audio we write
 * - if `max` is non-zero, then we want to stop reading after we've read
 *   `max` packs
 * - if `verbose` then we want to output diagnostic information
 * - if `quiet` then we want to be as quiet as we can
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int ps_to_ts(PS_reader_p  ps,
                    TS_writer_p  output,
                    int          pad_start,
                    int          program_repeat,
                    int          video_type,
                    int          is_dvd,
                    int          video_stream,
                    int          audio_stream,
                    int          want_ac3_audio,
                    int          output_dolby_as_dvb,
                    uint32_t     pmt_pid,
                    uint32_t     pcr_pid,
                    uint32_t     video_pid,
                    int          keep_audio,
                    uint32_t     audio_pid,
                    int          max,
                    int          verbose,
                    int          quiet)
{
  int     err;
  struct  program_data prog_data = {0};

  prog_data.transport_stream_id = 1;
  prog_data.program_number = 1;
  prog_data.pmt_pid = pmt_pid;
  prog_data.pcr_pid = pcr_pid;
  prog_data.video_pid = video_pid;
  prog_data.audio_pid = audio_pid;
  prog_data.video_type = video_type;
  prog_data.output_dolby_as_dvb = output_dolby_as_dvb;
  prog_data.video_stream = video_stream;
  prog_data.want_ac3 = want_ac3_audio;
  prog_data.is_dvd = is_dvd;
  if (want_ac3_audio)
  {
    prog_data.audio_stream = PRIVATE1_AUDIO_STREAM_ID;
    if (is_dvd)
      prog_data.audio_substream = audio_stream;
    else
      prog_data.audio_substream = -1;  // use the first we find
  }
  else
    prog_data.audio_stream = audio_stream;

  // We have one program - we'll make it program 1
#define PROGRAM_NUMBER  1
  err = build_pidint_list(&prog_data.prog_list);
  if (err) return 1;
  err = append_to_pidint_list(prog_data.prog_list,pmt_pid,PROGRAM_NUMBER);
  if (err)
  {
    free_pidint_list(&prog_data.prog_list);
    return 1;
  }
  prog_data.pmt = build_pmt(PROGRAM_NUMBER,0,pcr_pid);
  if (err)
  {
    free_pidint_list(&prog_data.prog_list);
    return 1;
  }

  err = _ps_to_ts(ps,output,&prog_data,pad_start,program_repeat,keep_audio,
                  max,verbose,quiet);
  if (err)
  {
    free_pidint_list(&prog_data.prog_list);
    free_pmt(&prog_data.pmt);
    return 1;
  }

  free_pidint_list(&prog_data.prog_list);
  free_pmt(&prog_data.pmt);
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

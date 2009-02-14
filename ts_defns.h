/*
 * Definitions for working with H.222 Transport Stream packets
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

#ifndef _ts_defns
#define _ts_defns

#include "compat.h"

// Transport Stream packets are always the same size
#define TS_PACKET_SIZE 188

// When we are putting data into a TS packet, we need the first four
// bytes for heading information, which means that we will have at most
// 184 bytes for our payload
#define MAX_TS_PAYLOAD_SIZE  (TS_PACKET_SIZE-4)

// ------------------------------------------------------------
// Support for PCR read-ahead buffering
// Basically, always ensure that we know have read both the
// previous and the next PCR, so we can calculate the actual
// PCR for each packet between.

// Let's guess for a maximum number of TS entries we're likely to need
// to be able to hold...
//
// XXX But whatever number we guess here will be too small for some
// XXX streams, or so big it's really quite over the top for most
// XXX (and more than I'd like). So maybe we should have something
// XXX that's likely to cope for most streams, and we should (ideally)
// XXX have a way for the user to set the size with a swich, but also
// XXX (perhaps) we should allow the reader to continue (using the last
// XXX calculated rate) if we can't read ahead? Or perhaps having the
// XXX switch is enough, for the nonce... Or maybe we should allow the
// XXX buffer to grow (on demand, within some sort of reason) if it
// XXX needs to.
#define PCR_READ_AHEAD_SIZE     20000          // a made-up number

struct _ts_pcr_buffer
{
  byte     TS_buffer[PCR_READ_AHEAD_SIZE][TS_PACKET_SIZE];
  // For convenience (since we'll already have calculated this once),
  // remember each packets PID
  uint32_t TS_buffer_pids[PCR_READ_AHEAD_SIZE];
  // And the PCR PID we're looking for (we have to assume that's fairly
  // static, or we couldn't do read-aheads and interpolations)
  uint32_t TS_buffer_pcr_pid;
  // The number of TS entries we've got therein, the *last* of which
  // has a PCR
  int      TS_buffer_len;
  // Which TS packet we should read next...
  int      TS_buffer_next;
  // The PCR of that last entry
  uint64_t TS_buffer_end_pcr;
  // And the PCR of the *previous* last entry
  uint64_t TS_buffer_prev_pcr;
  // From which, we can deduce the time per packet
  uint64_t TS_buffer_time_per_TS;
  // For diagnostic purposes, the sequence number of TS_buffer[0]
  // (and thus, of the overall read-ahead buffer) in the overall file
  int      TS_buffer_posn;
  // Did we read an EOF before finding a "second" PCR?
  // (perhaps we should instead call this "TS_playing_out", but that's
  // less directly named from how we set it)
  int      TS_had_EOF;
};
typedef struct _ts_pcr_buffer *TS_pcr_buffer_p;
#define SIZEOF_TS_PCR_BUFFER sizeof(struct _ts_pcr_buffer)

// ------------------------------------------------------------
// The number of TS packets to read ahead
#define TS_READ_AHEAD_COUNT 1024        // aim for multiple of block boundary -- used to be 50
// Thus the number of bytes to read ahead
#define TS_READ_AHEAD_BYTES  TS_READ_AHEAD_COUNT*TS_PACKET_SIZE

// A read-ahead buffer for reading TS packets.
//
// Note that `posn` always gives the file position of the *next* TS packet to
// be read from the file (so after reading a TS packet with
// `read_next_TS_packet`, the position of said packet is `posn`-TS_PACKET_SIZE)
struct _ts_reader
{
  int      file;            // the file to read from
  offset_t posn;            // the position of the next-to-be-read TS packet
  void *handle;             // handle to pass to read_fn and seek_fn.


  // Reader and seek functions. If these are non-NULL we call them
  //  when we would call read() or seek().
  int (*read_fn)(void *, byte *, size_t);
  int (*seek_fn)(void *, offset_t);

  byte     read_ahead[TS_READ_AHEAD_COUNT*TS_PACKET_SIZE];
  byte    *read_ahead_ptr;  // location of next packet in said array
  byte    *read_ahead_end;  // pointer just after the end of `read_ahead`

  // If we are doing PCR read-ahead (so we have exact PCR values for our
  // TS packets), then we also need:
  TS_pcr_buffer_p    pcrbuf;
};
typedef struct _ts_reader *TS_reader_p;
#define SIZEOF_TS_READER sizeof(struct _ts_reader)

#endif // _ts_defns

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

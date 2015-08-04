/*
 * Utilities for working with H.222 Transport Stream packets
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
#include <string.h>
#include <ctype.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "ts_fns.h"
#include "tswrite_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "pidint_fns.h"
#include "pes_fns.h"

#define DEBUG 0
#define DEBUG_DTS 0
#define DEBUG_WRITE_PACKETS 0

// Should we report reserved bits that are set to the wrong value?
// For the moment, make this a global, since it lets me suppress
// it easily. There should be some sort of command line switch
// to set this to FALSE in utilities that it matters for.
static int report_bad_reserved_bits = FALSE;


// ============================================================
// Suppport for the creation of Transport Streams.
// ============================================================

// Remember the continuity counter value for each/any PID
// (do I really want to have an array of size 8191+1?
//  and do I want it static? not if this ever becomes a
//  library module...)
static int continuity_counter[0x1fff+1] = {0};


/*
 * Return the next value of continuity_counter for the given pid
 */
static inline int next_continuity_count(uint32_t pid)
{
  uint32_t next = (continuity_counter[pid] + 1) & 0x0f;
  continuity_counter[pid] = next;
  return next;
}

/*
 * Create a PES header for our data.
 *
 * - `data_len` is the length of our ES data
 *   If this is too long to fit into 16 bits, then we will create a header
 *   with 0 as its length. Note this is only allowed (by the standard) for
 *   video data.
 * - `stream_id` is the elementary stream id to use (see H.222 Table 2-18).
 *   If the stream id indicates an audio stream (as elucidated by Note 2 in
 *   that same table), then the data_alignment_indicator flag will be set
 *   in the PES header - i.e., we assume that the audio frame *starts*
 *   (has its syncword) at the start of the PES packet payload.
 * - `with_PTS` should be TRUE if the PTS value in `pts` should be written
 *   to the PES header.
 * - `with_DTS` should be TRUE if the DTS value in `dts` should be written
 *   to the PES header. Note that if `with_DTS` is TRUE, then `with_PTS`
 *   must also be TRUE. If it is not, then the DTS value will be used for
 *   the PTS.
 * - `PES_hdr` is the resultant PES packet header, and
 * - `PES_hdr_len` its length (at the moment that's always the same, as
 *   we're not yet outputting any timing information (PTS/DTS), and so
 *   can get away with a minimal PES header).
 */
extern void PES_header(uint32_t  data_len,
                       byte      stream_id,
                       int       with_PTS,
                       uint64_t  pts,
                       int       with_DTS,
                       uint64_t  dts,
                       byte     *PES_hdr,
                       int      *PES_hdr_len)
{
  int  extra_len = 0;

  if (with_DTS && !with_PTS)
  {
    with_PTS = TRUE;
    pts = dts;
  }

  // If PTS=DTS then there is no point explictly coding the DTS so junk it
  if (with_DTS && pts == dts)
    with_DTS = FALSE;

  // packet_start_code_prefix
  PES_hdr[0] = 0x00;
  PES_hdr[1] = 0x00;
  PES_hdr[2] = 0x01;

  PES_hdr[3] = stream_id;

  // PES_packet_length comes next, but we'll actually sort it out
  // at the end, when we know what else we've put into our header

  // Flags: '10' then PES_scrambling_control .. original_or_copy
  // If it appears to be an audio stream, we set the data alignment indicator
  // flag, to indicate that the audio data starts with its syncword. For video
  // data, we leave the flag unset.
  if (IS_AUDIO_STREAM_ID(stream_id))
    PES_hdr[6] = 0x84;     // just data alignment indicator flag set
  else
    PES_hdr[6] = 0x80;     // no flags set

  // Flags: PTS_DTS_flags .. PES_extension_flag
  if (with_DTS && with_PTS)
    PES_hdr[7] = 0xC0;
  else if (with_PTS)
    PES_hdr[7] = 0x80;
  else
    PES_hdr[7] = 0x00;     // yet more unset flags (nb: no PTS/DTS info)

  // PES_header_data_length
  if (with_DTS && with_PTS)
  {
    PES_hdr[8] = 0x0A;
    encode_pts_dts(&(PES_hdr[9]),3,pts);
    encode_pts_dts(&(PES_hdr[14]),1,dts);
    *PES_hdr_len = 9 + 10;
    extra_len = 3 + 10; // 3 bytes after the length field, plus our PTS & DTS
  }
  else if (with_PTS)
  {
    PES_hdr[8] = 0x05;
    encode_pts_dts(&(PES_hdr[9]),2,pts);
    *PES_hdr_len = 9 + 5;
    extra_len = 3 + 5; // 3 bytes after the length field, plus our PTS
  }
  else
  {
    PES_hdr[8] = 0x00; // 0 means there is no more data
    *PES_hdr_len = 9;
    extra_len = 3; // just the basic 3 bytes after the length field
  }

  // So now we can set the length field itself...
  if (data_len > 0xFFFF || (data_len + extra_len) > 0xFFFF)
  {
    // If the length is too great, we just set it "unset"
    // @@@ (this should only really be done for TS-wrapped video, so perhaps
    //     we should complain if this is not video?)
    PES_hdr[4] = 0;
    PES_hdr[5] = 0;
  }
  else
  {
    // The packet length doesn't include the bytes up to and including the
    // packet length field, but it *does* include any bytes of the PES header
    // after it.
    data_len += extra_len;
    PES_hdr[4] = (byte) ((data_len & 0xFF00) >> 8);
    PES_hdr[5] = (byte) ((data_len & 0x00FF));
  }
}

/*
 * Write out our TS packet, as composed from its parts.
 *
 * - `output` is the TS writer context to write with
 * - `TS_packet` is the TS packet buffer, already filled to length `TS_hdr_len`
 *   with TS header information
 * - `pes_hdr` is the PES header data, length `pes_hdr_len`, which is
 *    written out thereafter
 * - `data` is then the actual ES data, length `data_len`
 *
 * When outputting asynchronously, the writer needs to know any timing
 * information that is available for the packet. Thus:
 *
 * - `pid` is the PID for the packet
 * - `got_pcr` is TRUE if we have a PCR for the packet, in which case
 * - `pcr` is that PCR.
 *
 * Restrictions
 * ============
 *
 * `TS_hdr_len` + `pes_hdr_len` + `data_len` should equal 188.
 *
 * `pes_hdr_len` may be 0 if there is no PES header data to be written
 * (i.e., this is not the start of the PES packet's data, or the packet
 * is not a PES packet).
 *
 * For real data, `data_len` should never be 0 (the exception is when
 * writing NULL packets).
 *
 * `TS_hdr_len` must never be 0.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
static int write_TS_packet_parts(TS_writer_p output,
                                 byte        TS_packet[TS_PACKET_SIZE],
                                 int         TS_hdr_len,
                                 byte        pes_hdr[],
                                 int         pes_hdr_len,
                                 byte        data[],
                                 int         data_len,
                                 uint32_t    pid,
                                 int         got_pcr,
                                 uint64_t    pcr)
{
  int err;
  int total_len  = TS_hdr_len + pes_hdr_len + data_len;

  if (total_len != TS_PACKET_SIZE)
  {
    fprint_err("### TS packet length is %d, not 188 (composed of %d + %d + %d)\n",
               total_len,TS_hdr_len,pes_hdr_len,data_len);
    return 1;
  }

  // We want to make a single write, so we need to assemble the package
  // into our packet buffer
  if (pes_hdr_len > 0)
    memcpy(&(TS_packet[TS_hdr_len]),pes_hdr,pes_hdr_len);

  if (data_len > 0)
    memcpy(&(TS_packet[TS_hdr_len+pes_hdr_len]),data,data_len);

  err = tswrite_write(output,TS_packet,pid,got_pcr,pcr);
  if (err)
  {
    fprint_err("### Error writing out TS packet: %s\n",strerror(errno));
    return 1;
  }
  return 0;
}

/*
 * Write our data as a (series of) Transport Stream PES packets.
 *
 * - `output` is the TS writer context we're using to write our TS data out
 * - `pes_hdr` is NULL if the data to be written out is already PES, and is
 *   otherwise a PES header constructed with PES_header()
 * - `pes_hdr_len` is the length of said PES header (or 0)
 * - `data` is (the remainder of) our ES data (e.g., a NAL unit) or PES packet
 * - `data_len` is its length
 * - `start` is true if this is the first time we've called this function
 *   to output (part of) this data (in other words, this should be true
 *   when someone else calls this function, and false when the function
 *   calls itself). This is expected to be TRUE if a PES header is given...
 * - `set_pusi` is TRUE if we should set the payload unit start indicator
 *   (generally true if `start` is TRUE). This is ignored if `start` is FALSE.
 * - `pid` is the PID to use for this TS packet
 * - `stream_id` is the PES packet stream id to use (e.g.,
 *    DEFAULT_VIDEO_STREAM_ID)
 * - `got_PCR` is TRUE if we have a `PCR` value (this is only
 *   relevant when `start` is also TRUE).
 * - `PCR_base` and `PCR_extn` then encode that PCR value (ditto)
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
static int write_some_TS_PES_packet(TS_writer_p  output,
                                    byte        *pes_hdr,
                                    int          pes_hdr_len,
                                    byte        *data,
                                    uint32_t     data_len,
                                    int          start,
                                    int          set_pusi,
                                    uint32_t     pid,
                                    byte         stream_id,
                                    int          got_PCR,
                                    uint64_t     PCR_base,
                                    uint32_t     PCR_extn)
{
#define DEBUG_THIS 0
  byte    TS_packet[TS_PACKET_SIZE];
  int     TS_hdr_len;
  uint32_t controls = 0;
  uint32_t pes_data_len = 0;
  int     err;
  int     got_adaptation_field = FALSE;
  uint32_t space_left;  // Bytes available for payload, after the TS header

  if (pid < 0x0010 || pid > 0x1ffe)
  {
    fprint_err("### PID %03x is outside legal program stream range",pid);
    return 1;
  }

  // If this is the first time we've "seen" this data, and it is not
  // already wrapped up as PES, then we need to remember its PES header
  // in our calculations
  if (pes_hdr)
    pes_data_len = data_len + pes_hdr_len;
  else
  {
    pes_hdr_len = 0;
    pes_data_len = data_len;
  }

#if DEBUG_THIS
  if (start)
    print_msg("TS_PES ");
  else
    print_msg("       ");
  print_data(TRUE,"",data,data_len,20);
#endif

  // We always start with a sync_byte to identify this as a
  // Transport Stream packet
  TS_packet[0] = 0x47;
  // Should we set the "payload_unit_start_indicator" bit?
  // Only for the first packet containing our data.
  if (start && set_pusi)
    TS_packet[1] = (byte)(0x40 | ((pid & 0x1f00) >> 8));
  else
    TS_packet[1] = (byte)(0x00 | ((pid & 0x1f00) >> 8));
  TS_packet[2] = (byte)(pid & 0xff);

  // Sort out the adaptation field, if any
  if (start && got_PCR)
  {
    // This is the start of the data, and we have a PCR value to output,
    // so we know we have an adaptation field
    controls = 0x30;  // adaptation field control = '11' = both
    TS_packet[3] = (byte) (controls | next_continuity_count(pid));
    // And construct said adaptation field...
    TS_packet[4]  = 7; // initial adaptation field length
    TS_packet[5]  = 0x10;  // flag bits 0001 0000 -> got PCR
    TS_packet[6]  = (byte)   (PCR_base >> 25);
    TS_packet[7]  = (byte)  ((PCR_base >> 17) & 0xFF);
    TS_packet[8]  = (byte)  ((PCR_base >>  9) & 0xFF);
    TS_packet[9]  = (byte)  ((PCR_base >>  1) & 0xFF);
    TS_packet[10] = (byte) (((PCR_base & 0x1) << 7) | 0x7E | (PCR_extn >> 8));
    TS_packet[11] = (byte)  (PCR_extn >>  1);
    TS_hdr_len = 12;
    space_left = MAX_TS_PAYLOAD_SIZE - 8;
    got_adaptation_field = TRUE;
#if DEBUG_THIS
    fprint_msg("       start & got_PCR -> with adaptation field, space left %d, TS_packet[4] %d\n",space_left,TS_packet[4]);
#endif
  }
  else if (pes_data_len < MAX_TS_PAYLOAD_SIZE)
  {
    // Our data is less than 184 bytes long, which means it won't fill
    // the payload, so we need to pad it out with an (empty) adaptation
    // field, padded to the appropriate length
    controls = 0x30;  // adaptation field control = '11' = both
    TS_packet[3] = (byte)(controls | next_continuity_count(pid));
    if (pes_data_len == (MAX_TS_PAYLOAD_SIZE - 1))  // i.e., 183
    {
      TS_packet[4] = 0; // just the length used to pad
      TS_hdr_len = 5;
      space_left = MAX_TS_PAYLOAD_SIZE - 1;
    }
    else
    {
      TS_packet[4] = 1; // initial length
      TS_packet[5] = 0;  // unset flag bits
      TS_hdr_len = 6;
      space_left = MAX_TS_PAYLOAD_SIZE - 2;  // i.e., 182
    }
    got_adaptation_field = TRUE;
#if DEBUG_THIS
    fprint_msg("       <184, pad with empty adaptation field, space left %d, TS_packet[4] %d\n",space_left,TS_packet[4]);
#endif
  }
  else
  {
    // The data either fits exactly, or is too long and will need to be
    // continued in further TS packets. In either case, we don't need an
    // adaptation field
    controls = 0x10;  // adaptation field control = '01' = payload only
    TS_packet[3] = (byte)(controls | next_continuity_count(pid));
    TS_hdr_len = 4;
    space_left = MAX_TS_PAYLOAD_SIZE;
#if DEBUG_THIS
    fprint_msg("       >=184, space left %d\n",space_left);
#endif
  }

  if (got_adaptation_field)
  {
    // Do we need to add stuffing bytes to allow for short PES data?
    if (pes_data_len < space_left)
    {
      int ii;
      int padlen = space_left - pes_data_len;
      for (ii = 0; ii < padlen; ii++)
        TS_packet[TS_hdr_len+ii] = 0xFF;
      TS_packet[4] += padlen;
      TS_hdr_len   += padlen;
      space_left   -= padlen;
#if DEBUG_THIS
      fprint_msg("       stuffing %d, space left %d, TS_packet[4] %d\n",padlen,space_left,TS_packet[4]);
#endif
    }
  }
  
  if (pes_data_len == space_left)
  {
#if DEBUG_THIS
    print_msg("       == fits exactly\n");
#endif
    // Our data fits exactly
    err = write_TS_packet_parts(output,
                                TS_packet,TS_hdr_len,
                                pes_hdr,pes_hdr_len,
                                data,data_len,
                                pid,got_PCR,(PCR_base*300)+PCR_extn);
    if (err) return err;
  }
  else
  {
    // We need to look at more than one packet...
    // Write out the first 184-pes_hdr_len bytes
    int increment = space_left - pes_hdr_len;
    err = write_TS_packet_parts(output,
                                TS_packet,TS_hdr_len,
                                pes_hdr,pes_hdr_len,
                                data,increment,
                                pid,got_PCR,(PCR_base*300)+PCR_extn);
    if (err) return err;
#if DEBUG_THIS
    fprint_msg("       == wrote %d, leaving %d\n",increment,data_len-increment);
#endif
    // Leaving data_len - (184-pes_hdr_len) bytes still to go
    // Is recursion going to be efficient enough?
    if ((data_len - increment) > 0)
    {
      err = write_some_TS_PES_packet(output,NULL,0,
                                     &(data[increment]),data_len-increment,
                                     FALSE,FALSE,pid,stream_id,FALSE,0,0);
      if (err) return err;
    }
  }
  return 0;
}

/*
 * Write out our ES data as a Transport Stream PES packet.
 *
 * - `output` is the TS output context returned by `tswrite_open`
 * - `data` is our ES data (e.g., a NAL unit)
 * - `data_len` is its length
 * - `pid` is the PID to use for this TS packet
 * - `stream_id` is the PES packet stream id to use (e.g.,
 *    DEFAULT_VIDEO_STREAM_ID)
 *
 * If the data to be written is more than 65535 bytes long (i.e., the
 * length will not fit into 2 bytes), then the PES packet written will
 * have PES_packet_length set to zero (see ISO/IEC 13818-1 (H.222.0)
 * 2.4.3.7, Semantic definitions of fields in PES packet). This is only
 * allowed for video streams.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_ES_as_TS_PES_packet(TS_writer_p output,
                                     byte        data[],
                                     uint32_t    data_len,
                                     uint32_t    pid,
                                     byte        stream_id)
{
  byte    pes_hdr[TS_PACKET_SIZE];  // better be more than long enough!
  int     pes_hdr_len = 0;

#if DEBUG_WRITE_PACKETS
  fprint_msg("||  ES as TS/PES, pid %x (%d)\n",pid,pid);
#endif
  
  PES_header(data_len,stream_id,FALSE,0,FALSE,0,pes_hdr,&pes_hdr_len);

  return write_some_TS_PES_packet(output,pes_hdr,pes_hdr_len,
                                  data,data_len,TRUE,TRUE,pid,stream_id,
                                  FALSE,0,0);
}

/*
 * Write out our ES data as a Transport Stream PES packet, with PTS and/or DTS
 * if we've got them, and some attempt to write out a sensible PCR.
 *
 * - `output` is the TS output context returned by `tswrite_open`
 * - `data` is our ES data (e.g., a NAL unit)
 * - `data_len` is its length
 * - `pid` is the PID to use for this TS packet
 * - `stream_id` is the PES packet stream id to use (e.g.,
 *    DEFAULT_VIDEO_STREAM_ID)
 * - `got_pts` is TRUE if we have a PTS value, in which case
 * - `pts` is said PTS value
 * - `got_dts` is TRUE if we also have DTS, in which case
 * - `dts` is said DTS value.
 *
 * We also want to try to write out a sensible PCR value.
 *
 * PTS can go up as well as down (it is the time at which the next frame
 * should be presented to the user, but frames do not necessarily occur
 * in presentation order).
 *
 * DTS only goes up, since it is the time that the frame should be decoded.
 *
 * Thus, if we have it, the DTS is sensible to use for the PCR...
 *
 * If the data to be written is more than 65535 bytes long (i.e., the
 * length will not fit into 2 bytes), then the PES packet written will
 * have PES_packet_length set to zero (see ISO/IEC 13818-1 (H.222.0)
 * 2.4.3.7, Semantic definitions of fields in PES packet). This is only
 * allowed for video streams.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_ES_as_TS_PES_packet_with_pts_dts(TS_writer_p output,
                                                  byte        data[],
                                                  uint32_t    data_len,
                                                  uint32_t    pid,
                                                  byte        stream_id,
                                                  int         got_pts,
                                                  uint64_t    pts,
                                                  int         got_dts,
                                                  uint64_t    dts)
{
  byte    pes_hdr[TS_PACKET_SIZE];  // better be more than long enough!
  int     pes_hdr_len = 0;

#if DEBUG_WRITE_PACKETS
  fprint_msg("||  ES as TS/PES with PTS/DTS, pid %x (%d)\n",pid,pid);
#endif

  PES_header(data_len,stream_id,got_pts,pts,got_dts,dts,pes_hdr,&pes_hdr_len);

  return write_some_TS_PES_packet(output,pes_hdr,pes_hdr_len,
                                  data,data_len,TRUE,TRUE,pid,stream_id,
                                  got_dts,dts,0);
}

/*
 * Write out our ES data as a Transport Stream PES packet, with PCR.
 *
 * - `output` is the TS output context returned by `tswrite_open`
 * - `data` is our ES data (e.g., a NAL unit)
 * - `data_len` is its length
 * - `pid` is the PID to use for this TS packet
 * - `stream_id` is the PES packet stream id to use (e.g.,
 *    DEFAULT_VIDEO_STREAM_ID)
 * - `pcr_base` and `pcr_extn` encode the PCR value.
 *
 * If the data to be written is more than 65535 bytes long (i.e., the
 * length will not fit into 2 bytes), then the PES packet written will
 * have PES_packet_length set to zero (see ISO/IEC 13818-1 (H.222.0)
 * 2.4.3.7, Semantic definitions of fields in PES packet). This is only
 * allowed for video streams.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_ES_as_TS_PES_packet_with_pcr(TS_writer_p output,
                                              byte        data[],
                                              uint32_t    data_len,
                                              uint32_t    pid,
                                              byte        stream_id,
                                              uint64_t    pcr_base,
                                              uint32_t    pcr_extn)
{
  byte    pes_hdr[TS_PACKET_SIZE];  // better be more than long enough!
  int     pes_hdr_len = 0;

#if DEBUG_WRITE_PACKETS
  fprint_msg("||  ES as TS/PES with PCR, pid %x (%d)\n",pid,pid);
#endif

  PES_header(data_len,stream_id,FALSE,0,FALSE,0,pes_hdr,&pes_hdr_len);

  return write_some_TS_PES_packet(output,pes_hdr,pes_hdr_len,
                                  data,data_len,TRUE,TRUE,pid,stream_id,
                                  TRUE,pcr_base,pcr_extn);
}

/*
 * Write out a PES packet's data as a Transport Stream PES packet.
 *
 * - `output` is the TS output context returned by `tswrite_open`
 * - `data` is our PES data (e.g., a program stream video data packet)
 * - `data_len` is its length
 * - `pid` is the PID to use for this TS packet
 * - `stream_id` is the PES packet stream id to use (e.g.,
 *    DEFAULT_VIDEO_STREAM_ID)
 * - `got_pcr` is TRUE if we have values for the PCR in this packet,
 *   in which case `pcr_base` and `pcr_extn` are the parts of the PCR.
 *
 * If the data to be written is more than 65535 bytes long (i.e., the
 * length will not fit into 2 bytes), then the PES packet written will
 * have PES_packet_length set to zero (see ISO/IEC 13818-1 (H.222.0)
 * 2.4.3.7, Semantic definitions of fields in PES packet). This is only
 * allowed for video streams.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_PES_as_TS_PES_packet(TS_writer_p output,
                                      byte        data[],
                                      uint32_t    data_len,
                                      uint32_t    pid,
                                      byte        stream_id,
                                      int         got_pcr,
                                      uint64_t    pcr_base,
                                      uint32_t    pcr_extn)
{
// Should we write MPEG-1 packet data out as ES (wrapped in MPEG-2 PES in TS),
// rather than writing the packets out directly in TS? (that latter doesn't
// work very well, as TS is not defined to work for MPEG-1 style packets).
#define MPEG1_AS_ES       1

#if DEBUG_WRITE_PACKETS
  fprint_msg("|| PES as TS/PES, pid %x (%d)\n",pid,pid);
#endif

#if 0   // XXX
  print_data(TRUE,"TS_PES",data,data_len,20);
  print_end_of_data("      ",data,data_len,20);
#endif  // XXX

#if MPEG1_AS_ES
  if (IS_H222_PES(data))
  {
#endif  // MPEG1_AS_ES
    return write_some_TS_PES_packet(output,NULL,0,
                                    data,data_len,TRUE,TRUE,pid,stream_id,
                                    got_pcr,pcr_base,pcr_extn);
#if MPEG1_AS_ES
  }
  else
  {
    // Write MPEG-1 data out as ES in (MPEG-2) PES
    int     got_pts, got_dts;
    uint64_t pts, dts;
    int     offset = calc_mpeg1_pes_offset(data,data_len);
    int     err = find_PTS_DTS_in_PES(data,data_len,
                                      &got_pts,&pts,&got_dts,&dts);
    if (err)   // Just try to carry on...
    {
      got_pts = FALSE;
      got_dts = FALSE;
    }
    return write_ES_as_TS_PES_packet_with_pts_dts(output,
                                                  data + offset,
                                                  data_len - offset,
                                                  pid,
                                                  stream_id,
                                                  got_pts,pts,
                                                  got_dts,dts);
  }
#endif  // MPEG1_AS_ES
}

/*
 * Construct a Transport Stream packet header for PAT or PMT data.
 *
 * The data is required to fit within a single TS packet - i.e., to be
 * 183 bytes or less.
 *
 * - `pid` is the PID to use for this packet.
 * - `data_len` is the length of the PAT or PMT data
 * - `TS_hdr` is a byte array into (the start of) which to write the
 *   TS header data.
 * - `TS_hdr_len` returns how much data we've written therein.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
static int TS_program_packet_hdr(uint32_t pid,
                                 int      data_len,
                                 byte     TS_hdr[TS_PACKET_SIZE],
                                 int     *TS_hdr_len)
{
  uint32_t controls = 0;
  int     pointer, ii;

  if (data_len > (TS_PACKET_SIZE - 5))  // i.e., 183
  {
    fprint_err("### PMT/PAT data for PID %02x is too long (%d > 183)",
               pid,data_len);
    return 1;
  }
  
  // We always start with a sync_byte to identify this as a
  // Transport Stream packet

  TS_hdr[0] = 0x47;
  // We want the "payload_unit_start_indicator" bit set
  TS_hdr[1] = (byte)(0x40 | ((pid & 0x1f00) >> 8));
  TS_hdr[2] = (byte)(pid & 0xff);
  // We don't need any adaptation field controls
  controls = 0x10;
  TS_hdr[3] = (byte)(controls | next_continuity_count(pid));

  // Next comes a pointer to the actual payload data
  // (i.e., 0 if the data is 183 bytes long)
  // followed by pad bytes until we *get* to the data
  pointer = (byte)(TS_PACKET_SIZE - 5 - data_len);
  TS_hdr[4] = pointer;
  for (ii=0; ii<pointer; ii++)
    TS_hdr[5+ii] = 0xff;

  *TS_hdr_len = 5+pointer;
  return 0;
}

/*
 * Write out a Transport Stream PAT and PMT, for a single stream.
 * 
 * - `output` is the TS output context returned by `tswrite_open`
 * - `transport_stream_id` is the id for this particular transport stream.
 * - `program_number` is the program number to use for the PID.
 * - `pmt_pid` is the PID for the PMT.
 * - `pid` is the PID of the stream to enter in the tables. This is also
 *    used as the PCR PID.
 * - `stream_type` is the type of stream. MPEG-2 video is 0x01,
 *   MPEG-4/AVC (H.264) is 0x1b.
 *
 * Since we're outputting a TS representing a single ES, we only need to
 * support a single program stream, containing a single PID.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_TS_program_data(TS_writer_p output,
                                 uint32_t    transport_stream_id,
                                 uint32_t    program_number,
                                 uint32_t    pmt_pid,
                                 uint32_t    pid,
                                 byte        stream_type)
{
  int                   err;
  pidint_list_p         prog_list;
  pmt_p                 pmt;

  // We have a single program stream
  err = build_pidint_list(&prog_list);
  if (err) return 1;
  err = append_to_pidint_list(prog_list,pmt_pid,program_number);
  if (err)
  {
    free_pidint_list(&prog_list);
    return 1;
  }

  pmt = build_pmt((uint16_t)program_number,0,pid);  // Use program stream PID as PCR PID
  if (pmt == NULL)
  {
    free_pidint_list(&prog_list);
    return 1;
  }
  err = add_stream_to_pmt(pmt,pid,stream_type,0,NULL);
  if (err)
  {
    free_pidint_list(&prog_list);
    free_pmt(&pmt);
    return 1;
  }

  err = write_pat_and_pmt(output,transport_stream_id,prog_list,pmt_pid,pmt);
  if (err)
  {
    free_pidint_list(&prog_list);
    free_pmt(&pmt);
    return 1;
  }

  free_pidint_list(&prog_list);
  free_pmt(&pmt);
  return 0;
}

/*
 * Write out a Transport Stream PAT and PMT, for multiple streams.
 * 
 * - `output` is the TS output context returned by `tswrite_open`
 * - `transport_stream_id` is the id for this particular transport stream.
 * - `program_number` is the program number to use for the PMT PID.
 * - `pmt_pid` is the PID for the PMT.
 * - `pcr_pid` is the PID that contains the PCR.
 * - `num_progs` is how many program streams are to be defined.
 * - `prog_pid` is an array of audio/video PIDs
 * - `prog_type` is an array of the corresponding stream types
 *
 * Note that if `num_progs` is 0, `pcr_pid` is ignored.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_TS_program_data2(TS_writer_p output,
                                  uint32_t    transport_stream_id,
                                  uint32_t    program_number,
                                  uint32_t    pmt_pid,
                                  uint32_t    pcr_pid,
                                  int         num_progs,
                                  uint32_t    prog_pid[],
                                  byte        prog_type[])
{
  int                   err;
  int                   ii;
  pidint_list_p         prog_list;
  pmt_p                 pmt;

  // We have a single program stream
  err = build_pidint_list(&prog_list);
  if (err) return 1;
  err = append_to_pidint_list(prog_list,pmt_pid,program_number);
  if (err)
  {
    free_pidint_list(&prog_list);
    return 1;
  }

  pmt = build_pmt((uint16_t)program_number,0,pcr_pid);
  if (pmt == NULL)
  {
    free_pidint_list(&prog_list);
    return 1;
  }
  for (ii=0; ii<num_progs; ii++)
  {
    err = add_stream_to_pmt(pmt,prog_pid[ii],prog_type[ii],0,NULL);
    if (err)
    {
      free_pidint_list(&prog_list);
      free_pmt(&pmt);
      return 1;
    }
  }

  err = write_pat_and_pmt(output,transport_stream_id,prog_list,pmt_pid,pmt);
  if (err)
  {
    free_pidint_list(&prog_list);
    free_pmt(&pmt);
    return 1;
  }

  free_pidint_list(&prog_list);
  free_pmt(&pmt);
  return 0;
}

/*
 * Write out a Transport Stream PAT.
 * 
 * - `output` is the TS output context returned by `tswrite_open`
 * - `transport_stream_id` is the id for this particular transport stream.
 * - `prog_list` is a PIDINT list of program number / PID pairs.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_pat(TS_writer_p    output,
		     uint32_t       transport_stream_id,
		     pidint_list_p  prog_list)
{
  int      ii;
  byte     data[1021+3];
  byte     TS_packet[TS_PACKET_SIZE];
  int      TS_hdr_len;
  int      err;
  int      section_length;
  int      offset, data_length;
  uint32_t crc32;

#if DEBUG_WRITE_PACKETS
  print_msg("|| PAT pid 0\n");
#endif

  section_length = 9 + prog_list->length * 4;
  if (section_length > 1021)
  {
    print_err("### PAT data is too long - will not fit in 1021 bytes\n");
    // TODO: Ideally, would be to stderr
    report_pidint_list(prog_list,"Program list","Program",FALSE);
    return 1;
  }

  data[0] = 0x00;
  // The section length is fixed because our data is fixed
  data[1] = (byte) (0xb0 | ((section_length & 0x0F00) >> 8));
  data[2] = (byte) (section_length & 0x0FF);
  data[3] = (byte) ((transport_stream_id & 0xFF00) >> 8);
  data[4] = (byte)  (transport_stream_id & 0x00FF);
  // For simplicity, we'll have a version_id of 0
  data[5] = 0xc1;
  // First section of the PAT has section number 0, and there is only
  // that section
  data[6] = 0x00;
  data[7] = 0x00;

  offset = 8;
  for (ii = 0; ii < prog_list->length; ii++)
  {
    data[offset+0] = (byte) ((prog_list->number[ii] & 0xFF00) >> 8);
    data[offset+1] = (byte)  (prog_list->number[ii] & 0x00FF);
    data[offset+2] = (byte) (0xE0 | ((prog_list->pid[ii] & 0x1F00) >> 8));
    data[offset+3] = (byte) (prog_list->pid[ii] & 0x00FF);
    offset += 4;
  }

  crc32 = crc32_block(0xffffffff,data,offset);
  data[12] = (byte) ((crc32 & 0xff000000) >> 24);
  data[13] = (byte) ((crc32 & 0x00ff0000) >> 16);
  data[14] = (byte) ((crc32 & 0x0000ff00) >>  8);
  data[15] = (byte)  (crc32 & 0x000000ff);
  data_length = offset+4;

#if 1
  if (data_length != section_length + 3)
  {
    fprint_err("### PAT length %d, section length+3 %d\n",
               data_length,section_length+3);
    return 1;
  }
#endif

  crc32 = crc32_block(0xffffffff,data,data_length);
  if (crc32 != 0)
  {
    print_err("### PAT CRC does not self-cancel\n");
    return 1;
  }
  err = TS_program_packet_hdr(0x00,data_length,TS_packet,&TS_hdr_len);
  if (err)
  {
    print_err("### Error constructing PAT packet header\n");
    return 1;
  }
  err = write_TS_packet_parts(output,TS_packet,TS_hdr_len,NULL,0,
                              data,data_length,0x00,FALSE,0);
  if (err)
  {
    print_err("### Error writing PAT\n");
    return 1;
  }
  return 0;
}

/*
 * Write out a Transport Stream PMT, given a PMT datastructure
 * 
 * - `output` is the TS output context returned by `tswrite_open`
 * - `pmt_pid` is the PID for the PMT.
 * - 'pmt' is the datastructure containing the PMT information
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_pmt(TS_writer_p output,
		     uint32_t    pmt_pid,
		     pmt_p       pmt)
{
  int      ii;
  byte     data[3+1021];	// maximum PMT size
  byte     TS_packet[TS_PACKET_SIZE];
  int      TS_hdr_len;
  int      err;
  int      section_length;
  int      offset, data_length;
  uint32_t crc32;

#if DEBUG_WRITE_PACKETS
  fprint_msg("|| PMT pid %x (%d)\n",pmt_pid,pmt_pid);
#endif

  if (pmt_pid < 0x0010 || pmt_pid > 0x1ffe)
  {
    fprint_err("### PMT PID %03x is outside legal range\n",pmt_pid);
    return 1;
  }
  if (pid_in_pmt(pmt,pmt_pid))
  {
    fprint_err("### PMT PID and program %d PID are both %03x\n",
               pid_index_in_pmt(pmt,pmt_pid),pmt_pid);
    return 1;
  }

  // Much of the PMT should look very familiar, after the PAT

  // Calculate the length of the section
  section_length = 13 + pmt->program_info_length;
  for (ii = 0; ii < pmt->num_streams; ii++)
    section_length += 5 + pmt->streams[ii].ES_info_length;
  if (section_length > 1021)
  {
    print_err("### PMT data is too long - will not fit in 1021 bytes\n");
    report_pmt(FALSE,"    ",pmt);
    return 1;
  }

  data[0] = 0x02;
  data[1] = (byte) (0xb0 | ((section_length & 0x0F00) >> 8));
  data[2] = (byte) (section_length & 0x0FF);
  data[3] = (byte) ((pmt->program_number & 0xFF00) >> 8);
  data[4] = (byte)  (pmt->program_number & 0x00FF);
  data[5] = 0xc1;
  data[6] = 0x00; // section number
  data[7] = 0x00; // last section number
  data[8] = (byte) (0xE0 | ((pmt->PCR_pid & 0x1F00) >> 8));
  data[9] = (byte) (pmt->PCR_pid & 0x00FF);
  data[10] = 0xF0;
  data[11] = (byte)pmt->program_info_length;
  if (pmt->program_info_length > 0)
    memcpy(data+12,pmt->program_info,pmt->program_info_length);

  offset = 12 + pmt->program_info_length;

  for (ii=0; ii < pmt->num_streams; ii++)
  {
    uint32_t pid = pmt->streams[ii].elementary_PID;
    uint16_t len = pmt->streams[ii].ES_info_length;
    data[offset+0] = pmt->streams[ii].stream_type;
    data[offset+1] = (byte) (0xE0 | ((pid & 0x1F00) >> 8));
    data[offset+2] = (byte) (pid & 0x00FF);
    data[offset+3] = ((len & 0xFF00) >> 8) | 0xF0;
    data[offset+4] =   len & 0x00FF;
    memcpy(data+offset+5,pmt->streams[ii].ES_info,len);
    offset += 5 + len;
  }

  crc32 = crc32_block(0xffffffff,data,offset);
  data[offset+0] = (byte) ((crc32 & 0xff000000) >> 24);
  data[offset+1] = (byte) ((crc32 & 0x00ff0000) >> 16);
  data[offset+2] = (byte) ((crc32 & 0x0000ff00) >>  8);
  data[offset+3] = (byte)  (crc32 & 0x000000ff);
  data_length = offset + 4;

#if 1
  if (data_length != section_length + 3)
  {
    fprint_err("### PMT length %d, section length+3 %d\n",
               data_length,section_length+3);
    return 1;
  }
#endif

  crc32 = crc32_block(0xffffffff,data,data_length);
  if (crc32 != 0)
  {
    print_err("### PMT CRC does not self-cancel\n");
    return 1;
  }
  err = TS_program_packet_hdr(pmt_pid,data_length,TS_packet,&TS_hdr_len);
  if (err)
  {
    print_err("### Error constructing PMT packet header\n");
    return 1;
  }
  err = write_TS_packet_parts(output,TS_packet,TS_hdr_len,NULL,0,
                              data,data_length,0x02,FALSE,0);
  if (err)
  {
    print_err("### Error writing PMT\n");
    return 1;
  }
  return 0;
}

/*
 * Write out a Transport Stream PAT and PMT, given the appropriate
 * datastructures
 * 
 * - `output` is the TS output context returned by `tswrite_open`
 * - `transport_stream_id` is the id for this particular transport stream.
 * - `prog_list` is a PIDINT list of program number / PID pairs.
 * - `pmt_pid` is the PID for the PMT.
 * - 'pmt' is the datastructure containing the PMT information
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_pat_and_pmt(TS_writer_p    output,
                             uint32_t       transport_stream_id,
                             pidint_list_p  prog_list,
                             uint32_t       pmt_pid,
                             pmt_p          pmt)
{
  int err;
  err = write_pat(output,transport_stream_id,prog_list);
  if (err) return 1;
  err = write_pmt(output,pmt_pid,pmt);
  if (err) return 1;
  return 0;
}

/*
 * Write out a Transport Stream PAT, for a single program.
 * 
 * - `output` is the TS output context returned by `tswrite_open`
 * - `transport_stream_id` is the id for this particular transport stream.
 * - `program_number` is the program number to use for the PID.
 * - `pmt_pid` is the PID for the PMT.
 *
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_single_program_pat(TS_writer_p output,
                                    uint32_t    transport_stream_id,
                                    uint32_t    program_number,
                                    uint32_t    pmt_pid)
{
  int                   err;
  pidint_list_p         prog_list;

  err = build_pidint_list(&prog_list);
  if (err) return 1;
  err = append_to_pidint_list(prog_list,pmt_pid,program_number);
  if (err)
  {
    free_pidint_list(&prog_list);
    return 1;
  }

  err = write_pat(output,transport_stream_id,prog_list);
  if (err)
  {
    free_pidint_list(&prog_list);
    return 1;
  }

  free_pidint_list(&prog_list);
  return 0;
}

/*
 * Write out a Transport Stream Null packet.
 *
 * - `output` is the TS output context returned by `tswrite_open`
 * 
 * Returns 0 if it worked, 1 if something went wrong.
 */
extern int write_TS_null_packet(TS_writer_p output)
{
  byte   TS_packet[TS_PACKET_SIZE];
  int    err, ii;

#if DEBUG_WRITE_PACKETS
  print_msg("|| Null packet\n");
#endif

  TS_packet[0] = 0x47;
  TS_packet[1] = 0x1F;  // PID is 0x1FFF
  TS_packet[2] = 0xFF;
  TS_packet[3] = 0x20;  // payload only
  for (ii=4; ii<TS_PACKET_SIZE; ii++)
    TS_packet[ii] = 0xFF;

  err = write_TS_packet_parts(output,TS_packet,TS_PACKET_SIZE,NULL,0,NULL,0,
                              0x1FF,FALSE,0);
  if (err)
  {
    print_err("### Error writing null TS packet\n");
    return 1;
  }
  return 0;
}

// ============================================================
// Reading a Transport Stream
// ============================================================

static uint64_t TWENTY_SEVEN_MHZ = 27000000;


// ------------------------------------------------------------
// File handling
// ------------------------------------------------------------
/*
 * Build a TS packet reader, including its read-ahead buffer
 *
 * - `file` is the file that the TS packets will be read from.
 *   It is assumed that its read position is at its start.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int new_TS_reader(TS_reader_p  *tsreader)
{
  TS_reader_p new = malloc(SIZEOF_TS_READER);
  if (new == NULL)
  {
    print_err("### Unable to allocate TS read-ahead buffer\n");
    return 1;
  }

  memset(new, '\0', SIZEOF_TS_READER);

  new->file = -1;

  *tsreader = new;
  return 0;
}

// ------------------------------------------------------------
// File handling
// ------------------------------------------------------------
/*
 * Build a TS packet reader, including its read-ahead buffer
 *
 * - `file` is the file that the TS packets will be read from.
 *   It is assumed that its read position is at its start.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int build_TS_reader(int           file,
                           TS_reader_p  *tsreader)
{
  TS_reader_p new;
  int err = new_TS_reader(&new);
  if (err) return 1;

  new->file = file;

  *tsreader = new;
  return 0;
}


/*
 * Build a TS packet reader using the given functions as read() and seek().
 *
 * Returns 0 on success, 1 on failure.
 */
extern int build_TS_reader_with_fns(void *handle,
                                    int (*read_fn)(void *, byte *, size_t),
                                    int (*seek_fn)(void *, offset_t), 
                                    TS_reader_p *tsreader)
{
  TS_reader_p new;
  int err = new_TS_reader(&new);
  if (err) return 1;

  new->handle = handle;
  new->read_fn = read_fn;
  new->seek_fn = seek_fn;

  *tsreader = new;
  return 0;
}


/*
 * Open a file to read TS packets from.
 *
 * If `filename` is NULL, then the input will be taken from standard input.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int open_file_for_TS_read(char         *filename,
                                 TS_reader_p  *tsreader)
{
  int  err;
  int  file;

  if (filename == NULL)
    file = STDIN_FILENO;
  else
  {
    file = open_binary_file(filename,FALSE);
    if (file == -1)
      return 1;
  }

  err = build_TS_reader(file,tsreader);
  if (err)
  {
    (void) close_file(file);
    return 1;
  }
  return 0;
}

/*
 * Free a TS packet read-ahead buffer
 *
 * Sets `buffer` to NULL.
 */
extern void free_TS_reader(TS_reader_p  *tsreader)
{
  if (*tsreader != NULL)
  {
    if ((*tsreader)->pcrbuf != NULL)
      free((*tsreader)->pcrbuf);
    (*tsreader)->file = -1;
    free(*tsreader);
    *tsreader = NULL;
  }
}

/*
 * Free a TS packet read-ahead buffer and close the referenced file
 * (if it is not standard input).
 *
 * Sets `buffer` to NULL, whether the file close succeeds or not.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int close_TS_reader(TS_reader_p  *tsreader)
{
  int err = 0;
  if (*tsreader == NULL)
    return 0;
  if ((*tsreader)->file != STDIN_FILENO && (*tsreader)->file != -1)
    err = close_file((*tsreader)->file);

  free_TS_reader(tsreader);
  return err;
}

/*
 * Seek to a given offset in the TS reader's file
 *
 * (This should be used in preference to just seeking on the "bare" file
 * since it also unsets the read-ahead buffer. However, it is still just
 * a wrapper around `seek_file`.)
 *
 * It is assumed (but not checked) that the seek will end up at an appropriate
 * offset for reading a TS packet - i.e., presumably some multiple of
 * TS_PACKET_SIZE.
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
extern int seek_using_TS_reader(TS_reader_p  tsreader,
                                offset_t     posn)
{
  tsreader->read_ahead_ptr = NULL;
  tsreader->read_ahead_end = NULL;
  tsreader->posn = posn;

  if (tsreader->seek_fn)
    {
      return tsreader->seek_fn(tsreader->handle, posn);
    }
  else
    {
      return seek_file(tsreader->file,posn);
    }
}
  
/*
 * Read the next several TS packets, possibly not from the start
 *
 * - `tsreader` is the TS packet reading context
 * - `start_len` is the number of bytes of the first packet we've already
 *   got in hand - normally 0.
 * - `packet` is (a pointer to) the resultant TS packet.
 *
 *   This is a pointer into the reader's read-ahead buffer, and so should not
 *   be freed. Note that this means that it may not persist after another call
 *   of this function (and will not persist after a call of
 *   `free_TS_reader`).
 *
 * Returns 0 if all goes well, EOF if end of file was read, or 1 if some
 * other error occurred (in which case it will already have output a message
 * on stderr about the problem).
 */
static int read_next_TS_packets(TS_reader_p  tsreader,
                                int          start_len,
                                byte        *packet[TS_PACKET_SIZE])
{
#ifdef _WIN32
  int total = start_len;
  int length;
#else
  ssize_t total = start_len;
  ssize_t length;
#endif

  // If we exit with an error make sure we don't return anything valid here!
  *packet = NULL;

  if (tsreader->read_ahead_ptr == tsreader->read_ahead_end)
  {
    // Try to allow for partial reads
    while (total < TS_READ_AHEAD_BYTES)
    {
      if (tsreader->read_fn)
        length = tsreader->read_fn(tsreader->handle,
                                   &(tsreader->read_ahead[total]),
                                   TS_READ_AHEAD_BYTES-total);
      else
        length = read(tsreader->file,
                      &(tsreader->read_ahead[total]),
                      TS_READ_AHEAD_BYTES - total);

      if (length == 0)  // EOF - no more data to read
        break;
      else if (length == -1)
      {
        fprint_err("### Error reading TS packets: %s\n",strerror(errno));
        return 1;
      }
      total += length;
    }

    // If we didn't manage to read anything at all, then indicate EOF this
    // time - we assume that if we actually read to the EOF but got some data,
    // we'll "hit" EOF again next time we try to read.
    if (total == 0)
      return EOF;

    if (total % TS_PACKET_SIZE != 0)
    {
      fprint_err("!!! %d byte%s ignored at end of file - not enough"
                 " to make a TS packet\n",
                 (int)(total % TS_PACKET_SIZE),(total % TS_PACKET_SIZE == 1?"":"s"));
      // Retain whatever full packets we *do* have
      total = total - (total % TS_PACKET_SIZE);
      if (total == 0)
        return EOF;
    }
    tsreader->read_ahead_ptr = tsreader->read_ahead;
    tsreader->read_ahead_end = tsreader->read_ahead + total;
  }

  *packet = tsreader->read_ahead_ptr;
  tsreader->read_ahead_ptr += TS_PACKET_SIZE;  // ready for next time
  tsreader->posn += TS_PACKET_SIZE;            // ditto
  return 0;
}

/*
 * Read the (rest of the) first TS packet, given its first four bytes
 *
 * This is intended for use after inspecting the first four bytes of the
 * input file, to determine if the file is TS or PS.
 *
 * - `tsreader` is the TS packet reading context
 * - `start` is the first four bytes of the file 
 * - `packet` is (a pointer to) the resultant TS packet.
 *
 *   This is a pointer into the reader's read-ahead buffer, and so should not
 *   be freed. Note that this means that it may not persist after another call
 *   of this function (and will not persist after a call of
 *   `free_TS_reader`).
 *
 * Note that the caller is trusted to call this only when appropriate.
 *
 * Returns 0 if all goes well, EOF if end of file was read, or 1 if some
 * other error occurred (in which case it will already have output a message
 * on stderr about the problem).
 */
extern int read_rest_of_first_TS_packet(TS_reader_p  tsreader,
                                        byte         start[4],
                                        byte       **packet)
{
  tsreader->read_ahead[0] = start[0];
  tsreader->read_ahead[1] = start[1];
  tsreader->read_ahead[2] = start[2];
  tsreader->read_ahead[3] = start[3];

  // So we already have the first 4 bytes in hand
  return read_next_TS_packets(tsreader,4,packet);
}

/*
 * Read the next TS packet.
 *
 * - `tsreader` is the TS packet reading context
 * - `packet` is (a pointer to) the resultant TS packet.
 *
 *   This is a pointer into the reader's read-ahead buffer, and so should not
 *   be freed. Note that this means that it may not persist after another call
 *   of this function (and will not persist after a call of
 *   `free_TS_reader`).
 *
 * Returns 0 if all goes well, EOF if end of file was read, or 1 if some
 * other error occurred (in which case it will already have output a message
 * on stderr about the problem).
 */
extern int read_next_TS_packet(TS_reader_p  tsreader,
                               byte       **packet)
{
  return read_next_TS_packets(tsreader,0,packet);
}

// ------------------------------------------------------------
// Reading a transport stream with buffered timing
// Keeps a PCR in hand, so that it has accurate timing information
// for each TS packet
// ------------------------------------------------------------
// This is a simplistic approach to the problem -- if it suffices,
// it will be left as-is until something more sophisticated is
// needed


/* Make sure we've got a PCR buffer allocated, and that
 * its content is entirely unset (so this also serves as
 * a "reset" function).
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int start_TS_packet_buffer(TS_reader_p  tsreader)
{
  if (tsreader->pcrbuf == NULL)
  {
    tsreader->pcrbuf = malloc(SIZEOF_TS_PCR_BUFFER);
    if (tsreader->pcrbuf == NULL)
    {
      print_err("### Unable to allocate TS PCR read-ahead buffer\n");
      return 1;
    }
  }
  memset(tsreader->pcrbuf, '\0', SIZEOF_TS_PCR_BUFFER);
  return 0;
}


/* Fill up the PCR read-ahead buffer with TS entries, until we hit
 * one (of the correct PID) with a PCR.
 *
 * Returns 0 if all went well, 1 if something went wrong, EOF if EOF was read.
 */
static int fill_TS_packet_buffer(TS_reader_p  tsreader)
{
  int ii;

  // Work out which TS packet we *will* have as our first (zeroth) entry
  tsreader->pcrbuf->TS_buffer_posn += tsreader->pcrbuf->TS_buffer_len;

  tsreader->pcrbuf->TS_buffer_len = 0;
  tsreader->pcrbuf->TS_buffer_next = 0;
  for (ii=0; ii<PCR_READ_AHEAD_SIZE; ii++)
  {
    byte    *data;
    uint32_t pid;
    int      got_pcr;
    uint64_t pcr;
    int      payload_unit_start_indicator;
    byte    *adapt;
    int      adapt_len;
    byte    *payload;
    int      payload_len;

    // Retrieve a pointer to the data for the next TS packet
    int err = read_next_TS_packet(tsreader,&data);
    if (err)
    {
      if (err == EOF)
      {
        // For simplicity (of my coding, not anything else), when we hit
        // EOF we'll just return as much. This means that we will ignore
        // any TS records between the last TS-with-a-PCR and the EOF.
        return EOF;
      }
      else
      {
        fprint_err("### Error (pre)reading TS packet %d\n",
                   tsreader->pcrbuf->TS_buffer_posn+ii);
        return 1;
      }
    }

    // Copy the data into our own read-ahead buffer
    memcpy(tsreader->pcrbuf->TS_buffer[ii],data,TS_PACKET_SIZE);

    err = split_TS_packet(data,&pid,&payload_unit_start_indicator,
                          &adapt,&adapt_len,&payload,&payload_len);
    if (err)
    {
      fprint_err("### Error splitting TS packet %d\n",
                 tsreader->pcrbuf->TS_buffer_posn+ii);
      return 1;
    }
    tsreader->pcrbuf->TS_buffer_len ++;

    if (pid != tsreader->pcrbuf->TS_buffer_pcr_pid)
      continue;                 // don't care about any PCR it might have

    get_PCR_from_adaptation_field(adapt,adapt_len,&got_pcr,&pcr);
    if (got_pcr)
    {
      tsreader->pcrbuf->TS_buffer_prev_pcr = tsreader->pcrbuf->TS_buffer_end_pcr;
      tsreader->pcrbuf->TS_buffer_end_pcr = pcr;
      tsreader->pcrbuf->TS_buffer_time_per_TS =
        pcr_unsigned_diff(tsreader->pcrbuf->TS_buffer_end_pcr, tsreader->pcrbuf->TS_buffer_prev_pcr) /
                                                tsreader->pcrbuf->TS_buffer_len;
      return 0;
    }
  }
  // If we ran out of buffer, then we've really got no choice but to give up
  // with an appropriate grumble
  fprint_err("!!! Next PCR not found when reading forwards"
             " (for %d TS packets, starting at TS packet %d)\n",PCR_READ_AHEAD_SIZE,
             tsreader->pcrbuf->TS_buffer_posn);
  return 1;
}

/* Set up the the "looping" buffered TS packet reader and let it know what its
 * PCR PID is.
 *
 * This must be called before any other _buffered_TS_packet function.
 *
 * - `pcr_pid` is the PID within which we should look for PCR entries
 *
 * Returns 0 if all went well, 1 if something went wrong (allocating space
 * for the TS PCR buffer).
 */
extern int prime_read_buffered_TS_packet(TS_reader_p  tsreader,
                                         uint32_t     pcr_pid)
{
  if (start_TS_packet_buffer(tsreader))
    return 1;
  tsreader->pcrbuf->TS_buffer_pcr_pid = pcr_pid;
  return 0;
}

/* Retrieve the first TS packet from the PCR read-ahead buffer,
 * complete with its calculated PCR time.
 *
 * prime_read_buffered_TS_packet() must have been called before this.
 *
 * This should be called the first time a TS packet is to be read
 * using the PCR read-ahead buffer. It "primes" the read-ahead mechanism
 * by performing the first actual read-ahead.
 *
 * - `pcr_pid` is the PID within which we should look for PCR entries
 * - `start_count` is the index of the current (last read) TS entry (which will
 *   generally be the PMT).
 * - `data` returns a pointer to the TS packet data
 * - `pid` is its PID
 * - `pcr` is its PCR, calculated using the previous known PCR and
 *   the following known PCR.
 * - `count` is the index of the returned TS packet in the file
 *
 * Note that, like read_next_TS_packet, we return a pointer to our data,
 * and, similarly, warn that it will go away next time this function
 * is called.
 *
 * Returns 0 if all went well, 1 if something went wrong, EOF if EOF was read.
 */
extern int read_first_TS_packet_from_buffer(TS_reader_p  tsreader,
                                            uint32_t     pcr_pid,
                                            uint32_t     start_count,
                                            byte        *data[TS_PACKET_SIZE],
                                            uint32_t    *pid,
                                            uint64_t    *pcr,
                                            uint32_t    *count)
{
  int err;

  if (tsreader->pcrbuf == NULL)
  {
    print_err("### TS PCR read-ahead buffer has not been set up\n"
              "    Make sure prime_read_buffered_TS_packet() has been called\n");
    return 1;
  }

  // Reset things
  tsreader->pcrbuf->TS_buffer_next = 0;
  tsreader->pcrbuf->TS_buffer_end_pcr = 0;
  tsreader->pcrbuf->TS_buffer_prev_pcr = 0;
  tsreader->pcrbuf->TS_buffer_posn = start_count;
  tsreader->pcrbuf->TS_buffer_len = 0;
  tsreader->pcrbuf->TS_buffer_pcr_pid = pcr_pid;
  tsreader->pcrbuf->TS_had_EOF = FALSE;

  // Read TS packets into our buffer until we find one with a PCR
  err = fill_TS_packet_buffer(tsreader);
  if (err) return err;

  // However, it's only the last packet (the one with the PCR) that
  // we are actually interested in
  tsreader->pcrbuf->TS_buffer_next = tsreader->pcrbuf->TS_buffer_len - 1;

  // Why, this is the very packet with its own PCR
  *pcr = tsreader->pcrbuf->TS_buffer_end_pcr;

  *data = tsreader->pcrbuf->TS_buffer[tsreader->pcrbuf->TS_buffer_next];
  *pid = tsreader->pcrbuf->TS_buffer_pids[tsreader->pcrbuf->TS_buffer_next];

  *count = start_count + tsreader->pcrbuf->TS_buffer_len;

  tsreader->pcrbuf->TS_buffer_next ++;
  return 0;
}

/* Retrieve the next TS packet from the PCR read-ahead buffer,
 * complete with its calculated PCR time.
 *
 * - `data` returns a pointer to the TS packet data
 * - `pid` is its PID
 * - `pcr` is its PCR, calculated using the previous known PCR and
 *   the following known PCR.
 *
 * Note that, like read_next_TS_packet, we return a pointer to our data,
 * and, similarly, warn that it might go away next time this function
 * is called.
 *
 * Returns 0 if all went well, 1 if something went wrong, EOF if EOF was read.
 */
extern int read_next_TS_packet_from_buffer(TS_reader_p  tsreader,
                                           byte        *data[TS_PACKET_SIZE],
                                           uint32_t    *pid,
                                           uint64_t    *pcr)
{
  int err;

  if (tsreader->pcrbuf == NULL)
  {
    print_err("### TS PCR read-ahead buffer has not been set up\n"
              "    Make sure read_first_TS_packet_from_buffer() has been called\n");
    return 1;
  }

  if (tsreader->pcrbuf->TS_buffer_next == tsreader->pcrbuf->TS_buffer_len)
  {
    if (tsreader->pcrbuf->TS_had_EOF)
    {
      // We'd already run out of look-ahead packets, so just return
      // our (deferred) end-of-file
      return EOF;
    }
    else
    {
      err = fill_TS_packet_buffer(tsreader);
      if (err == EOF)
      {
        // An EOF means we read the end-of-file before finding the next
        // TS packet with a PCR. We could stop here (returning EOF), but
        // whilst that would mean all TS packets had guaranteed accurate
        // PCRs, it would also mean that we would ignore some TS packets
        // at the end of the file. This proved unacceptable in practice,
        // so our second best choice is to "play out" using the last
        // known PCR rate-of-change.
        tsreader->pcrbuf->TS_had_EOF = TRUE;              // remember we're playing out
        // Must move PCR start
        tsreader->pcrbuf->TS_buffer_prev_pcr = tsreader->pcrbuf->TS_buffer_end_pcr;
        // If we read nothing we must die now
        if (tsreader->pcrbuf->TS_buffer_next == tsreader->pcrbuf->TS_buffer_len)
          return err;
      }
      else if (err)
        return err;
    }
  }

  *data = tsreader->pcrbuf->TS_buffer[tsreader->pcrbuf->TS_buffer_next];
  *pid = tsreader->pcrbuf->TS_buffer_pids[tsreader->pcrbuf->TS_buffer_next];

  tsreader->pcrbuf->TS_buffer_next ++;

  if (tsreader->pcrbuf->TS_buffer_next == tsreader->pcrbuf->TS_buffer_len &&
      !tsreader->pcrbuf->TS_had_EOF)
  {
    // Why, this is the very packet with its own PCR
    *pcr = tsreader->pcrbuf->TS_buffer_end_pcr;
  }
  else
  {
    *pcr = pcr_unsigned_wrap(tsreader->pcrbuf->TS_buffer_prev_pcr +
           tsreader->pcrbuf->TS_buffer_time_per_TS *
           tsreader->pcrbuf->TS_buffer_next);
  }
  return 0;
}

/*
 * Read the next TS packet, coping with looping, etc.
 *
 * prime_read_buffered_TS_packet() should have been called first.
 *
 * This is a convenience wrapper around read_first_TS_packet_from_buffer()
 * and read_next_TS_packet_from_buffer().
 *
 * This differs from ``read_TS_packet`` in that it assumes that the
 * underlying code will already have read to the next PCR, so that
 * it can know the *actual* (PCR-based) time for each TS packet.
 *
 * - `tsreader` is the TS reader context
 * - `count` is a running count of TS packets read from this input
 * - `data` is a pointer to the data for the packet
 * - `pid` is the PID of the TS packet
 * - `pcr` is the PCR value (possibly calculated) for this TS packet
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable) - i.e., rewind to `start_posn` and start again if
 *   `count` reaches `max` (obviously only if `max` is greater than zero).
 * - `start_count` is the value `count` should have after we've looped back
 *   to `start_posn`
 * - if `quiet` is true, then only error messages should be written out
 *
 * Returns 0 if all went well, 1 if something went wrong, EOF if `loop` is
 * false and either EOF was read, or `max` TS packets were read.
 */
extern int read_buffered_TS_packet(TS_reader_p  tsreader,
                                   uint32_t    *count,
                                   byte        *data[TS_PACKET_SIZE],
                                   uint32_t    *pid,
                                   uint64_t    *pcr,
                                   int          max,
                                   int          loop,
                                   offset_t     start_posn,
                                   uint32_t     start_count,
                                   int          quiet)
{
  int     err;

  if (max > 0 && (*count) >= (uint32_t)max)
  {
    if (loop)
    {
      if (!quiet)
        fprint_msg("Read %d packets, rewinding and continuing\n",max);
      err = seek_using_TS_reader(tsreader,start_posn);
      if (err) return 1;
      *count = start_count;
    }
    else
    {
      if (!quiet) fprint_msg("Stopping after %d TS packets\n",max);
      return EOF;
    }
  }

  // Read the next packet
  if (*count == start_count)
  {
    // XXX
    // XXX We *strongly* assume that we will find two PCRs (in the
    // XXX required distance -- I think it's best to declare that
    // XXX "not a problem", by fiat.
    // XXX
    // XXX But is it acceptable that we ignore any TS packets before
    // XXX the first packet with a PCR? Probably more so than that we
    // XXX should ignore any packets at the end of the file.
    // XXX
    err = read_first_TS_packet_from_buffer(tsreader,
                                           tsreader->pcrbuf->TS_buffer_pcr_pid,
                                           start_count,data,pid,pcr,count);
    if (err)
    {
      if (err == EOF)
      {
        print_err("### EOF looking for first PCR\n");
        return 1;
      }
      else
      {
        fprint_err("### Error reading TS packet %d, looking for first PCR\n",
                   *count);
        return 1;
      }
    }
  }
  else
  {
    err = read_next_TS_packet_from_buffer(tsreader,data,pid,pcr);
    if (err)
    {
      if (err == EOF)
      {
        if (!loop)
          return EOF;
        if (!quiet)
          fprint_msg("EOF (after %d TS packets), rewinding and continuing\n",
                     *count);
      }
      else
      {
        fprint_err("### Error reading TS packet %d\n",*count);
        if (!loop)
          return 1;
        if (!quiet)
          print_msg("!!! Rewinding and continuing anyway\n");
      }
      err = seek_using_TS_reader(tsreader,start_posn);
      if (err) return 1;

      *count = start_count;
      err = read_first_TS_packet_from_buffer(tsreader,
                                             tsreader->pcrbuf->TS_buffer_pcr_pid,
                                             start_count,data,pid,pcr,count);
      if (err)
      {
        print_err("### Failed rewinding\n");
        return 1;
      }
    }
    else
      (*count) ++;
  }
  return 0;
}

// ------------------------------------------------------------
// Packet interpretation
// ------------------------------------------------------------
/*
 * Retrieve the PCR (if any) from a TS packet's adaptation field
 *
 * - `adapt` is the adaptation field content
 * - `adapt_len` is its length
 * - `got_PCR` is TRUE if the adaptation field contains a PCR
 * - `pcr` is then the PCR value itself
 */
extern void get_PCR_from_adaptation_field(byte     adapt[],
                                          int      adapt_len,
                                          int     *got_pcr,
                                          uint64_t *pcr)
{
  if (adapt_len == 0 || adapt == NULL)
    *got_pcr = FALSE;
  else if (adapt[0] & 0x10)  // We have a PCR
  {
    *got_pcr = TRUE;
    // The program_clock_reference_base
    // NB: Force the first byte to be unsigned 64 bit, or else on Windows
    // it tends to get shifted as a signed integer, and sign-extended,
    // before it gets turned unsigned (which is probably the "correct"
    // behaviour according to the standard. oh well).
    *pcr = ((uint64_t)adapt[1] << 25) | (adapt[2] << 17) | (adapt[3] << 9) |
      (adapt[4] << 1) | (adapt[5] >> 7);
    // Plus the program clock reference extension
    *pcr = ((*pcr) * 300) + ((adapt[5] & 1) << 8) + adapt[6];
  }
  else
    *got_pcr = FALSE;
  return;
}

/*
 * Report on the contents of this TS packet's adaptation field
 *
 * - `adapt` is the adaptation field content
 * - `adapt_len` is its length
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern void report_adaptation_field(byte        adapt[],
                                    int         adapt_len)
{
  int      got_pcr;
  uint64_t pcr;

  if (adapt_len == 0 || adapt == NULL)
    return;

  fprint_msg("  Adaptation field len %3d [flags %02x]",adapt_len,adapt[0]);
  if (adapt[0] != 0)
  {
    print_msg(":");
    if (ON(adapt[0],0x80)) print_msg(" discontinuity ");
    if (ON(adapt[0],0x40)) print_msg(" random access ");
    if (ON(adapt[0],0x20)) print_msg(" ES-priority ");
    if (ON(adapt[0],0x10)) print_msg(" PCR ");
    if (ON(adapt[0],0x08)) print_msg(" OPCR ");
    if (ON(adapt[0],0x04)) print_msg(" splicing ");
    if (ON(adapt[0],0x02)) print_msg(" private ");
    if (ON(adapt[0],0x01)) print_msg(" extension ");
  }
  print_msg("\n");

  get_PCR_from_adaptation_field(adapt,adapt_len,&got_pcr,&pcr);
  if (got_pcr)
  {
    fprint_msg(" .. PCR %12" LLU_FORMAT_STUMP "\n", pcr);
  }
  return;
}

/*
 * Report on the timing information from this TS packet's adaptation field
 *
 * - if `times` is non-NULL, then timing information (derived from the PCR)
 *   will be calculated and reported
 * - `adapt` is the adaptation field content
 * - `adapt_len` is its length
 * - `packet_count` is a count of how many TS packets up to now
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern void report_adaptation_timing(timing_p    times,
                                     byte        adapt[],
                                     int         adapt_len,
                                     int         packet_count)
{
  int      got_pcr;
  uint64_t pcr;

  if (adapt_len == 0 || adapt == NULL || times == NULL)
    return;

  get_PCR_from_adaptation_field(adapt,adapt_len,&got_pcr,&pcr);
  if (got_pcr)
  {
    fprint_msg(" .. PCR %12" LLU_FORMAT_STUMP, pcr);
    if (!times->had_first_pcr)
    {
      times->last_pcr_packet = times->first_pcr_packet = packet_count;
      times->last_pcr = times->first_pcr = pcr;
      times->had_first_pcr = TRUE;
    }
    else
    {
      if (pcr < times->last_pcr)
        fprint_msg(" Discontinuity: PCR was %7" LLU_FORMAT_STUMP ", now %7"
                   LLU_FORMAT_STUMP,times->last_pcr,pcr);
      else
      {
        fprint_msg(" Mean byterate %7" LLU_FORMAT_STUMP,
                   ((packet_count - times->first_pcr_packet) * TS_PACKET_SIZE) *
                   TWENTY_SEVEN_MHZ / pcr_unsigned_diff(pcr, times->first_pcr));
        fprint_msg(" byterate %7" LLU_FORMAT_STUMP,
                   ((packet_count - times->last_pcr_packet) * TS_PACKET_SIZE) *
                   TWENTY_SEVEN_MHZ / pcr_unsigned_diff(pcr, times->last_pcr));
      }
    }
    times->last_pcr_packet = packet_count;
    times->last_pcr = pcr;
    print_msg("\n");
  }
  return;
}

/*
 * Report on the contents of this TS packet's payload. The packet is assumed
 * to have a payload that is (part of) a PES packet.
 *
 * - if `show_data` then the data for the PES packet will also be shown
 * - `stream_type` is the stream type of the data, or -1 if it is not
 *   known
 * - `payload` is the payload of the TS packet. We know it can't be more
 *   than 184 bytes long, because of the packet header bytes.
 * - regardless, `payload_len` is the actual length of the payload.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern void report_payload(int         show_data,
                           int         stream_type,
                           byte        payload[MAX_TS_PAYLOAD_SIZE],
                           int         payload_len,
                           int         payload_unit_start_indicator)
{
  if (payload_unit_start_indicator)
    report_PES_data_array2(stream_type,payload,payload_len, show_data?1000:0);
  else if (show_data)
    print_data(TRUE,"Data",payload,payload_len,1000);
}

/*
 * Extract the program list from a PAT packet (PID 0x0000).
 *
 * Handles the result of calling build_psi_data() for this PAT.
 *
 * - if `verbose`, then report on what we're doing
 * - `data` is the data for the PAT packet.
 * - `data_len` is the length of said data.
 * - `prog_list` is the list of program numbers versus PIDs.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int extract_prog_list_from_pat(int            verbose,
                                      byte           data[],
                                      int            data_len,
                                      pidint_list_p *prog_list)
{
  int     table_id;
  int     section_syntax_indicator,zero_bit,reserved1;
  int     section_length;
  int     transport_stream_id;
  int     version_number;
  int     current_next_indicator;
  int     section_number;
  int     last_section_number;
  uint32_t crc = 0;
  uint32_t check_crc;
  byte   *program_data;
  int     program_data_len;
  int     err;

  if (data_len == 0)
  {
    print_err("### PAT data has zero length\n");
    return 1;
  }
  if (data == NULL)
  {
    print_err("### PAT data is NULL\n");
    return 1;
  }

  if (DEBUG) print_data(TRUE,"Data",data,data_len,1000);

  // The table id in a PAT should be 0
  table_id = data[0];
  if (table_id != 0)
  {
    fprint_err("### PAT table id is %0#8x, should be 0\n",table_id);
    return 1;
  }

  // Check bits - do we actually want to check these?
  section_syntax_indicator = (data[1] & 0x80) >> 7;
  zero_bit = (data[1] & 0x40) >> 6;
  reserved1 = (data[1] & 0x30) >> 4;
  if (section_syntax_indicator != 1 && report_bad_reserved_bits)
    print_err("!!! PAT: section syntax indicator is 0, not 1\n");
  if (zero_bit != 0 && report_bad_reserved_bits)
    print_err("!!! PAT: zero bit is 1, not 0\n");
  if (reserved1 != 3 && report_bad_reserved_bits)
    fprint_err("!!! PAT: reserved1 is %d, not 3\n",reserved1);

  section_length = ((data[1] & 0xF) << 8) | data[2];
  if (verbose)
    fprint_msg("  section length:       %03x (%d)\n",
               section_length,section_length);

  // If the section length doesn't match our data length, we've got problems
  // (remember, the section_length counts bytes after the section_length field)
  if (section_length > data_len - 3)
  {
    fprint_err("### PAT section length %d is more than"
               " length of remaining data %d\n",section_length,data_len-3);
    return 1;
  }
  else if (section_length < data_len - 3)
  {
    fprint_err("!!! PAT section length %d does not use all of"
               " remaining data %d\n",section_length,data_len-3);
    // Adjust it and carry on
    data_len = section_length + 3;
  }
  data_len = section_length + 3;

  transport_stream_id = (data[3] << 8) | data[4];
  if (verbose) fprint_msg("  transport stream id: %04x\n",transport_stream_id);
  // reserved2 = (data[5] & 0xC0) >> 14;
  version_number = (data[5] & 0x3E) >> 1;
  current_next_indicator = data[5] & 0x1;
  section_number = data[6];
  last_section_number = data[7];
  if (verbose)
    fprint_msg("  version number %02x, current next %x, section number %x, last"
               " section number %x\n",version_number,current_next_indicator,
           section_number,last_section_number);

  // 32 bits at the end of a program association section is reserved for a CRC
  // (OK, let's extract it stupidly...)
  crc = (crc << 8) | data[data_len-4];
  crc = (crc << 8) | data[data_len-3];
  crc = (crc << 8) | data[data_len-2];
  crc = (crc << 8) | data[data_len-1];

  // Let's check the CRC
  check_crc = crc32_block(0xffffffff,data,data_len);
  if (check_crc != 0)
  {
    fprint_err("!!! Calculated CRC for PAT is %08x, not 00000000"
               " (CRC in data was %08x)\n",check_crc,crc);
    return 1;
  }

  // (remember the section length is for the bytes *after* the section
  // length field, so for data[3...])
  program_data = data + 8;
  program_data_len = data_len - 8 - 4; // The "-4" is for the CRC

  //print_data(TRUE,"Rest:",program_data,program_data_len,1000);

  err = build_pidint_list(prog_list);
  if (err) return 1;

  while (program_data_len > 0)
  {
    int program_number = (program_data[0] << 8) | program_data[1];
    uint32_t pid = ((program_data[2] & 0x1F) << 8) | program_data[3];

    // A program_number of 0 indicates the network ID, so ignore it and
    //  don't append to the program list - rrw 2004-10-13
    if (!program_number)
    {
      if (verbose)
        fprint_msg("    Network ID %04x (%3d)\n", pid, pid);
    }
    else
    {
      if (verbose)
        fprint_msg("    Program %03x (%3d) -> PID %04x (%3d)\n",
                   program_number,program_number,pid,pid);
      err = append_to_pidint_list(*prog_list,pid,program_number);
      if (err) return 1;
    }
    program_data = program_data + 4;
    program_data_len = program_data_len - 4;
  }
  return 0;
}


static const char * dvb_component_type3_str(int component_type)
{
  switch (component_type)
  {
  case 0x01:
    return "EBU Teletext subtitles";
  case 0x02:
    return "associated EBU Teletext";
  case 0x03:
    return "VBI data";
  case 0x10:
    return "DVB subtitles (normal) with no monitor aspect ratio criticality";
  case 0x11:
    return "DVB subtitles (normal) for display on 4:3 aspect ratio monitor";
  case 0x12:
    return "DVB subtitles (normal) for display on 16:9 aspect ratio monitor";
  case 0x13:
    return "DVB subtitles (normal) for display on 2.21:1 aspect ratio monitor";
  case 0x14:
    return "DVB subtitles (normal) for display on a high definition monitor";
  case 0x20:
    return "DVB subtitles (for the hard of hearing) with no monitor aspect ratio criticality";
  case 0x21:
    return "DVB subtitles (for the hard of hearing) for display on 4:3 aspect ratio monitor";
  case 0x22:
    return "DVB subtitles (for the hard of hearing) for display on 16:9 aspect ratio monitor";
  case 0x23:
    return "DVB subtitles (for the hard of hearing) for display on 2.21:1 aspect ratio monitor";
  case 0x24:
    return "DVB subtitles (for the hard of hearing) for display on a high definition monitor";

  default:
    if (component_type >= 0xb0 && component_type <= 0xfe)
    {
      return "user defined";
    }
    break;
  }
  return "reserved";
}

static const char * const descriptor_names[] =
{
    "Reserved",  // 0
    "Forbidden",  // 1
    "Video stream",  // 2
    "Audio stream",  // 3
    "Hierarchy",  // 4
    "Registration",  // 5
    "Data stream alignment",  // 6
    "Target background grid",  // 7
    "Video window",  // 8
    "CA",  // 9
    "ISO 639 language",  // 10
    "System clock",  // 11
    "Multiplex buffer utilization",  // 12
    "Copyright",  // 13
    "Maximum bitrate",  // 14
    "Private data indicator",  // 15
    "Smoothing buffer",  // 16
    "STD",  // 17
    "IBP",  // 18
    "Defined in ISO/IEC 13818-6",  // 19
    "Defined in ISO/IEC 13818-6",  // 20
    "Defined in ISO/IEC 13818-6",  // 21
    "Defined in ISO/IEC 13818-6",  // 22
    "Defined in ISO/IEC 13818-6",  // 23
    "Defined in ISO/IEC 13818-6",  // 24
    "Defined in ISO/IEC 13818-6",  // 25
    "Defined in ISO/IEC 13818-6",  // 26
    "MPEG-4 video",  // 27
    "MPEG-4 audio",  // 28
    "IOD",  // 29
    "SL",  // 30
    "FMC",  // 31
    "External ES ID",  // 32
    "MuxCode",  // 33
    "FmxBufferSize",  // 34
    "MultiplexBuffer",  // 35
    "Content labeling",  // 36
    "Metadata pointer",  // 37
    "Metadata",  // 38
    "Metadata STD",  // 39
    "AVC video descriptor",  // 40
    "IPMP (defined in ISO/IEC 13818-11, MPEG-2 IPMP)",  // 41
    "AVC timing and HRD descriptor",  // 42
    "MPEG-2 AAC audio",  // 43
    "FlexMuxTiming",  // 44
    "MPEG-4 text",  // 45
    "MPEG-4 audio extension",  // 46
    "auxiliary video stream",  // 47
    "SVC extension",  // 48
    "MVC extension",  // 49
    "J2K video descriptor",  // 50
    "MVC operation point descriptor",  // 51
    "MPEG2 stereoscopic video format",  // 52
    "Stereoscopic_program_info_descriptor", // 53
    "Stereoscopic_video_info_descriptor", // 54
    "Transport_profile_descriptor", // 55
    "HEVC video descriptor", // 56
    "Reserved (57)", // 57
    "Reserved (58)", // 58
    "Reserved (59)", // 59
    "Reserved (60)", // 60
    "Reserved (61)", // 61
    "Reserved (62)", // 62
    "Extension descriptor", // 63
};


// From ATSC A/52B section A3.4
// N.B. Horizontal lines in the table represent valid stop points

static void print_ac3_audio_descriptor(const int is_msg, const byte * const buf, const int len)
{
  const byte * p = buf;
  const byte * const eop = p + len;
  static const char * const sample_rate_txt[8] = {
    "48k", "44k1", "32k", "Reserved(3)", "48k or 44.1k", "48k or 32k", "44.1k or 32k", "48k or 44.1k or 32k"
  };
  static const int bit_rate_n[32] = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
  };
  static const char * const dsurmod_txt[4] = {
    "Unknown", "Not Dolby suuround encoded", "Dolby surround encoded", "Reserved"
  };
  static const char * const num_channels_txt[16] = {
    "1 + 1", "1/0", "2/0", "3/0", "2/1", "3/1", "2/2", "3/2",
    "1", "<=2", "<=3", "<=4", "<=5", "<=6", "Reserved(14)", "Reserved(15)"
  };
  static const char * const priority_txt[4] = {
    "Reserved(0)", "Primary Audio", "Other Audio", "Not specified"
  };
  unsigned int nc;
  unsigned int bsmod;

  if (p >= eop)
  {
    goto too_short;
  }
  fprint_msg_or_err(is_msg, "sample_rate: %s, bsid: %d", sample_rate_txt[*p >> 5], *p & 0x1f);
  ++p;
  fprint_msg_or_err(is_msg, ", bit_rate: %s %dk, dsurmod: %s",  *p >> 7 ? "Upper" : "Exact",
      bit_rate_n[(*p >> 2) & 31],
      dsurmod_txt[*p & 3]);
  if (++p >= eop)
  {
    goto too_short;
  }
  nc = (*p >> 1) & 0x0f;
  bsmod = *p >> 5;
  fprint_msg_or_err(is_msg, ", bsmod: %d, num_channels: %s, full_svc: %d",
      bsmod, num_channels_txt[nc], *p & 1);
  if (++p >= eop)
  {
    goto done;
  }
  fprint_msg_or_err(is_msg, ", langcod: %d", *p);
  if (nc == 0)
  {
    if (++p >= eop)
    {
      goto done;
    }
    fprint_msg_or_err(is_msg, ", langcod2: %d", *p);
  }
  if (++p >= eop)
  {
    goto done;
  }
  if (bsmod < 2)
  {
    fprint_msg_or_err(is_msg, ", mainid: %d, priority: %s", *p >> 5, priority_txt[(*p >> 3) & 3]);
    if ((*p & 7) != 7)
    {
      fprint_msg_or_err(is_msg, ", reserved(7): %d", *p & 7);
    }
  }
  else
  {
    fprint_msg_or_err(is_msg, ", asvcflags: %#08x", *p);
  }
  if (++p >= eop)
  {
    goto done;
  }
  {
    unsigned int textlen = *p >> 1;
    const int utf16 = !(*p & 1);
    fprint_msg_or_err(is_msg, ", text(%c): ", utf16 ? 'U' : 'S');
    if (p + textlen >= eop)
    {
      goto too_short;
    }
    if (textlen == 0)
    {
      fprint_msg_or_err(is_msg, "<none>");
    }
    else if (!utf16)
    {
      fprint_msg_or_err(is_msg, "\"");
      while (textlen-- != 0)
      {
        fprint_msg_or_err(is_msg, "%c", *++p);
      }
      fprint_msg_or_err(is_msg, "\"");
    }
    else
    {
      fprint_msg_or_err(is_msg, "??");
      p += textlen;
    }
  }
  if (++p >= eop)
  {
    goto done;
  }
  {
    const int language_flag = *p >> 7;
    const int language_flag_2 = (*p >> 6) & 1;

    if ((*p & 0x1f) != 0x1f)
    {
      fprint_msg_or_err(is_msg, ", reserved(0x1f): %#x", *p & 0x1f);
    }

    if (language_flag)
    {
      if (++p + 2 >= eop)
      {
        goto too_short;
      }
      fprint_msg_or_err(is_msg, ", language: %c%c%c", p[0], p[1], p[2]);
      p += 2;
    }
    if (language_flag_2)
    {
      if (++p + 2 >= eop)
      {
        goto too_short;
      }
      fprint_msg_or_err(is_msg, ", language_2: %c%c%c", p[0], p[1], p[2]);
      p += 2;
    }
  }

  if (++p >= eop)
  {
    goto done;
  }

  print_data(is_msg, "additional_info: ", p, eop - p,100);

done:
  fprint_msg_or_err(is_msg, "\n");
  return;

too_short:
  fprint_msg_or_err(is_msg, "; ### block short ###\n");
}

static void print_HEVC_descriptor(const int is_msg, const byte * const buf, const int len)
{
  const uint8_t * p = buf;
  const byte * const eop = p + len;
  static const char * const prog_interlace[4] = {
    "unknown scan source",
    "interlaced source",
    "progressive source",
    "mixed scan source"
  };

  if (len == 9)
  {
    // I've seen a number of these but I can't find a standard
    print_data(is_msg, "HEVC video descriptor ### bad length", buf, len ,100);
    return;
  }

  fprint_msg_or_err(is_msg, "HEVC video descriptor:");

  if (p >= eop)
  {
    goto too_short;
  }
  fprint_msg_or_err(is_msg, " profile_space=%d, tier_flag=%d, profile_idc=%d", *p >> 6, (*p >> 5) & 1, *p & 0x1f);
  if (++p + 3 >= eop)
  {
    goto too_short;
  }
  fprint_msg_or_err(is_msg, ", profile_compatability=%#08x", uint_32_be(p));
  if ((p += 4) + 5 >= eop)
  {
    goto too_short;
  }
  fprint_msg_or_err(is_msg, ", %s%s%s", prog_interlace[*p >> 6], *p & 0x20 ? ", non_packed" : "", *p & 0x10 ? ", frame_only" : "");
  if ((*p & 0xf) != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0 || p[4] != 0 || p[5] != 0)
  {
    fprint_msg_or_err(is_msg, ", ### reserved_zero_44bits=0x%x%02x%08x", *p & 0xf, p[1], uint_32_be(p + 2));
  }
  if ((p += 6) >= eop)
  {
    goto too_short;
  }
  fprint_msg_or_err(is_msg, ", level=%d.%d", *p / 30, *p % 30);
  if (++p >= eop)
  {
    goto too_short;
  }
  fprint_msg_or_err(is_msg, "%s%s", *p & 0x40 ? ", still" : "", *p & 0x20 ? ", 24hr" : "");
  if ((*p & 0x1f) != 0x1f)
  {
    fprint_msg_or_err(is_msg, ", ### reserved=%#02x", *p & 0x1f);
  }
  if ((*p++ & 0x80) != 0)
  {
    fprint_msg_or_err(is_msg, ", temporal_id");

    if (p + 2 >= eop)
    {
      goto too_short;
    }

    if ((*p >> 3) != 0x1f)
    {
      fprint_msg_or_err(is_msg, " ### reserved=%#02x", *p & 0x1f);
    }
    fprint_msg_or_err(is_msg, " min=%d", *p & 7);
    ++p;
    if ((*p >> 3) != 0x1f)
    {
      fprint_msg_or_err(is_msg, " ### reserved=%#02x", *p & 0x1f);
    }
    fprint_msg_or_err(is_msg, " max=%d", *p & 7);
    ++p;
  }
  fprint_msg_or_err(is_msg,"\n");
  return;

too_short:
  fprint_msg_or_err(is_msg, "; ### block short ###\n");
}

/*
 * Print out information about program descriptors
 * (either from the PMT program info, or the PMT/stream ES info)
 *
 * - if `is_msg` then print as a message, otherwise as an error
 * - `leader1` and `leader2` are the text to write at the start of each line
 *   (either or both may be NULL)
 * - `desc_data` is the data containing the descriptors
 * - `desc_data_len` is its length
 *
 * Returns 0 if all went well, 1 if something went wrong
 *
 * If you want to interpret more descriptors then ITU-T J.94 is the standard
 */
extern int print_descriptors(int    is_msg,
                             char  *leader1,
                             char  *leader2,
                             byte  *desc_data,
                             int    desc_data_len)
{
  byte   data_len = desc_data_len;
  byte  *data = desc_data;
  while (data_len >= 2)
  {
    byte  tag = data[0];
    byte  this_length = data[1];

    data     += 2;
    data_len -= 2;

    if (this_length > data_len)
    {
      // Not much we can do - try giving up?
      fprint_msg_or_err(is_msg,"Descriptor %x says length %d, but only %d bytes left\n",
                        tag,this_length,data_len);
      return 1;  // Hmm - well, maybe
    }

    if (leader1 != NULL) fprint_msg_or_err(is_msg,"%s",leader1);
    if (leader2 != NULL) fprint_msg_or_err(is_msg,"%s",leader2);

    {
      int    ii;
      uint32_t temp_u;

      switch (tag)
      {
      case 5:
        fprint_msg_or_err(is_msg,"Registration ");
        if (this_length >= 4)
        {
          for (ii=0; ii<4; ii++)
          {
            if (isprint(data[ii]))
              fprint_msg_or_err(is_msg,"%c",data[ii]);
            else
              fprint_msg_or_err(is_msg,"<%02x>",data[ii]);
          }
          if (this_length > 4)
            for (ii=4; ii < this_length; ii++)
              fprint_msg_or_err(is_msg," %02x",data[ii]);
        }
        fprint_msg_or_err(is_msg,"\n");
        break;
      case 9:           // I see this in data, so might as well "explain" it
        fprint_msg_or_err(is_msg,"Conditional access: ");
        temp_u = (data[0] << 8) | data[1];
        fprint_msg_or_err(is_msg,"id %04x (%d) ",temp_u,temp_u);
        temp_u = ((data[2] & 0x1F) << 8) | data[3];
        fprint_msg_or_err(is_msg,"PID %04x (%d) ",temp_u,temp_u);
        if (data_len > 4)
          print_data(is_msg,"data",&data[4],data_len-4,data_len-4);
        else
          fprint_msg_or_err(is_msg,"\n");
        break;
      case 10:            // We'll assume the length is a multiple of 4
        fprint_msg_or_err(is_msg,"Languages: ");
        for (ii = 0; ii < this_length/4; ii++)
        {
          byte audio_type;
          if (ii > 0) fprint_msg_or_err(is_msg,", ");
          fprint_msg_or_err(is_msg,"%c",*(data+(ii*4)+0));
          fprint_msg_or_err(is_msg,"%c",*(data+(ii*4)+1));
          fprint_msg_or_err(is_msg,"%c",*(data+(ii*4)+2));
          audio_type = *(data+(ii*4)+3);
          switch (audio_type)
          {
          case 0: /*fprint_msg_or_err(is_msg,"/undefined");*/ break;  // clearer to say nowt?
          case 1: fprint_msg_or_err(is_msg,"/clean effects"); break;
          case 2: fprint_msg_or_err(is_msg,"/hearing impaired"); break;
          case 3: fprint_msg_or_err(is_msg,"/visual impaired commentary"); break;
          default: fprint_msg_or_err(is_msg,"/reserved:0x%02x",audio_type); break;
          }
        }
        fprint_msg_or_err(is_msg,"\n");
        break;

      case 40:
      {
        const uint8_t * p = data;
        unsigned int t;

        fprint_msg_or_err(is_msg,"AVC video descriptor: ");

        if (this_length != 4)
        {
          if (this_length < 4)
          {
            // Give up if too short
            fprint_msg_or_err(is_msg,"### descriptor too short %d ###\n", this_length);
            break;
          }
          else
          {
            // Complain but carry on if too long
            fprint_msg_or_err(is_msg,"### descriptor too long %d ###: \n", this_length);
          }
        }

        fprint_msg_or_err(is_msg,"profile idc: %d, ", *p++);
        fprint_msg_or_err(is_msg,"constraint_set[");
        t = *p;
        for (ii = 0; ii != 6; ++ii)
        {
          fprint_msg_or_err(is_msg,"%c", (t & 0x80) != 0 ? '0' + ii : '-');
          t <<= 1;
        }
        fprint_msg_or_err(is_msg,"], AVC_compatible_flags: %d, ", *p++ & 3);
        fprint_msg_or_err(is_msg,"level_idc: %d, ", *p++);
        fprint_msg_or_err(is_msg,"AVC_still_present: %d, ", (*p >> 7) & 1);
        fprint_msg_or_err(is_msg,"AVC_24_hour_picture_flag: %d, ", (*p >> 6) & 1);
        fprint_msg_or_err(is_msg,"Frame_Packing_SEI_not_present_flag: %d, ", (*p >> 5) & 1);
        fprint_msg_or_err(is_msg,"reserved: %#x", *p & 0x1f);

        fprint_msg_or_err(is_msg,"\n");
        break;
      }

      case 42:
        {
          const uint8_t * p = data;
          fprint_msg_or_err(is_msg,"AVC timing and HRD descriptor: ");
          fprint_msg_or_err(is_msg,"hrd_management_valid_flag: %d, ", (*p & 0x80) != 0);
          if ((*p & 0x7e) != 0x7e)
          {
            fprint_msg_or_err(is_msg,"reserved: %#x, ", (*p & 0x7e) >> 1);
          }
          if ((*p++ & 1) != 0)  // picture_and_timing_info_present
          {
            int flag90 = *p >> 7;
            uint32_t n = 1, k = 300;
            uint32_t ntick;

            if (flag90)
            {
              fprint_msg_or_err(is_msg,"90kHz_flag, ", *p & 0x7f);
            }
            if ((*p & 0x7f) != 0x7f)
            {
              fprint_msg_or_err(is_msg,"reserved: %#x, ", *p & 0x7f);
            }
            ++p;
            if (!flag90)
            {
              n = uint_32_be(p);
              p += 4;
              k = uint_32_be(p);
              p += 4;
              fprint_msg_or_err(is_msg,"N/K: %u/%u, ", n, k);
            }
            ntick = uint_32_be(p);
            p += 4;
            fprint_msg_or_err(is_msg,"num_units_in_tick: %u, ", ntick);
            if (k == 0 || ntick == 0)
              fprint_msg_or_err(is_msg,"(frame rate: \?\?\?), ");
            else
              fprint_msg_or_err(is_msg,"(frame rate: %.6g), ", ((double)n * 27000000.0) / ((double)k * (double)ntick) / 2.0);
          }
          fprint_msg_or_err(is_msg,"fixed_frame_rate_flag: %u, ", *p >> 7);
          fprint_msg_or_err(is_msg,"temporal_poc_flag: %u, ", (*p >> 6) & 1);
          fprint_msg_or_err(is_msg,"picture_to_display_conversion_flag: %u", (*p >> 5) & 1);
          if ((*p & 0x1f) != 0x1f)
          {
            fprint_msg_or_err(is_msg,", reserved: %#x", *p & 0x1f);
          }
          fprint_msg_or_err(is_msg,"\n");
        }
        break;

      case 52:
        fprint_msg_or_err(is_msg,"MPEG2 stereoscopic video format: ");

        if (this_length != 1)
        {
          if (this_length < 1)
          {
            // Give up if too short
            fprint_msg_or_err(is_msg,"### descriptor too short %d ###\n", this_length);
            break;
          }
          else
          {
            // Complain but carry on if too long
            fprint_msg_or_err(is_msg,"### descriptor too long %d ###: \n", this_length);
          }
        }

        if ((data[0] & 0x80) != 0)
        {
          fprint_msg_or_err(is_msg,"arrangement not present: reserved: %#x", data[0] & 0x7f);
        }
        else
        {
          fprint_msg_or_err(is_msg,"arrangement: ");
          switch (data[0])
          {
          case 3:
            fprint_msg_or_err(is_msg,"S3D side by side");
            break;
          case 4:
            fprint_msg_or_err(is_msg,"S3D top and bottom");
            break;
          case 8:
            fprint_msg_or_err(is_msg,"2D video");
            break;
          default:
            fprint_msg_or_err(is_msg,"reserved: %#x", data[0] & 0x7f);
            break;
          }
        }
        fprint_msg_or_err(is_msg,"\n");
        break;

      case 56:
        print_HEVC_descriptor(is_msg, data, this_length);
        break;

      case 0x56:  // teletext
        for (ii = 0; ii < this_length; ii += 5)
        {
          int jj;
          int teletext_type, teletext_magazine, teletext_page;
          if (ii == 0)
            fprint_msg_or_err(is_msg,"Teletext: ");
          else
          {
            if (leader1 != NULL) fprint_msg_or_err(is_msg,"%s",leader1);
            if (leader2 != NULL) fprint_msg_or_err(is_msg,"%s",leader2);
            fprint_msg_or_err(is_msg,"          ");
          }
          fprint_msg_or_err(is_msg,"language=");
          for (jj=ii; jj<ii+3; jj++)
          {
            if (isprint(data[jj]))
              fprint_msg_or_err(is_msg,"%c",data[jj]);
            else
              fprint_msg_or_err(is_msg,"<%02x>",data[jj]);
          }
          teletext_type = (data[ii+3] & 0xF8) >> 3;
          teletext_magazine = (data[ii+3] & 0x07);
          teletext_page = data[ii+4];
          fprint_msg_or_err(is_msg,", type=");
          switch (teletext_type)
          {
          case 1: fprint_msg_or_err(is_msg,"Initial"); break;
          case 2: fprint_msg_or_err(is_msg,"Subtitles"); break;
          case 3: fprint_msg_or_err(is_msg,"Additional info"); break;
          case 4: fprint_msg_or_err(is_msg,"Programme schedule"); break;
          case 5: fprint_msg_or_err(is_msg,"Hearing impaired subtitles"); break;
          default: fprint_msg_or_err(is_msg,"%x (reserved)",teletext_type); break;
          }
          fprint_msg_or_err(is_msg,", magazine %d, page %x",teletext_magazine,teletext_page);
          fprint_msg_or_err(is_msg,"\n");
        }
        break;

      case 0x59:
      {
        fprint_msg_or_err(is_msg, "subtitling_descriptor(s):\n");

        for (ii = 0; ii + 8 <= this_length; ii += 8)
        {
          char lang[4];
          unsigned int subtitling_type = data[ii + 3];
          unsigned int composition_page_id = (data[ii + 4] << 8) | data[ii + 5];
          unsigned int ancillary_page_id = (data[ii + 6] << 8) | data[ii + 7];
          lang[0] = data[ii + 0];
          lang[1] = data[ii + 1];
          lang[2] = data[ii + 2];
          lang[3] = 0;
          if (leader1 != NULL) fprint_msg_or_err(is_msg,"%s",leader1);
          if (leader2 != NULL) fprint_msg_or_err(is_msg,"%s",leader2);
          fprint_msg_or_err(is_msg,"  language='%s', subtitling_type=%u\n",
            lang, subtitling_type);
          if (leader1 != NULL) fprint_msg_or_err(is_msg,"%s",leader1);
          if (leader2 != NULL) fprint_msg_or_err(is_msg,"%s",leader2);
          fprint_msg_or_err(is_msg, "    (%s)\n", dvb_component_type3_str(subtitling_type));
          if (leader1 != NULL) fprint_msg_or_err(is_msg,"%s",leader1);
          if (leader2 != NULL) fprint_msg_or_err(is_msg,"%s",leader2);
          fprint_msg_or_err(is_msg, 
            "  composition_page_id=%u, ancillary_page_id=%u\n",
            composition_page_id, ancillary_page_id);
        }
        if (ii < this_length)
          fprint_msg_or_err(is_msg, "### %d spare bytes at end of descriptor\n", this_length - ii);
        break;
      }

      case 0x6A:
        print_data(is_msg,"DVB AC-3",data,this_length,100);
        break;
      case 0x81:
//        print_data(is_msg,"ATSC AC-3",data,this_length,100);
        fprint_msg_or_err(is_msg, "ATSC AC-3: ");
        print_ac3_audio_descriptor(is_msg, data, this_length);
        break;

      default:
        {
          char    temp_c[50]; // twice as much as I need...
          snprintf(temp_c, sizeof(temp_c), "%s (%d)",
            tag < sizeof(descriptor_names)/sizeof(descriptor_names[0]) ?
                descriptor_names[tag] :
              tag < 64 ? "Reserved" : "User Private",
            tag);
          print_data(is_msg,temp_c,data,this_length,100);
        }
        break;
      }
    }
    data_len -= this_length;
    data += this_length;
  }
  return 0;
}

/*
 * Given a TS packet, extract the (next bit of) a PAT/PMT's data.
 *
 * - if `verbose`, then report on what we're doing
 * - `payload` is the payload of the current TS packet. We know it can't be
 *   more than 184 bytes long, because of the packet header bytes.
 * - regardless, `payload_len` is the actual length of the payload.
 * - `pid` is the PID of this TS packet.
 * - `data` is the data array for the whole of the data of this PSI.
 *   If it is passed as NULL, then the TS packet must be the first for
 *   this PSI, and this function will malloc an array of the appropriate
 *   length (and return it here). If it is non-NULL, then it is partially
 *   full.
 * - `data_len` is the actual length of the `data` array -- if `data` is NULL
 *   then this will be set by the function.
 * - `data_used` is how many bytes of data are already in the `data` array.
 *   This will be updated by this function - if it is returned as equal to
 *   `data_len`, then the PAT/PMT packet data is complete.
 *
 * Usage:
 *  
 *  If a PSI packet has PUSI set, then it is the first packet of said PSI
 *  (which, for our purposes, means PAT or PMT). If it does not, then it
 *  is a continuation. If PUSI was set, call this with ``data`` NULL, otherwise
 *  pass it some previous data to continue.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int build_psi_data(int            verbose,
                          byte           payload[MAX_TS_PAYLOAD_SIZE],
                          int            payload_len,
                          uint32_t       pid,
                          byte         **data,
                          int           *data_len,
                          int           *data_used)
{
  byte   *packet_data;
  int     packet_data_len;
  int     pointer;
  int     section_length;

  if (payload_len == 0)
  {
    print_err("### PMT payload has zero length\n");
    return 1;
  }
  if (payload == NULL)
  {
    print_err("### PMT payload is NULL\n");
    return 1;
  }

  if (*data == NULL)
  {
    // We have the first section of a PSI packet, which contains the pointer
    // field - deal with it
    pointer = payload[0];

    if (pointer > (payload_len - 1))
    {
      fprint_err("### PMT payload: pointer is %d, which is off the end of"
                 " the packet (length %d)\n",pointer,payload_len);
      return 1;
    }

    // if (DEBUG) print_data(TRUE,"PMT",payload,payload_len,1000);
    packet_data = payload + pointer + 1;
    packet_data_len = payload_len - pointer - 1;
    if (DEBUG) print_data(TRUE,"Data",packet_data,packet_data_len,1000);

    section_length = ((packet_data[1] & 0xF) << 8) | packet_data[2];

#if 0 // XXX
      print_msg("===========================================\n");
      print_data(TRUE,"build_pmt_data(new)",packet_data,packet_data_len,packet_data_len);
#endif

    *data_len = section_length + 3;
    // Beware - if our PMT is shorter than our TS packet, we only want to
    // "use" the data that belongs to our PMT, not the rest of the packet
    // (which is hopefully full of 0xFF anyway)
    // We want to get this right because our callers decide if they've
    // finished reading a PMT by comparing data_used with data_len.
    if (packet_data_len > *data_len)
      *data_used = *data_len;
    else
      *data_used = packet_data_len;
    *data = malloc(*data_len);
    if (*data == NULL)
    {
      print_err("### Unable to malloc PSI data array\n");
      return 1;
    }
    memcpy(*data,packet_data,*data_len);
  }
  else
  {
    // This is a continuation of a PSI packet - it doesn't contain a pointer
    // field, so our data is just data
    int space_left = *data_len - *data_used;
    packet_data = payload;
    packet_data_len = payload_len;
    if (DEBUG) print_data(TRUE,"Data",packet_data,packet_data_len,1000);

#if 0 // XXX
    print_msg("===========================================\n");
    print_data(TRUE,"build_pmt_data(old)",packet_data,packet_data_len,100);
#endif
    if (space_left > packet_data_len)
    {
      // We have more than enough room - use all of this packet
      memcpy(*data + *data_used, packet_data, packet_data_len);
      *data_used += packet_data_len;
    }
    else
    {
      // We have more than enough data - use what we need
      // (we assume the rest will be 0xFF padded, but shan't check)
      memcpy(*data + *data_used, packet_data, space_left);
      *data_used += space_left;
    }
  }
  return 0;
}

/*
 * Extract the program map table from a PMT packet.
 *
 * Handles the result of calling build_psi_data() for this PMT.
 *
 * - if `verbose`, then report on what we're doing
 * - `data` is the data for the PMT packet.
 * - `data_len` is the length of said data.
 * - `pid` is the PID of this PMT
 * - `pmt` is the new PMT datastructure
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int extract_pmt(int            verbose,
                       byte           data[],
                       int            data_len,
                       uint32_t       pid,
                       pmt_p         *pmt)
{
  int     table_id;
  int     section_syntax_indicator,zero_bit,reserved;
  uint16_t program_number;
  uint32_t pcr_pid;
  int     section_length;
  int     version_number;
  int     current_next_indicator;
  int     section_number;
  int     last_section_number;
  int     program_info_length;
  uint32_t crc = 0;
  uint32_t check_crc;
  byte   *stream_data;
  int     stream_data_len;
  int     err;

  if (data_len == 0)
  {
    print_err("### PMT data has zero length\n");
    return 1;
  }
  if (data == NULL)
  {
    print_err("### PMT data is NULL\n");
    return 1;
  }

  if (DEBUG) print_data(TRUE,"Data",data,data_len,1000);

  // Check the table id (maybe this should be done by our caller?)
  table_id = data[0];
  if (table_id != 2)
  {
    // The table_id for a PMT is 2.
    // A PAT may also reference user private tables, and I've seen data with
    // other table values (including FF) as well:
    if (0x03 <= table_id && table_id <=0xFE)  // user private table
    {
      if (verbose)
      {
        fprint_msg("    'PMT' with PID %04x is user private table %02x\n",pid,table_id);
        print_data(TRUE,"    Data",data,data_len,20);
      }
    }
    else
    {
      if (0x03 <= table_id && table_id <= 0x3F)
        fprint_err("### PMT table id is %0#x (H.222 / ISO/IEC 13818-1"
                   " reserved), should be 2\n",table_id);
      else
        fprint_err("### PMT table id is %0#x (%s), should be 2\n",
                   table_id,(table_id==0x00?"PAT":
                          table_id==0x01?"CAT":
                          table_id==0xFF?"Forbidden":"???"));
      print_data(FALSE,"    Data",data,data_len,20);
    }
    // Best we can do is to pretend it didn't happen
    *pmt = build_pmt(0,0,0);  // empty "PMT" with program number 0, PCR PID 0
    if (*pmt == NULL) return 1;
    return 0;
  }

  // Check bits
  section_syntax_indicator = (data[1] & 0x80) >> 7;
  zero_bit = (data[1] & 0x40) >> 6;
  reserved = (data[1] & 0x30) >> 4;
  if (section_syntax_indicator != 1 && report_bad_reserved_bits)
    print_err("!!! PMT: section syntax indicator is 0, not 1\n");
  if (zero_bit != 0 && report_bad_reserved_bits)
    print_err("!!! PMT: zero bit is 1, not 0\n");
  if (reserved != 3 && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after zero bit) is %d, not 3\n",reserved);

  section_length = ((data[1] & 0xF) << 8) | data[2];
  if (verbose)
    fprint_msg("  section length:  %03x (%d)\n",section_length,section_length);

  // If the section length doesn't match our data length, we've got problems
  // (remember, the section_length counts bytes after the section_length field)
  if (section_length > data_len - 3)
  {
    fprint_err("### PMT section length %d is more than"
               " length of remaining data %d\n",section_length,data_len-3);
    return 1;
  }
  else if (section_length < data_len - 3)
  {
    fprint_err("!!! PMT section length %d does not use all of"
               " remaining data %d\n",section_length,data_len-3);
    // Adjust it and carry on
    data_len = section_length + 3;
  }

  program_number = (data[3] << 8) | data[4];
  if (verbose)
    fprint_msg("  program number: %04x\n",program_number);
  reserved = (data[5] & 0xC0) >> 6;
  if (reserved != 3 && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after program_number)"
               " is %d, not 3\n",reserved);
  version_number = (data[5] & 0x3E) >> 1;
  current_next_indicator = data[5] & 0x1;
  section_number = data[6];
  last_section_number = data[7];
  if (verbose)
    fprint_msg("  version number %02x, current next %x, section number %x, last"
               " section number %x\n",version_number,current_next_indicator,
           section_number,last_section_number);

  reserved = (data[8] & 0xE0) >> 5;
  if (reserved != 7 && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after last_section_number)"
               " is %d, not 7\n",reserved);
  pcr_pid = ((data[8] & 0x1F) << 8) | data[9];
  if (verbose)
    fprint_msg("  PCR PID: %04x\n",pcr_pid);

  reserved = (data[10] & 0xF0) >> 4;
  if (reserved != 0xF && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after PCR PID)"
               " is %x, not F\n",reserved);

  program_info_length = ((data[10] & 0x0F) << 8) | data[11];
  if (verbose)
    fprint_msg("  program info length: %d\n",program_info_length);

  if (verbose && program_info_length > 0)
  {
    print_msg("  Program info:\n");
    print_descriptors(TRUE,"    ",NULL,&data[12],program_info_length);
  }

  // 32 bits at the end of a program association section is reserved for a CRC
  // (OK, let's extract it stupidly...)
  crc = (crc << 8) | data[data_len-4];
  crc = (crc << 8) | data[data_len-3];
  crc = (crc << 8) | data[data_len-2];
  crc = (crc << 8) | data[data_len-1];

  // Let's check the CRC
  check_crc = crc32_block(0xffffffff,data,data_len);
  if (check_crc != 0)
  {
    fprint_err("!!! Calculated CRC for PMT (PID %04x) is %08x, not 00000000"
               " (CRC in data was %08x)\n",pid,check_crc,crc);
    // Should we carry on or give up (if "give up", then "!!!" should be "###").
    //return 1;
  }

  // So we can work out the length of the actual program data
  // (remember the section length is for the bytes *after* the section
  // length field, so for data[3...])
  stream_data = data + 12 + program_info_length;
  stream_data_len = data_len - 12 - program_info_length - 4; // "-4" == CRC

  //print_data(TRUE,"Rest:",stream_data,stream_data_len,1000);

  *pmt = build_pmt(program_number,version_number,pcr_pid);
  if (*pmt == NULL) return 1;

  if (program_info_length > 0)
  {
    err = set_pmt_program_info(*pmt,program_info_length,&data[12]);
    if (err)
    {
      free_pmt(pmt);
      return 1;
    }
  }

  if (verbose)
    print_msg("  Program streams:\n");
  while (stream_data_len > 0)
  {
    int stream_type = stream_data[0];
    uint32_t pid = ((stream_data[1] & 0x1F) << 8) | stream_data[2];
    int ES_info_length =  ((stream_data[3] & 0x0F) << 8) | stream_data[4];
    if (verbose)
    {
      fprint_msg("    PID %04x -> Stream %02x %s\n",pid,stream_type,
                 h222_stream_type_str(stream_type));
      if (ES_info_length > 0)
        print_descriptors(TRUE,"        ",NULL,&stream_data[5],ES_info_length);
    }
    err = add_stream_to_pmt(*pmt,pid,stream_type,ES_info_length,
                            stream_data+5);
    if (err)
    {
      free_pmt(pmt);
      return 1;
    }
    stream_data = stream_data + 5 + ES_info_length;
    stream_data_len = stream_data_len - 5 - ES_info_length;
  }
  return 0;
}

/*
 * Extract the stream list (and PCR PID) from a PMT packet.
 *
 * Assumes that the whole content of the PMT is in this single packet.
 *
 * - if `verbose`, then report on what we're doing
 * - `payload` is the payload of the TS packet. We know it can't be more
 *   than 184 bytes long, because of the packet header bytes.
 * - regardless, `payload_len` is the actual length of the payload.
 * - `pid` is the PID of this TS packet.
 * - `program_number` is the program number.
 * - `pcr_pid` is the PID of packets containing the PCR, or 0.
 * - `stream_list` is a list of stream versus PID.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int extract_stream_list_from_pmt(int            verbose,
                                        byte           payload[MAX_TS_PAYLOAD_SIZE],
                                        int            payload_len,
                                        uint32_t       pid,
                                        int           *program_number,
                                        uint32_t      *pcr_pid,
                                        pidint_list_p *stream_list)
{
  byte   *data;
  int     data_len;
  int     pointer;
  int     table_id;
  int     section_syntax_indicator,zero_bit,reserved;
  int     section_length;
  int     version_number;
  int     current_next_indicator;
  int     section_number;
  int     last_section_number;
  int     program_info_length;
  uint32_t crc = 0;
  uint32_t check_crc;
  byte   *stream_data;
  int     stream_data_len;
  int     err;

  if (payload_len == 0)
  {
    print_err("### PMT payload has zero length\n");
    return 1;
  }
  if (payload == NULL)
  {
    print_err("### PMT payload is NULL\n");
    return 1;
  }
  pointer = payload[0];

  if (pointer > (payload_len - 1))
  {
    fprint_err("### PMT payload: pointer is %d, which is off the end of"
               " the packet (length %d)\n",pointer,payload_len);
    return 1;
  }

  // if (DEBUG) print_data(TRUE,"PMT",payload,payload_len,1000);
  data = payload + pointer + 1;
  data_len = payload_len - pointer - 1;
  if (DEBUG) print_data(TRUE,"Data",data,data_len,1000);

  // Check the table id (maybe this should be done by our caller?)
  table_id = data[0];
  if (table_id != 2)
  {
    // The table_id for a PMT is 2.
    // A PAT may also reference user private tables, and I've seen data with
    // other table values (including FF) as well:
    if (0x03 <= table_id && table_id <=0xFE)  // user private table
    {
      if (verbose)
      {
        fprint_msg("    'PMT' with PID %04x is user private table %02x\n",pid,table_id);
        print_data(TRUE,"    Data",data,data_len,20);
      }
    }
    else
    {
      if (0x03 <= table_id && table_id <= 0x3F)
        fprint_err("### PMT table id is %0#x (H.222 / ISO/IEC 13818-1"
                   " reserved), should be 2\n",table_id);
      else
        fprint_err("### PMT table id is %0#x (%s), should be 2\n",
                   table_id,(table_id==0x00?"PAT":
                             table_id==0x01?"CAT":
                             table_id==0xFF?"Forbidden":"???"));
      print_data(FALSE,"    Data",data,data_len,20);
    }
    // Best we can do is to pretend it didn't happen
    *program_number = 0;
    *pcr_pid = 0;
    *stream_list = NULL;
    return 0;
  }

  // Check bits
  section_syntax_indicator = (data[1] & 0x80) >> 7;
  zero_bit = (data[1] & 0x40) >> 6;
  reserved = (data[1] & 0x30) >> 4;
  if (section_syntax_indicator != 1 && report_bad_reserved_bits)
    print_err("!!! PMT: section syntax indicator is 0, not 1\n");
  if (zero_bit != 0 && report_bad_reserved_bits)
    print_err("!!! PMT: zero bit is 1, not 0\n");
  if (reserved != 3 && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after zero bit) is %d, not 3\n",reserved);

  section_length = ((data[1] & 0xF) << 8) | data[2];
  if (verbose)
    fprint_msg("  section length:   %03x (%d)\n",section_length,section_length);

  // If the section length continues into another packet, we're not going
  // to cope with it. Otherwise, we need to adjust our idea of how long
  // the data we want to "read" is.
  if (section_length + 3 > data_len)
  {
    fprint_err("### PMT continues into another packet - section length %d,"
               " remaining packet data length %d\n",
               section_length,data_len-3);
    fprint_err("    This software does not support PMT data spanning"
               " multiple TS packets\n");
    return 1;
  }
  data_len = section_length + 3;

  *program_number = (data[3] << 8) | data[4];
  if (verbose)
    fprint_msg("  program number: %04x\n",*program_number);
  reserved = (data[5] & 0xC0) >> 6;
  if (reserved != 3 && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after program_number)"
               " is %d, not 3\n",reserved);
  version_number = (data[5] & 0x3E) >> 1;
  current_next_indicator = data[5] & 0x1;
  section_number = data[6];
  last_section_number = data[7];
  if (verbose)
    fprint_msg("  version number %02x, current next %x, section number %x, last"
               " section number %x\n",version_number,current_next_indicator,
               section_number,last_section_number);

  reserved = (data[8] & 0xE0) >> 5;
  if (reserved != 7 && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after last_section_number)"
               " is %d, not 7\n",reserved);
  *pcr_pid = ((data[8] & 0x1F) << 8) | data[9];
  if (verbose)
    fprint_msg("  PCR PID: %04x\n",*pcr_pid);

  reserved = (data[10] & 0xF0) >> 4;
  if (reserved != 0xF && report_bad_reserved_bits)
    fprint_err("!!! PMT: reserved (after PCR PID)"
               " is %x, not F\n",reserved);

  program_info_length = ((data[10] & 0x0F) << 8) | data[11];
  if (verbose)
    fprint_msg("  program info length: %d\n",program_info_length);

  if (verbose && program_info_length > 0)
  {
    print_msg("  Program info:\n");
    print_descriptors(TRUE,"    ",NULL,&data[12],program_info_length);
  }

  // 32 bits at the end of a program association section is reserved for a CRC
  // (OK, let's extract it stupidly...)
  crc = (crc << 8) | data[data_len-4];
  crc = (crc << 8) | data[data_len-3];
  crc = (crc << 8) | data[data_len-2];
  crc = (crc << 8) | data[data_len-1];

  // Let's check the CRC
  check_crc = crc32_block(0xffffffff,data,data_len);
  if (check_crc != 0)
  {
    fprint_err("!!! Calculated CRC for PMT (PID %04x) is %08x, not 00000000"
               " (CRC in data was %08x)\n",pid,check_crc,crc);
    return 1;
  }

  // So we can work out the length of the actual program data
  // (remember the section length is for the bytes *after* the section
  // length field, so for data[3...])
  stream_data = data + 12 + program_info_length;
  stream_data_len = data_len - 12 - program_info_length - 4; // "-4" == CRC

  //print_data(TRUE,"Rest:",stream_data,stream_data_len,1000);

  err = build_pidint_list(stream_list);
  if (err) return 1;

  if (verbose)
    print_msg("  Program streams:\n");
  while (stream_data_len > 0)
  {
    int stream_type = stream_data[0];
    uint32_t pid = ((stream_data[1] & 0x1F) << 8) | stream_data[2];
    int ES_info_length =  ((stream_data[3] & 0x0F) << 8) | stream_data[4];
    if (verbose)
    {
#define SARRAYSIZE 40
      char buf[SARRAYSIZE];
      snprintf(buf,SARRAYSIZE,"(%s)",h222_stream_type_str(stream_type));
      // On Windows, snprintf does not guarantee to write a terminating NULL
      buf[SARRAYSIZE-1] = '\0';
      fprint_msg("    Stream %02x %-40s -> PID %04x\n",stream_type,buf,pid);
      if (ES_info_length > 0)
        print_descriptors(TRUE,"        ",NULL,&stream_data[5],ES_info_length);
    }
    // For the moment, we shan't bother to remember the extra info.
    err = append_to_pidint_list(*stream_list,pid,stream_type);
    if (err) return 1;
    stream_data = stream_data + 5 + ES_info_length;
    stream_data_len = stream_data_len - 5 - ES_info_length;
  }
  return 0;
}

/*
 * Split a TS packet into its main parts
 *
 * - `buf` is the data for the packet
 * - `pid` is the PID of said data
 * - `payload_unit_start_indicator` is TRUE if any payload in this
 *   packet forms the start of a PES packet. Its meaning is not significant
 *   if there is no payload, or if the payload is not (part of) a PES packet.
 * - `adapt` is an offset into `buf`, acting as an array of the actual
 *   adaptation control bytes. It will be NULL if there are no adaptation
 *   controls.
 * - `adapt_len` is the length of the adaptation controls (i.e., the
 *   number of bytes). It will be 0 if there are no adaptation controls.
 * - `payload` is an offset into `buf`, acting as an array of the actual
 *   payload bytes. It will be NULL if there is no payload.
 * - `payload_len` is the length of the payload *in this packet* (i.e., the
 *   number of bytes. It will be 0 if there is no payload.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int split_TS_packet(byte      buf[TS_PACKET_SIZE],
                           uint32_t *pid,
                           int      *payload_unit_start_indicator,
                           byte     *adapt[],
                           int      *adapt_len,
                           byte     *payload[],
                           int      *payload_len)
{
  int     adaptation_field_control;

  if (buf[0] != 0x47)
  {
    fprint_err("### TS packet starts %02x, not %02x\n",buf[0],0x47);
    return 1;
  }
  *payload_unit_start_indicator = (buf[1] & 0x40) >> 6;
  *pid = ((buf[1] & 0x1f) << 8) | buf[2];

  if (*pid == 0x1FFF)
  {
    // Null packets don't contain any data, so let's not allow "spurious"
    // interpretation of their innards
    *adapt = NULL;
    *adapt_len = 0;
    *payload = NULL;
    *payload_len = 0;
    return 0;
  }
  
  adaptation_field_control = (buf[3] & 0x30) >> 4;
  switch (adaptation_field_control)
  {
  case 0:
    fprint_err("### Packet PID %04x has adaptation field control = 0\n"
               "    which is a reserved value (no payload, no adaptation field)\n",
               *pid);
    *adapt = NULL;
    *adapt_len = 0;
    *payload = NULL;
    *payload_len = 0;
    break;
  case 1:
    // Payload only
    *adapt = NULL;
    *adapt_len = 0;
    *payload = buf + 4;
    *payload_len = TS_PACKET_SIZE - 4;
    break;
  case 2:
    // Adaptation field only
    *adapt_len = buf[4];
    if (*adapt_len == 0)
      *adapt = NULL;
    else
      *adapt = buf + 5;
    *payload = NULL;
    *payload_len = 0;
    break;
  case 3:
    // Payload and adaptation field
    *adapt_len = buf[4];
    if (*adapt_len == 0)
      *adapt = NULL;
    else
      *adapt = buf + 5;
    *payload = buf + 5 + buf[4];
    *payload_len = TS_PACKET_SIZE - 5 - buf[4];
    break;
  default:
    // How this might occur, other than via program error, I can't think.
    fprint_err("### Packet PID %04x has adaptation field control %x\n",
               *pid,adaptation_field_control);
    return 1;
  }
  return 0;
}

/*
 * Return the next TS packet, as payload and adaptation controls.
 *
 * This is a convenience wrapping of `read_next_TS_packet` and
 * `split_TS_packet`. Because of this, the data referenced by `adapt` and
 * `payload` will generally not persist over further calls of this function
 * and `read_next_TS_packet`, as it is held within the TS reader's read-ahead
 * buffer.
 *
 * - `tsreader` is the TS packet reading context
 * - `pid` is the PID of said data
 * - `payload_unit_start_indicator` is TRUE if any payload in this
 *   packet forms the start of a PES packet. Its meaning is not significant
 *   if there is no payload, or if the payload is not (part of) a PES packet.
 * - `adapt` is an offset into `buf`, acting as an array of the actual
 *   adaptation control bytes. It will be NULL if there are no adaptation
 *   controls.
 * - `adapt_len` is the length of the adaptation controls (i.e., the
 *   number of bytes). It will be 0 if there are no adaptation controls.
 * - `payload` is an offset into `buf`, acting as an array of the actual
 *   payload bytes. It will be NULL if there is no payload.
 * - `payload_len` is the length of the payload *in this packet* (i.e., the
 *   number of bytes. It will be 0 if there is no payload.
 *
 * Returns 0 if all went well, EOF if there is no more data, 1 if something
 * went wrong.
 */
extern int get_next_TS_packet(TS_reader_p  tsreader,
                              uint32_t    *pid,
                              int         *payload_unit_start_indicator,
                              byte        *adapt[],
                              int         *adapt_len,
                              byte        *payload[],
                              int         *payload_len)
{
  int    err;
  byte  *packet;

  err = read_next_TS_packet(tsreader,&packet);
  if (err == EOF)
    return EOF;
  else if (err)
  {
    print_err("### Error reading TS packet\n");
    return 1;
  }
  return split_TS_packet(packet,pid,payload_unit_start_indicator,
                         adapt,adapt_len,payload,payload_len);
}

/*
 * Find the first (next) PAT.
 *
 * - `tsreader` is the TS packet reading context
 * - if `max` is non-zero, then it is the maximum number of TS packets to read
 * - if `verbose` is true, then output extra information
 * - if `quiet` is true, then don't output normal informational messages
 * - `num_read` is the number of packets read to find the PAT (or before
 *   giving up)
 * - `prog_list` is the program list from the PAT, or NULL if none was found
 *
 * Returns 0 if all went well, EOF if no PAT was found,
 * 1 if something else went wrong.
 */
extern int find_pat(TS_reader_p     tsreader,
                    int             max,
                    int             verbose,
                    int             quiet,
                    int            *num_read,
                    pidint_list_p  *prog_list)
{
  int    err;
  byte  *pat_data = NULL;
  int    pat_data_len = 0;
  int    pat_data_used = 0;

  *prog_list = NULL;
  *num_read  = 0;
  if (!quiet) print_msg("Locating first PAT\n");
  
  for (;;)
  {
    uint32_t pid;
    int      payload_unit_start_indicator;
    byte    *adapt, *payload;
    int      adapt_len, payload_len;

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
      return EOF;
    else if (err)
    {
      print_err("### Error reading TS packet\n");
      if (pat_data) free(pat_data);
      return 1;
    }

    (*num_read) ++;

    if (pid == 0x0000)
    {
      if (!quiet)
        fprint_msg("Found PAT after reading %d packet%s\n",
                   *num_read,(*num_read==1?"":"s"));

      if (payload_len == 0)
      {
        print_err("### Packet is PAT, but has no payload\n");
        if (pat_data) free(pat_data);
        return 1;
      }

      if (payload_unit_start_indicator && pat_data)
      {
        // Lose any data we started but didn't complete
        print_err("!!! Discarding previous (uncompleted) PAT data\n");
        free(pat_data);
        pat_data = NULL; pat_data_len = 0; pat_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pat_data)
      {
        print_err("!!! Discarding PAT continuation, no PAT started\n");
        continue;
      }

      err = build_psi_data(verbose,payload,payload_len,pid,
                           &pat_data,&pat_data_len,&pat_data_used);
      if (err)
      {
        fprint_err("### Error %s PAT\n",
                   (payload_unit_start_indicator?"starting new":"continuing"));
        if (pat_data) free(pat_data);
        return 1;
      }

      if (pat_data_len == pat_data_used)
      {
        err = extract_prog_list_from_pat(verbose,pat_data,pat_data_len,prog_list);
        if (pat_data) free(pat_data);
        return err;
      }
    }
    
    if (max > 0 && *num_read >= max)
    {
      if (!quiet) fprint_msg("Stopping after %d TS packets\n",max);
      if (pat_data) free(pat_data);
      return EOF;
    }
  }
}

/*
 * Find the next PMT, and report on it.
 *
 * - `tsreader` is the TS packet reading context
 * - `pmt_pid` is the PID of the PMT we are looking for
 * - if `program_number` is -1, then any PMT with that PID is acceptable,
 *   otherwise we're only interested in a PMT with that PID and the given
 *   program number.
 * - if `max` is non-zero, then it is the maximum number of TS packets to read
 * - if `verbose` is true, then output extra information
 * - if `quiet` is true, then don't output normal informational messages
 * - `num_read` is the number of packets read to find the PMT (or before
 *   giving up)
 * - `pmt` is a new datastructure representing the PMT found
 *
 * Returns 0 if all went well, EOF if no PMT was found,
 * 1 if something else went wrong.
 */
extern int find_next_pmt(TS_reader_p     tsreader,
                         uint32_t        pmt_pid,
                         int             program_number,
                         int             max,
                         int             verbose,
                         int             quiet,
                         int            *num_read,
                         pmt_p		*pmt)
{
  int    err;
  byte  *pmt_data = NULL;
  int    pmt_data_len = 0;
  int    pmt_data_used = 0;

  *pmt = NULL;
  *num_read = 0;
  if (!quiet) print_msg("Locating next PMT\n");

  for (;;)
  {
    uint32_t pid;
    int     payload_unit_start_indicator;
    byte   *adapt, *payload;
    int     adapt_len, payload_len;

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
    {
      if (pmt_data) free(pmt_data);
      return EOF;
    }
    else if (err)
    {
      print_err("### Error reading TS packet\n");
      if (pmt_data) free(pmt_data);
      return 1;
    }

    (*num_read) ++;

    if (pid == pmt_pid)
    {
      if (!quiet)
        fprint_msg("Found %s PMT with PID %04x (%d) after reading %d packet%s\n",
                   (payload_unit_start_indicator?"start of":"more of"),
               pid,pid,*num_read,(*num_read==1?"":"s"));

      if (payload_len == 0)
      {
        fprint_err("### Packet is PMT with PID %04x (%d),"
                   " but has no payload\n",pid,pid);
        if (pmt_data) free(pmt_data);
        return 1;
      }

      if (payload_unit_start_indicator && pmt_data)
      {
        // Lose any data we started but didn't complete
        print_err("!!! Discarding previous (uncompleted) PMT data\n");
        free(pmt_data);
        pmt_data = NULL; pmt_data_len = 0; pmt_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !pmt_data)
      {
        print_err("!!! Discarding PMT continuation, no PMT started\n");
        continue;
      }

      err = build_psi_data(verbose,payload,payload_len,pid,
                           &pmt_data,&pmt_data_len,&pmt_data_used);
      if (err)
      {
        fprint_err("### Error %s PMT\n",
                   (payload_unit_start_indicator?"starting new":"continuing"));
        if (pmt_data) free(pmt_data);
        return 1;
      }

      if (pmt_data_len == pmt_data_used)
      {
        int pmt_program_number;

        err = extract_pmt(verbose,pmt_data,pmt_data_len,pid,pmt);
        pmt_program_number = *pmt == NULL ? -1 : (int)((*pmt)->program_number);

        if (pmt_data)
        {  
          free(pmt_data);
          pmt_data = NULL;
        }

        // Check we've got the right program number - it would appear to be
        // legitimate to have multiple PMTs carried in the same PID and some
        // abuse of this appears to happen in real life
        if (err == 0 && program_number >= 0)
        {
          if (pmt_program_number != program_number)
          {
            fprint_err("!!! Discarding PMT with program number %d\n", pmt_program_number);
            free_pmt(pmt);
            continue;
          }
        }
        return err;
      }
    }

    if (max > 0 && *num_read >= max)
    {
      if (!quiet) fprint_msg("Stopping after %d TS packets\n",max);
      if (pmt_data) free(pmt_data);
      return EOF;
    }
  }
}

/*
 * Find the next PAT, and from that the next PMT.
 *
 * Looks for the next PAT in the input stream, and then for the first
 * PMT thereafter. If there is more than one program stream in the PAT,
 * it looks for the PMT for the first.
 *
 * - `tsreader` is the TS packet reading context
 * - if `max` is non-zero, then it is the maximum number of TS packets to read
 * - if `verbose` is true, then output extra information
 * - if `quiet` is true, then don't output normal informational messages
 * - `num_read` is the number of packets read to find the PMT (or before
 *   giving up)
 * - `pmt` is a new datastructure containing the information from the PMT.
 *
 * Returns 0 if all went well, EOF if no PAT or PMT was found (and thus
 * no program stream), -2 if a PAT was found but it did not contain any
 * programs, 1 if something else went wrong.
 */
extern int find_pmt(TS_reader_p     tsreader,
        const int       req_prog_no,
		    int             max,
		    int             verbose,
		    int             quiet,
		    int            *num_read,
		    pmt_p	   *pmt)
{
  int  err;
  pidint_list_p  prog_list = NULL;
  int            sofar;
  int            prog_index = 0;
  int            prog_no = 0;

  *pmt = NULL;

  err = find_pat(tsreader,max,verbose,quiet,&sofar,&prog_list);

  if (err == EOF)
  {
    if (!quiet) print_msg("No PAT found\n");
    return 1;
  }
  else if (err)
  {
    print_err("### Error finding PAT\n");
    return 1;
  }

  if (!quiet)
  {
    print_msg("\n");
    report_pidint_list(prog_list,"Program list","Program",FALSE);
    print_msg("\n");
  }

  if (prog_list->length == 0)
  {
    if (!quiet) fprint_msg("No programs defined in PAT (packet %d)\n",sofar);
    return -2;
  }
  else if (prog_list->length > 1 && !quiet)
  {
    if (req_prog_no == 1)
      print_msg("Multiple programs in PAT - using the first non-zero\n\n");
    else
      fprint_msg("Multiple programs in PAT - program %d\n\n", req_prog_no);
  }

  for (prog_index = 0; prog_index < prog_list->length; ++prog_index)
  {
    if (prog_list->number[prog_index] == 0)
      continue;
    if (++prog_no == req_prog_no)
      break;
  }

  if (prog_no == 0)
  {
    fprint_msg("No non-zero program_numbers in PAT (packet %d)\n",sofar);
    return -2;
  }
  if (prog_no != req_prog_no)
  {
    fprint_msg("Unable to find program %d in PAT, only found %d (packet %d)\n", req_prog_no, prog_no, sofar);
    return -2;
  }

  // Amend max to take account of the packets we've already read
  max -= sofar;

  err = find_next_pmt(tsreader,prog_list->pid[prog_index],prog_list->number[prog_index],
                      max,verbose,quiet,num_read,pmt);

  free_pidint_list(&prog_list);

  *num_read += sofar;

  if (err == EOF)
  {
    if (!quiet) print_msg("No PMT found\n");
    return EOF;
  }
  else if (err)
  {
    print_err("### Error finding PMT\n");
    return 1;
  }

  if (!quiet)
  {
    print_msg("\n");
    print_msg("Program map\n");
    report_pmt(TRUE,"  ",*pmt);
    print_msg("\n");
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

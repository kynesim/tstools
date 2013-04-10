/*
 * Support for playing (streaming) TS packets.
 *
 * Exposes the functionality in tsplay_innards.c, mainly for use by tsplay.c
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

#ifndef _tsplay_fns
#define _tsplay_fns

#include "tswrite_defns.h"
#include "tsplay_defns.h"

/*
 * Read TS packets and then output them.
 *
 * Assumes (strongly) that it is starting from the start of the file.
 *
 * - `input` is the input stream (descriptor) to read
 * - `tswriter` is our (maybe buffered) writer
 * - if `pid_to_ignore` is non-zero, then any TS packets with that PID
 *   will not be written out (note: any PCR information in them may still
 *   be used)
 * - if `scan_for_PCRs`, use a read-ahead buffer to find the *next* PCR,
 *   and thus allow exact timing of packets.
 * - if we are using the PCR read-ahead buffer, and `override_pcr_pid` is
 *   non-zero, then it is the PID to use for PCRs, ignoring any value found in
 *   a PMT
 * - if `max` is greater than zero, then at most `max` TS packets should
 *   be read from the input
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `quiet` is true, then only error messages should be written out
 * - if `verbose` is true, then give extra progress messages
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int play_TS_stream(int         input,
                          TS_writer_p tswriter,
                          const tsplay_output_pace_mode pace_mode,
                          uint32_t    pid_to_ignore,
                          uint32_t    override_pcr_pid,
                          int         max,
                          int         loop,
                          int         quiet,
                          int         verbose);

/*
 * Read PS packets and then output them as TS.
 *
 * - `input` is the program stream
 * - `output` is the transport stream
 * - `pad_start` is the number of filler TS packets to start the output
 *   with.
 * - `program_repeat` is how often (after how many PS packs) to repeat
 *   the program information (PAT/PMT)
 * - `want_h264` should be true to indicate that the video stream is H.264
 *   (ISO/IEC 14496-2, MPEG-4/AVC), false if it is H.262 (ISO/IEC 13818-3,
 *   MPEG-2, or indeed 11172-3, MPEG-1)
 * - `input_is_dvd` indicates if the PS data came from a DVD, and thus follows
 *   its conventions for private_stream_1 and AC-3/DTS/etc. substreams
 * - `video_stream` indicates which video stream we want - i.e., the stream
 *   with id 0xE0 + <video_stream> - and -1 means the first video stream found.
 * - `audio_stream` indicates which audio stream we want. If `want_ac3_audio`
 *   is false, then this will be the stream with id 0xC0 + <audio_stream>, or,
 *   if it is -1, the first audio stream found.
 * - if `want_ac3_audio` is true, then if `is_dvd` is true, then we want
 *   audio from private_stream_1 (0xBD) with substream id <audio_stream>,
 *   otherwise we ignore `audio_stream` and assume that all data in
 *   private_stream_1 is the audio we want.
 * - `want_dolby_as_dvb` indicates if any Dolby (AC-3) audio data should be output
 *   with DVB or ATSC stream type
 * - `pmt_pid` is the PID of the PMT to write
 * - `pcr_pid` is the PID of the TS unit containing the PCR
 * - `video_pid` is the PID for the video we write
 * - `keep_audio` is true if the audio stream should be output, false if
 *   it should be ignored
 * - `audio_pid` is the PID for the audio we write
 * - if `max` is non-zero, then we want to stop reading after we've read
 *   `max` packets
 * - if `loop`, play the input file repeatedly (up to `max` TS packets
 *   if applicable)
 * - if `verbose` then we want to output diagnostic information
 *   (nb: only applies to first time if looping is enabled)
 * - if `quiet` then we want to be as quiet as we can
 *   (nb: only applies to first time if looping is enabled)
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int play_PS_stream(int          input,
                          TS_writer_p  output,
                          int          pad_start,
                          int          program_repeat,
                          int          force_stream_type,
                          int          want_h262,
                          int          input_is_dvd,
                          int          video_stream,
                          int          audio_stream,
                          int          want_ac3_audio,
                          int          want_dolby_as_dvb,
                          uint32_t     pmt_pid,
                          uint32_t     pcr_pid,
                          uint32_t     video_pid,
                          int          keep_audio,
                          uint32_t     audio_pid,
                          int          max,
                          int          loop,
                          int          verbose,
                          int          quiet);

#endif // tsplay_fns

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

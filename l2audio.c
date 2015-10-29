/*
 * Support for MPEG layer 2 audio streams.
 *
 * (actually, support for
 *
 *    - MPEG-1 audio (described in ISO/IEC 11172-3), layers 1..3
 *    - MPEG-2 audio (described in ISO/IEC 13818-3), layer 2
 *    - unofficial MPEG-2.5
 *
 * but MPEG-2 layer 2 is the main target)
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
#include "misc_fns.h"
#include "printing_fns.h"
#include "l2audio_fns.h"

#define DEBUG 0

// Bitrates by index, according to layer and protocol version
// Note that v3 is actually the mutant V2.5 protocol
static const int bitrate_v1l1[] =
{
    0,  32,  64,  96, 128, 160, 192, 224,
  256, 288, 320, 352, 384, 416, 448,   0
};
static const int bitrate_v1l2[] =
{
    0,  32,  48,  56,  64,  80,  96, 112,
  128, 160, 192, 224, 256, 320, 384,   0
};
static const int bitrate_v1l3[] =
{
    0,  32,  40,  48,  56,  64,  80, 96,
  112, 128, 160, 192, 224, 256, 320,  0
};
static const int bitrate_v2l1[] =
{
    0,  32,  48,  56,  64,  80,  96, 112,
  128, 144, 160, 176, 192, 224, 256,   0
};
static const int bitrate_v2l2[] =
{
   0,  8, 16,  24,  32,  40,  48, 56,
  64, 80, 96, 112, 128, 144, 160,  0
};

static const int * const bitrate_table[3][3] =
{
  { bitrate_v1l1, bitrate_v1l2, bitrate_v1l3 },
  { bitrate_v2l1, bitrate_v2l2, bitrate_v2l2 },
  { bitrate_v2l1, bitrate_v2l2, bitrate_v2l2 }
};

// Sample rates table
static const unsigned int sampling_table[3][3] =
{
  { 44100, 48000, 32000 },
  { 22050, 24000, 16000 },
  { 11025, 12000,  8000 }
};



// Decode frame header information
#define AUD_FRAME_RATE_N_0      96000
#define AUD_FRAME_RATE_N_1      88200
#define AUD_FRAME_RATE_N_2      64000
#define AUD_FRAME_RATE_N_3      48000
#define AUD_FRAME_RATE_N_4      44100
#define AUD_FRAME_RATE_N_5      32000
#define AUD_FRAME_RATE_N_6      24000
#define AUD_FRAME_RATE_N_7      22050
#define AUD_FRAME_RATE_N_8      16000
#define AUD_FRAME_RATE_N_9      12000
#define AUD_FRAME_RATE_N_10     11025
#define AUD_FRAME_RATE_N_11     8000
#define AUD_FRAME_RATE_N_12     7350
#define AUD_FRAME_RATE_N_13     0
#define AUD_FRAME_RATE_N_14     0
#define AUD_FRAME_RATE_N_15     0

const unsigned int aud_frame_rate_n[16] =
{
  AUD_FRAME_RATE_N_0,
  AUD_FRAME_RATE_N_1,
  AUD_FRAME_RATE_N_2,
  AUD_FRAME_RATE_N_3,
  AUD_FRAME_RATE_N_4,
  AUD_FRAME_RATE_N_5,
  AUD_FRAME_RATE_N_6,
  AUD_FRAME_RATE_N_7,
  AUD_FRAME_RATE_N_8,
  AUD_FRAME_RATE_N_9,
  AUD_FRAME_RATE_N_10,
  AUD_FRAME_RATE_N_11,
  AUD_FRAME_RATE_N_12,
  AUD_FRAME_RATE_N_13,
  AUD_FRAME_RATE_N_14,
  AUD_FRAME_RATE_N_15
};

/*
 * Look at a frame header and try to deduce the length of the frame.
 *
 * Returns the frame length deduced therefrom, or -1 if it finds something
 * wrong with the header data.
 */
static int peek_frame_header(const uint32_t header)
{
  unsigned int	version, layer, padding;
//  byte 		protected, private;
//  byte		mode, modex, copyright, original, emphasis;
  unsigned int	bitrate_enc, sampling_enc;
  unsigned int bitrate;
//  unsigned int sampling;
  byte rate;
//  unsigned int framesize
  unsigned int framelen;

  // Version:
  //   00 - MPEG Version 2.5
  //   01 - reserved
  //   10 - MPEG Version 2 (ISO/IEC 13818-3)
  //   11 - MPEG Version 1 (ISO/IEC 11172-3)
  version = (header >> 19) & 0x03;
  if (version == 1)
  {
    print_err("### Illegal version (1) in MPEG layer 2 audio header\n");
    return -1;
  }
  version = (version == 3) ? 1: (version == 2) ? 2: 3;

  // Layer:
  //   00 - reserved
  //   01 - Layer 3
  //   10 - Layer 2
  //   11 - Layer 1
  layer = (header >> 17) & 0x03;
  if (layer == 0)
  {
    print_err("### Illegal layer (0) in MPEG layer 2 audio header\n");
    return -1;
  }
  layer = 4 - layer;

  // protected (i.e. CRC present) field
//  protected = ! ((header >> 16) & 0x01);

  // bitrate field, whose meaning is dependent on version and layer
  bitrate_enc = (header >> 12) & 0x0f;
  if (bitrate_enc == 0x0f)
  {
    print_err("### Illegal bitrate_enc (0x0f) in MPEG layer 2 audio header\n");
    return -1;
  }

  bitrate = (bitrate_table[version-1][layer-1])[bitrate_enc];
  if (bitrate == 0) // bitrate now in kbits per channel
  {
    print_err("### Illegal bitrate (0 kbits/channel) in MPEG level 2"
              " audio header\n");
    return -1;
  }

  // sample rate field, whose meaning is dependent on version
  sampling_enc = (header >> 10) & 0x03;
  if (sampling_enc == 3)
  {
    print_err("### Illegal sampleing_enc (3) in MPEG layer 2 audio header\n");
    return -1;
  }
//  sampling = sampling_table[version-1][sampling_enc];

  // Make an AAC rate number from the rate number
  rate = (version * 3) + (sampling_enc & 2) + (sampling_enc == 0);

  // padding and private fields
  padding = (header >> 9) & 0x01;
//  private = (header >> 8) & 0x01;   // private doesn't get used

  // mode and mode extension - these are also not used
  // Channel Mode
  //    00 - Stereo
  //    01 - Joint stereo (Stereo)
  //    10 - Dual channel (Stereo)
  //    11 - Single channel (Mono)
//  mode  = (header >> 6) & 0x03;
//  modex = (header >> 4) & 0x03;

  // miscellaneous other things we ignore
//  copyright = (header >> 3) & 0x01;
//  original  = (header >> 2) & 0x01;
//  emphasis  = (header >> 0) & 0x03;

  // generate framesize and frame length
  // (for the moment, we only *use* the frame length)
  if (layer == 1)
  {
//    framesize = 384; // samples
    framelen  = (12000 * bitrate / aud_frame_rate_n[rate] + padding) * 4;
  }
  else if (version == 1)
  {
//    framesize = 1152; // samples
    framelen  = (144000 * bitrate / aud_frame_rate_n[rate] + padding);
  }
  else
  {
//    framesize = 576; // samples
    framelen  = (72000 * bitrate / aud_frame_rate_n[rate] + padding);
  }
  return framelen;
}

/*
 * Build a new layer2 audio frame datastructure
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static inline int build_audio_frame(audio_frame_p  *frame)
{
  audio_frame_p  new = malloc(SIZEOF_AUDIO_FRAME);
  if (new == NULL)
  {
    print_err("### Unable to allocate audio frame datastructure\n");
    return 1;
  }

  new->data = NULL;
  new->data_len = 0;

  *frame = new;
  return 0;
}

/*
 * Read the next audio frame.
 *
 * Assumes that the input stream is synchronised - i.e., it does not
 * try to cope if the next three bytes are not '1111 1111 1111'.
 *
 * - `file` is the file descriptor of the audio file to read from
 * - `frame` is the audio frame that is read
 *
 * Returns 0 if all goes well, EOF if end-of-file is read, and 1 if something
 * goes wrong.
 */
extern int read_next_l2audio_frame(int             file,
                                   audio_frame_p  *frame)
{
#define JUST_ENOUGH 6 // just enough to hold the bits of the headers we want

  int    err, ii;
  byte   header[JUST_ENOUGH];
  byte  *data = NULL;
  int    frame_length;	// XXXX Really 626.94 on average

  offset_t  posn = tell_file(file);
#if DEBUG
  fprint_msg("Offset: " OFFSET_T_FORMAT "\n",posn);
#endif

  err = read_bytes(file,JUST_ENOUGH,header);
  if (err == EOF)
    return EOF;
  else if (err)
  {
    fprint_err("### Error reading header bytes of MPEG layer 2 audio frame\n"
               "    (in frame starting at " OFFSET_T_FORMAT ")\n",posn);
    free(data);
    return 1;
  }

#if DEBUG
  print_msg("MPEG layer 2 frame\n");
  print_data(TRUE,"Start",header,JUST_ENOUGH,JUST_ENOUGH);
#endif

  while (header[0] != 0xFF || (header[1] & 0xe0) != 0xe0)
  {
    int skip = JUST_ENOUGH;
    fprint_err("### MPEG layer 2 audio frame does not start with '1111 1111 111x'\n"
               "    syncword - lost synchronisation?\n"
               "    Found 0x%X%X%X instead of 0xFFE\n",
               (header[0] & 0xF0) >> 4,
               (header[0] & 0x0F),
               (header[1] & 0xe0) >> 4);
    fprint_err("    (in frame starting at " OFFSET_T_FORMAT ")\n",posn);
    do
    {
      err = read_bytes(file,1,header);
      skip++;
      if (err == 0 && header[0] == 0xff)
      {
        err = read_bytes(file,1,header + 1);
        skip++;
        if (err == 0 && (header[1] & 0xe0) == 0xe0)
        {
          err = read_bytes(file,JUST_ENOUGH - 2, header + 2);
          break;
        }
      }
    } while (!err);
    if (err) return 1;
    fprint_err("#################### Resuming after %d skipped bytes\n",skip);
  }

  frame_length = peek_frame_header((header[1] << 16) | (header[2] << 8) | header[3]);
  if (frame_length < 1)
  {
    print_err("### Bad MPEG layer 2 audio header\n");
    return 1;
  }

  data = malloc(frame_length);
  if (data == NULL)
  {
    print_err("### Unable to extend data buffer for MPEG layer 2 audio frame\n");
    free(data);
    return 1;
  }

  for (ii=0; ii<JUST_ENOUGH; ii++)
    data[ii] = header[ii];

  err = read_bytes(file,frame_length - JUST_ENOUGH,&(data[JUST_ENOUGH]));
  if (err)
  {
    if (err == EOF)
      print_err("### Unexpected EOF reading rest of MPEG layer 2 audio frame\n");
    else
      print_err("### Error reading rest of MPEG layer 2 audio frame\n");
    free(data);
    return 1;
  }

  err = build_audio_frame(frame);
  if (err)
  {
    free(data);
    return 1;
  }
  (*frame)->data = data;
  (*frame)->data_len = frame_length;

  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

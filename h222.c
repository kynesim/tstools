/*
 * Datastructures and definitions useful for working with H.222 data,
 * whether it be Transport Stream or Program Stream
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

#include "h222_fns.h"

extern const char *h222_stream_type_str(unsigned s)
{
  switch (s)
  {
  case 0x00: return "Reserved";
  case 0x01: return "11172-2 video (MPEG-1)";
  case 0x02: return "H.262/13818-2 video (MPEG-2) or 11172-2 constrained video";
  case 0x03: return "11172-3 audio (MPEG-1)";
  case 0x04: return "13818-3 audio (MPEG-2)";
  case 0x05: return "H.222.0/13818-1  private sections";
  case 0x06: return "H.222.0/13818-1 PES private data (maybe Dolby/AC-3 in DVB)";
  case 0x07: return "13522 MHEG";
  case 0x08: return "H.222.0/13818-1 Annex A - DSM CC";
  case 0x09: return "H.222.1";
  case 0x0A: return "13818-6 type A";
  case 0x0B: return "13818-6 type B";
  case 0x0C: return "13818-6 type C";
  case 0x0D: return "13818-6 type D";
  case 0x0E: return "H.222.0/13818-1 auxiliary";
  case 0x0F: return "13818-7 Audio with ADTS transport syntax";
  case 0x10: return "14496-2 Visual (MPEG-4 part 2 video)";
  case 0x11: return "14496-3 Audio with LATM transport syntax (14496-3/AMD 1)";
  case 0x12: return "14496-1 SL-packetized or FlexMux stream in PES packets";
  case 0x13: return "14496-1 SL-packetized or FlexMux stream in 14496 sections";
  case 0x14: return "ISO/IEC 13818-6 Synchronized Download Protocol";
  case 0x15: return "Metadata in PES packets";
  case 0x16: return "Metadata in metadata_sections";
  case 0x17: return "Metadata in 13818-6 Data Carousel";
  case 0x18: return "Metadata in 13818-6 Object Carousel";
  case 0x19: return "Metadata in 13818-6 Synchronized Download Protocol";
  case 0x1A: return "13818-11 MPEG-2 IPMP stream";
  case 0x1B: return "H.264/14496-10 video (MPEG-4/AVC)";
  case 0x24: return "HEVC video stream";
  case 0x25: return "HEVC temporal video subset (profile Annex A H.265)";
  case 0x42: return "AVS Video";
  case 0x7F: return "IPMP stream";
  case 0x81: return "User private (commonly Dolby/AC-3 in ATSC)";
  default:
    if ((0x1C < s) && (s < 0x7E))
      return "H.220.0/13818-1 reserved";
    else if ((0x80 <= s) && (s <= 0xFF))
      return "User private";
    return "Unrecognised";
  }
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

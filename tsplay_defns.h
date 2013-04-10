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

#ifndef _tsplay_defns
#define _tsplay_defns

// If not being quiet, report progress every TSPLAY_REPORT_EVERY packets read
#define TSPLAY_REPORT_EVERY 10000

typedef enum tsplay_output_pace_mode_e
{
  TSPLAY_OUTPUT_PACE_FIXED,
  TSPLAY_OUTPUT_PACE_PCR1,  // Src buffering timing
  TSPLAY_OUTPUT_PACE_PCR2_TS,   // write buffer timing - use 1st PCR found for PID
  TSPLAY_OUTPUT_PACE_PCR2_PMT   // write buffer timing = look up PCR PID in PMT
} tsplay_output_pace_mode;

#endif // tsplay_defns

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

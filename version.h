/*
 * The official version number of this set of software.
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

#ifndef _version
#define _version

#include "printing_fns.h"

#define STRINGIZE1(x) #x
#define STRINGIZE(x) STRINGIZE1(x)
#define TSTOOLS_VERSION_STRING STRINGIZE(TSTOOLS_VERSION)

const char software_version[] = TSTOOLS_VERSION_STRING;

// The following is intended to be output as part of the main help text for
// each program. ``program_name`` is thus the name of the program.
#define REPORT_VERSION(program_name) \
  fprint_msg("  TS tools version %s, %s built %s %s\n", \
             software_version,(program_name), \
             __DATE__,__TIME__)

#endif // _version

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

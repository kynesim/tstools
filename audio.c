/*
 * Generic audio functionality
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
#include "audio_fns.h"
#include "adts_fns.h"
#include "l2audio_fns.h"
#include "ac3_fns.h"

/*
 * Build a new generic audio frame datastructure
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int build_audio_frame(audio_frame_p  *frame)
{
  audio_frame_p  new = malloc(SIZEOF_AUDIO_FRAME);
  if (new == NULL)
  {
    fprint_err("### Unable to allocate audio frame datastructure\n");
    return 1;
  }

  new->data = NULL;
  new->data_len = 0;

  *frame = new;
  return 0;
}

/*
 * Tidy up and free an audio frame datastructure when we've finished with it
 *
 * Empties the datastructure, frees it, and sets `frame` to NULL.
 *
 * If `frame` is already NULL, does nothing.
 */
extern void free_audio_frame(audio_frame_p  *frame)
{
  if (*frame == NULL)
    return;

  if ((*frame)->data != NULL)
  {
    free((*frame)->data);
    (*frame)->data = NULL;
  }
  (*frame)->data_len = 0;

  free(*frame);
  *frame = NULL;
}
/*
 * Read the next audio frame.
 *
 * Assumes that the input stream is synchronised - i.e., it does not
 * try to cope if (for MPEG2) the next three bytes are not '1111 1111 1111'.
 *
 * - `file` is the file descriptor of the audio file to read from
 * - `audio_type` indicates what type of audio - e.g., AUDIO_ADTS
 * - `frame` is the audio frame that is read
 *
 * Returns 0 if all goes well, EOF if end-of-file is read, and 1 if something
 * goes wrong.
 */
extern int read_next_audio_frame(int            file,
                                 int            audio_type,
                                 audio_frame_p *frame)
{
  switch (audio_type)
  {
  case AUDIO_ADTS_MPEG2:
    return read_next_adts_frame(file,frame,ADTS_FLAG_NO_EMPHASIS);
  case AUDIO_ADTS_MPEG4:
    return read_next_adts_frame(file,frame,ADTS_FLAG_FORCE_EMPHASIS);
  case AUDIO_ADTS:
    return read_next_adts_frame(file,frame,0);
  case AUDIO_L2:
    return read_next_l2audio_frame(file,frame);
  case AUDIO_AC3:
    return read_next_ac3_frame(file, frame);
  default:
    fprint_err("### Unrecognised audio type %d - cannot get next audio frame\n",
               audio_type);
    return 1;
  }
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

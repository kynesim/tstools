/*
 * PES reading facilities
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
#include "ts_fns.h"
#include "ps_fns.h"
#include "es_fns.h"
#include "pes_fns.h"
#include "pidint_fns.h"
#include "h262_fns.h"
#include "tswrite_fns.h"
#include "printing_fns.h"
#include "misc_fns.h"


//#define DEBUG
#define DEBUG_READ_PACKETS 0
#define DEBUG_PES_ASSEMBLY 0
#define DEBUG_PROGRAM_INFO 1
#define SHOW_PROGRAM_INFO 0

#define ALLOW_OVERLONG_PACKETS 1

// PES packet stream ids
// See H.222.0 Table 2-17 and Table 2-18
// These are specifically the stream ids used to decide if a PES packet
// contains header data, filler bytes or what
#define STREAM_ID_PROGRAM_STREAM_MAP  0xbc
#define STREAM_ID_PRIVATE_STREAM_1    0xbd
#define STREAM_ID_PADDING_STREAM      0xbe
#define STREAM_ID_PRIVATE_STREAM_2    0xbf
#define STREAM_ID_ECM_STREAM          0xf0
#define STREAM_ID_EMM_STREAM          0xf1
#define STREAM_ID_DSMCC_STREAM        0xf2
#define STREAM_ID_13522_STREAM        0xf3
#define STREAM_ID_H222_A_STREAM       0xf4
#define STREAM_ID_H222_B_STREAM       0xf5
#define STREAM_ID_H222_C_STREAM       0xf6
#define STREAM_ID_H222_D_STREAM       0xf7
#define STREAM_ID_H222_E_STREAM       0xf8
#define STREAM_ID_ANCILLARY_STREAM    0xf9
#define STREAM_ID_PROGRAM_STREAM_DIRECTORY  0xff

// ============================================================
// PES packet data
// ============================================================
/*
 * Build a new PES packet datastructure
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
static int build_PES_packet_data(PES_packet_data_p *data)
{
  PES_packet_data_p new = malloc(SIZEOF_PES_PACKET_DATA);
  if (new == NULL)
  {
    print_err("### Unable to allocate PES packet datastructure\n");
    return 1;
  }

  new->data = NULL;
  new->data_len = 0;
  new->es_data_len = 0;
  new->length = 0;
  new->posn = 0;
  new->is_video = TRUE;     // a guess
  new->data_alignment_indicator = FALSE; // another
  new->has_PTS = FALSE;     // assumed until told otherwise

  *data = new;
  return 0;
}

/*
 * Add some data to a PES packet datastructure
 *
 * - `data` is the PES packet datastructure concerned
 * - `bytes` is the data to add
 * - `bytes_len` is how much data there is
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
static inline int extend_PES_packet_data(PES_packet_data_p data,
                                         byte              bytes[],
                                         int               bytes_len)
{
  if (data->data == NULL)
  {
    data->data = malloc(bytes_len);
    if (data->data == NULL)
    {
      print_err("### Unable to extend PES packet data array\n");
      return 1;
    }
    memcpy(data->data,bytes,bytes_len);
    data->data_len = bytes_len;
  }
  else
  {
    data->data = realloc(data->data,data->data_len + bytes_len);
    if (data->data == NULL)
    {
      print_err("### Unable to extend PES packet data array\n");
      return 1;
    }
    memcpy(&(data->data[data->data_len]),bytes,bytes_len);
    data->data_len = data->data_len + bytes_len;
  }
  return 0;  
}

/*
 * Build a dummy PES packet datastructure.
 *
 * - `data` is the "new" dummy PES packet. Do not free this,
 *   as it is remembered statically inside the function.
 * - `data_len` is the required (total) size of the dummy PES packet
 *
 * Returns 0 if all goes well, 1 if something goes wrong
 */
static inline int build_dummy_PES_packet_data(PES_packet_data_p *data,
                                              int                data_len)
{
  int  err;
  static PES_packet_data_p  local_data = NULL;
  if (local_data == NULL)
  {
    err = build_PES_packet_data(&local_data);
    if (err)
    {
      print_err("### Error building dummy PES packet\n");
      return 1;
    }
    local_data->is_video = FALSE;
  }
  if (local_data->data == NULL)
  {
    local_data->data = malloc(data_len);
    if (local_data->data == NULL)
    {
      print_err("### Unable to extend dummy PES packet data array\n");
      return 1;
    }
    memset(local_data->data,0xFF,data_len);
  }
  else if (data_len > local_data->data_len)
  {
    local_data->data = realloc(local_data->data,data_len);
    if (local_data->data == NULL)
    {
      print_err("### Unable to extend dummy PES packet data array\n");
      return 1;
    }
    memset(local_data->data,0xFF,data_len);
  }

  if (data_len != local_data->data_len)
  {
    int PES_packet_len = data_len - 6;
    // Set up the data in the PES packet
    local_data->data[0] = 0x00;
    local_data->data[1] = 0x00;
    local_data->data[2] = 0x01;  // end of the packet_start_code_prefix
    local_data->data[3] = STREAM_ID_PADDING_STREAM;
    if (PES_packet_len > 0xFFFF)
    {
      local_data->data[4] = 0;
      local_data->data[5] = 0;
    }
    else
    {
      local_data->data[4] = (byte) ((PES_packet_len & 0xFF00) >> 8);
      local_data->data[5] = (byte) ((PES_packet_len & 0x00FF));
    }
    local_data->data_len = data_len;
  }
  *data = local_data;
  return 0;  
}

/*
 * Free a PES packet datastructure
 *
 * - `data` is the PES packet datastructure, which will be freed,
 *   and returned as NULL.
 */
extern void free_PES_packet_data(PES_packet_data_p *data)
{
  if ((*data) == NULL)
    return;
  if ((*data)->data != NULL)
  {
    free((*data)->data);
    (*data)->data = NULL;
  }
  (*data)->data_len = 0;
  (*data)->length = 0;
  free(*data);
  *data = NULL;
  return;
}

// ============================================================
// Transport Stream support - PID -> PES data
// (datastructure support based on that for pidint lists in ts.c)
// ============================================================
/*
 * Initialise a new PID/PES data datastructure.
 */
static int init_peslist(peslist_p  list)
{
  int ii;
  list->length = 0;
  list->size = PESLIST_START_SIZE;
  list->data = malloc(SIZEOF_PES_PACKET_DATA*PESLIST_START_SIZE);
  if (list->data == NULL)
  {
    print_err("### Unable to allocate PES array in PID/PES data array");
    return 1;
  }
  list->pid = malloc(sizeof(uint32_t)*PESLIST_START_SIZE);
  if (list->pid == NULL)
  {
    free(list->data);
    print_err("### Unable to allocate PID array in PID/PES data array\n");
    return 1;
  }
  // Just in case...
  for (ii=0; ii<list->size; ii++)
    list->data[ii] = NULL;
  return 0;
}

/*
 * Build a new PID/PES data datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int build_peslist(peslist_p  *list)
{
  peslist_p  new = malloc(SIZEOF_PESLIST);
  if (new == NULL)
  {
    print_err("### Unable to allocate PID/PES data array\n");
    return 1;
  }

  if (init_peslist(new))
    return 1;

  *list = new;
  return 0;
}

/*
 * Tidy up and free a PID/PES data datastructure after we've finished with it
 *
 * Clears the datastructure, frees it and returns `list` as NULL.
 *
 * Does nothing if `list` is already NULL.
 */
static void free_peslist(peslist_p  *peslist)
{
  peslist_p list = *peslist;
  if (list == NULL)
    return;
  if (list->data != NULL)
  {
    int ii;
    for (ii=0; ii<list->length; ii++)
    {
      if (list->data[ii] != NULL)
        free_PES_packet_data(&list->data[ii]);
    }
    free(list->data);
    list->data = NULL;
  }
  if (list->pid != NULL)
  {
    free(list->pid);
    list->pid = NULL;
  }
  list->length = 0;
  list->size = 0;
  free(list);
  *peslist = NULL;
}

/*
 * Lookup a PID to find its index in a PID/PES data array.
 *
 * Note that if `list` is NULL, then -1 will be returned.
 *
 * Returns its index (0 or more) if the PID is in the list, -1 if it is not.
 */
static inline int pid_index_in_peslist(peslist_p  list,
                                       uint32_t   pid)
{
  int ii;
  if (list == NULL)
    return -1;
  for (ii = 0; ii < list->length; ii++)
  {
    if (list->pid[ii] == pid)
      return ii;
  }
  return -1;
}

/*
 * Lookup a PID to see if it is in a PID/PES data array.
 *
 * Note that if `list` is NULL, then FALSE will be returned.
 *
 * Returns TRUE if the PID is in the list, FALSE if it is not.
 */
static inline int pid_in_peslist(peslist_p  list,
                                 uint32_t   pid)
{
  return pid_index_in_peslist(list,pid) != -1;
}

/*
 * Start a new PES packet for a PID.
 *
 * Creates a new entry for the given PID, and returns the corresponding new
 * PES packet.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int start_packet_in_peslist(PES_reader_p       reader,
                                   uint32_t           pid,
                                   int                is_video,
                                   PES_packet_data_p *data)
{
  int err;
  int ii;
  peslist_p  list = reader->packets;

  if (list == NULL)
  {
    print_err("### Unable to append to NULL PID/PES data array\n");
    return 1;
  }
  
  err = build_PES_packet_data(data);
  if (err)
  {
    print_err("### Unable to build new PES packet datastructure"
              " for PID/PES data array\n");
    return 1;
  }
  (*data)->is_video = is_video;

  for (ii=0; ii<list->length; ii++)
  {
    if (list->pid[ii] == pid)
    {
      // There is already an entry for this PID - does it have data?
      if (list->data[ii] != NULL)
      {
        PES_packet_data_p packet = list->data[ii];
        if (reader->give_warning)
          fprint_err("!!! PID %04x (%d) already has an unfinished PES packet"
                     " associated with it\n    %d byte%s of %d bytes were already"
                     " read - ignoring them\n",pid,pid,packet->data_len,
                     (packet->data_len==1?"":"s"),packet->length);
        free_PES_packet_data(&(list->data[ii]));
      }
      list->data[ii] = *data;
      return 0;
    }
  }

  // Otherwise, we need to add a new entry to the list
  if (list->length == list->size)
  {
    int newsize = list->size + PESLIST_INCREMENT;
    list->data = realloc(list->data,newsize*SIZEOF_PES_PACKET_DATA);
    if (list->data == NULL)
    {
      print_err("### Unable to extend PID/PES data array\n");
      free_PES_packet_data(data);
      return 1;
    }
    list->pid = realloc(list->pid,newsize*sizeof(uint32_t));
    if (list->pid == NULL)
    {
      print_err("### Unable to extend PID/PES data array\n");
      free_PES_packet_data(data);
      return 1;
    }
    list->size = newsize;
  }
  list->pid[list->length] = pid;
  list->data[list->length] = *data;
  list->length++;
  return 0;
}

/*
 * Find the PES packet for a PID.
 *
 * NB: returns `data` as NULL if the data for the PID *is* NULL.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static inline int find_packet_in_peslist(peslist_p          list,
                                         uint32_t           pid,
                                         PES_packet_data_p *data)
{
  int index = pid_index_in_peslist(list,pid);
  if (index == -1)
    return 1;
  *data = list->data[index];
  return 0;
}

/*
 * Clear the PES packet for a PID.
 *
 * Leaves the entry in the PID/PES array (with NULL data pointer) around for
 * reuse.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int clear_packet_in_peslist(peslist_p  list,
                                   uint32_t   pid)
{
  int index;

  if (list == NULL)
  {
    print_msg("Unable to clear PES packet in NULL PID/PES data array\n");
    return 1;
  }

  index = pid_index_in_peslist(list,pid);
  if (index == -1)
  {
    fprint_err("### Unable to find PID %04x (%x) in PID/PES data array,"
               " so cannot clear its data\n",pid,pid);
    return 1;
  }
  list->data[index] = NULL;
  return 0;
}

// ============================================================
// Reading the Program Stream
// ============================================================
/*
 * Return the next PES packet from the input PS file
 *
 * - `reader` is a PES reader context
 * - `packet_data` is the packet data (NULL if EOF is read)
 *
 * Returns 0 if all goes well, EOF if end of file is read, and 1 if
 * something goes wrong.
 */
static int read_next_PES_packet_from_PS(PES_reader_p       reader,
                                        PES_packet_data_p *packet_data)
{
  // Read PS packets
  // If a packet is a PES packet, return it

  for (;;)
  {
    int      err;
    byte     stream_id;    // The packet's stream id
    int      keep = FALSE; // Keep this packet?
    int      is_video = FALSE;
    struct PS_packet      packet = {0};
    struct PS_pack_header header = {0};

    err = read_PS_packet_start(reader->psreader,FALSE,&reader->posn,
                               &stream_id);
    if (err == EOF)
    {
      *packet_data = NULL;
      return EOF;
    }
    else if (err)
      return 1;

    // If it's the pack header, read it and ignore it
    if (stream_id == 0xba)
    {
      err = read_PS_pack_header_body(reader->psreader,&header);
      if (err == EOF)
      {
        fprint_err("!!! Unexpected EOF - partial PS packet at "
                   OFFSET_T_FORMAT " ignored\n",reader->posn);
        *packet_data = NULL;
        return EOF;
      }
      else if (err)
      {
        fprint_err("### Error reading data for pack header starting at "
                   OFFSET_T_FORMAT "\n",reader->posn);
        return 1;
      }
      continue;
    }

    err = read_PS_packet_body(reader->psreader,stream_id,&packet);
    if (err == EOF)
    {
      fprint_err("!!! Unexpected EOF - partial PS packet at "
                 OFFSET_T_FORMAT " ignored\n",reader->posn);
      *packet_data = NULL;
      return EOF;
    }
    else if (err)
    {
      fprint_err("### Error reading PS packet starting at "
                 OFFSET_T_FORMAT "\n",reader->posn);
      return 1;
    }

    // We have to decide whether to discard this data because it is not
    // a "PES" packet.

    // First, we know we can discard things that we are sure are part of the
    // PS infrastructure. Note that we don't need to check for 0xba (pack
    // header) because we already did that above, and we shouldn't have to
    // check for 0xb9 (MPEG_program_end_code), because that should already
    // have been interpreted as EOF by read_PS_packet_start().
    if (stream_id == 0xbb || // PS system header
        stream_id == 0xbc || // PS map
        stream_id == 0x01)   // PS directory
    {
      /* pass */;
    }
    else if (stream_id == PRIVATE1_AUDIO_STREAM_ID)
    {
      // It's private stream 1, traditionally used for Dolby (AC-3) audio
      if (reader->video_only)
        keep = FALSE;
      else if (reader->audio_stream_id == stream_id)
      {
        keep = TRUE;
        is_video = FALSE;
      }
    }
    else if ((stream_id >= 0xc0) && (stream_id <= 0xdf))
    {
      // It's a non-Dolby audio stream
      if (reader->video_only)
        keep = FALSE;
      else if (reader->audio_stream_id == stream_id)
      {
        keep = TRUE;
        is_video = FALSE;
      }
      else if (reader->audio_stream_id == 0)
      {
        // Aha - we're looking for an audio stream to use, and this is it
        reader->audio_stream_id = stream_id;
        keep = TRUE;
        is_video = FALSE;
        if (reader->give_info)
          fprint_msg("Selecting audio stream number %d\n",stream_id & 0x1F);
      }
    }
    else if (stream_id >= 0xe0 && stream_id <= 0xef)
    {
      // It's a video stream. We're assuming we only get one video
      // stream, so this is a "keeper" regardless
      keep = TRUE;
      is_video = TRUE;
    }

    if (keep)
    {
      err = build_PES_packet_data(packet_data);
      if (err) return 1;
      // We needn't copy the bytes from one "packet" to another,
      // it's easier to just transfer the array, if we're careful
      (*packet_data)->data      = packet.data;
      (*packet_data)->data_len  = packet.data_len;
      (*packet_data)->length    = packet.data_len;
      (*packet_data)->posn      = reader->posn;
      (*packet_data)->is_video  = is_video;
      // So the data array is no longer "present" in the orignal "packet"
      packet.data = NULL;
      packet.data_len = 0;
      break;
    }
    clear_PS_packet(&packet);
  }
  return 0;
}

// ============================================================
// Reading the Transport Stream
// ============================================================
/*
 * Look in the current program map to decide on the video and audio PIDs
 *
 * This is only expected to be called if the PMT has changed.
 *
 * TODO: The detection of different types of audio is not really strong enough
 * TODO: Take account of descriptors in deciding what things are, as well
 */
static void decide_pids(PES_reader_p  reader)
{
  pmt_p pmt = reader->program_map;
  int   ii;
  int   had_video = FALSE;
  int   had_audio = FALSE;

  if (pmt == NULL)
    return;

  // Since we're not expecting our datastructure to be very big,
  // or this function to be called very often, we can look at audio
  // and video separately.
  for (ii = 0; ii < pmt->num_streams; ii++)
  {
    if (IS_VIDEO_STREAM_TYPE(pmt->streams[ii].stream_type))
    {
      if (had_video)
      {
        if (reader->give_warning)
          fprint_err("!!! Multiple video streams in TS program %d, PMT"
                     " at " OFFSET_T_FORMAT " - using PID %04x\n",
                     reader->program_number,reader->posn,reader->video_pid);
        break;
      }
      else if (reader->video_pid == 0)
      {
        reader->video_pid = pmt->streams[ii].elementary_PID;
        switch (pmt->streams[ii].stream_type)
        {
        case AVC_VIDEO_STREAM_TYPE:
          reader->is_h264 = TRUE;
          reader->video_type = VIDEO_H264;
          break;
        case MPEG2_VIDEO_STREAM_TYPE:
        case MPEG1_VIDEO_STREAM_TYPE:   // well, more-or-less
          reader->is_h264 = FALSE;
          reader->video_type = VIDEO_H262;
          break;
        case AVS_VIDEO_STREAM_TYPE:
          reader->is_h264 = FALSE;
          reader->video_type = VIDEO_AVS;
          break;
        default:
          reader->is_h264 = FALSE;
          reader->video_type = VIDEO_UNKNOWN;
          break;
        }
        if (reader->give_info)
          fprint_msg("    Chose video PID %04x\n",reader->video_pid);
      }
      else if (pmt->streams[ii].elementary_PID != reader->video_pid)
      {
        if (reader->give_warning)
          fprint_err("!!! Video streams altered in TS program %d, PMT"
                     " at " OFFSET_T_FORMAT " - still using PID %04x\n",
                     reader->program_number,reader->posn,reader->video_pid);
        break;
      }
      had_video = TRUE;
    }
  }

  if (reader->video_only)
  {
    if (reader->give_info)
      print_msg("    Not interested in any audio streams\n");
    return;
  }

  for (ii = 0; ii < pmt->num_streams; ii++)
  {
    // Note that the audio detection will accept either DVB or ADTS Dolby
    // (AC-3) stream types
    if (IS_AUDIO_STREAM_TYPE(pmt->streams[ii].stream_type))
    {
      if (had_audio)
      {
        if (reader->give_warning)
          fprint_err("!!! Multiple audio streams in TS program %d, PMT"
                     " at " OFFSET_T_FORMAT " - using PID %04x\n",
                     reader->program_number,reader->posn,reader->audio_pid);
        break;
      }
      else if (reader->audio_pid == 0)
      {
        reader->audio_pid = pmt->streams[ii].elementary_PID;
        if (reader->give_info)
          fprint_msg("    Chose audio PID %04x\n",reader->audio_pid);
        if (IS_DOLBY_STREAM_TYPE(pmt->streams[ii].stream_type))
        {
          // Remember what stream type this Dolby data is using
          // - we'll assume that this doesn't change under our feet,
          // and thus we don't need to report if it changes
          reader->dolby_stream_type = pmt->streams[ii].stream_type;
          // If we're not overriding the output stream type, use it as is
          if (!reader->override_dolby_stream_type)
            reader->output_dolby_stream_type = reader->dolby_stream_type;
        }
      }
      else if (pmt->streams[ii].elementary_PID != reader->audio_pid)
      {
        if (reader->give_warning)
          fprint_err("!!! Audio streams altered in TS program %d, PMT"
                     " at " OFFSET_T_FORMAT " - still using PID %04x\n",
                     reader->program_number,reader->posn,reader->audio_pid);
        break;
      }
      had_audio = TRUE;
    }
  }

  if (!reader->override_program_data)
  {
    // Our output program data is to be the same as our input
    reader->output_video_pid = reader->video_pid;
    reader->output_audio_pid = reader->audio_pid;
    reader->output_pcr_pid = reader->pcr_pid;
    reader->output_pmt_pid = reader->pmt_pid;
  }
}

/*
 * Called to use the information extracted from a PMT packet.
 *
 * Note: don't free `pmt` after this call, as it is remembered
 * by the `reader`.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int refine_TS_program_info(PES_reader_p   reader,
                                  pmt_p          pmt)
{
  // If this is the *first* PMT, then just adopt its data wholesale
  if (reader->program_map == NULL)
  {
    reader->program_map = pmt;
    reader->pcr_pid = pmt->PCR_pid;
#if DEBUG_PROGRAM_INFO
    if (reader->give_info)
    {
      fprint_msg("PMT packet at " OFFSET_T_FORMAT ": first PMT, used as-is\n",
                 reader->posn);
      report_pmt(TRUE,"    ",reader->program_map);
    }
#else
    if (reader->give_info)
      report_pmt(TRUE,"    ",reader->program_map);
#endif
    // And use its information to determine our video/audio PIDs
    decide_pids(reader);
    reader->got_program_data = TRUE;
    return 0;
  }

  // Otherwise, check if this PMT contains the same information

  if (pmt->program_number != reader->program_number)
  {
    if (reader->give_info)
      fprint_msg("Ignoring PMT for program %d\n",pmt->program_number);
    free_pmt(&pmt);  // since our caller will not free it
    return 0;
  }

  if (same_pmt(pmt,reader->program_map))
  {
    free_pmt(&pmt);  // since our caller will not free it
    return 0;
  }

  // Grumble or replace? Maybe both is safest
  if (reader->give_warning)
  {
    fprint_err("!!! PMT in TS packet at " OFFSET_T_FORMAT
               " replaces previous program information\n",reader->posn);
    print_err("    Program information was:\n");
    report_pmt(FALSE,"      ",reader->program_map);
    print_err("    New program information is:\n");
    report_pmt(FALSE,"      ",pmt);
  }
  else if (reader->give_info)
  {
#if DEBUG_PROGRAM_INFO
    fprint_msg("PMT packet at " OFFSET_T_FORMAT ": updating program info\n",
               reader->posn);
#endif
    report_pmt(TRUE,"      ",pmt);
  }

  free_pmt(&reader->program_map);
  reader->program_map = pmt;
  reader->pcr_pid = pmt->PCR_pid;

  // And use this new information to determine/check our video/audio PIDs
  decide_pids(reader);
  return 0;
}

/*
 * Call this on each PMT found for the program being read from TS,
 * to refine its idea of what is in that program.
 *
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 *
 * Note that it is assumed that a particular program number will refer
 * to the same program stream throughout a TS file.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int extract_and_refine_TS_program_info(PES_reader_p   reader,
                                              uint32_t       pmt_pid,
                                              byte           pmt_data[],
                                              int            pmt_data_len)
{
  int     err;
  pmt_p   pmt = NULL;

  err = extract_pmt(FALSE,pmt_data,pmt_data_len,pmt_pid,&pmt);
  if (err)
  {
    print_err("### Error extracting stream list from PMT\n");
    return 1;
  }

  // If it's the wrong program, we're not interested
  if (pmt->program_number != reader->program_number)
  {
#if DEBUG_PROGRAM_INFO
    if (reader->give_info)
      fprint_msg("PMT packet at " OFFSET_T_FORMAT ": program number %d (not %d)\n",
                 reader->posn,pmt->program_number,reader->program_number);
#endif
    free_pmt(&pmt);
    return 0;
  }

  err = refine_TS_program_info(reader,pmt);
  if (err)
  {
    print_err("### Error refining TS program information from PMT\n");
    free_pmt(&pmt);
    return 1;
  }
  // Mustn't free `pmt` because it is remembered by the reader
  return 0;
}

/*
 * Look up the (first) PAT in a Transport Stream
 *
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 * - if `quiet` is not true, and the program number was given as 0, then
 *   the function will report on finding the first PAT and what program
 *   number it deduces therefrom.
 *
 * Note that it is assumed that a particular program number will refer
 * to the same program stream throughout a TS file.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int find_first_PAT(PES_reader_p   reader)
{
  int   err;
  int   num_read;
  pidint_list_p prog_list = NULL;

  err = find_pat(reader->tsreader,0,FALSE,!reader->give_info,
                 &num_read,&prog_list);
  if (err)
  {
    print_err("### Error finding first PAT\n");
    return 1;
  }

  if (prog_list->length == 0)
  {
    fprint_err("### No programs defined in first PAT (at " OFFSET_T_FORMAT
               ")\n",reader->tsreader->posn - TS_PACKET_SIZE);
    free_pidint_list(&prog_list);
    return 1;
  }
  else if (prog_list->length > 1 && reader->give_info)
    print_msg("Multiple programs in PAT - using the first\n\n");

  if (reader->program_number == 0)
  {
    reader->program_number = prog_list->number[0];
    reader->pmt_pid = prog_list->pid[0];
  }
  else
  {
    int   ii;
    int   got_program = FALSE;
    for (ii=0; ii<prog_list->length; ii++)
    {
      if (prog_list->number[ii] == reader->program_number)
      {
        got_program = TRUE;
        reader->pmt_pid = prog_list->pid[ii];
        break;
      }
    }
    if (!got_program)
    {
      fprint_err("### Program %d not found in first PAT at "
                 OFFSET_T_FORMAT "\n",reader->program_number,
                 reader->tsreader->posn - TS_PACKET_SIZE);
      return 1;
    }
  }
  free_pidint_list(&prog_list);
  return 0;
}

/*
 * Look up the (first after the first PAT) PMT in a Transport Stream
 *
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 *
 * Returns 0 if all goes well, 1 if something goes wrong, EOF if end-of-file
 * is found before the first (useful) PMT.
 */
static int find_first_PMT(PES_reader_p   reader)
{
  int     err;
  int     nread = 0;
  pmt_p   pmt = NULL;

  for (;;)
  {
    err = find_next_pmt(reader->tsreader,reader->pmt_pid,-1,0,
                        FALSE,!reader->give_info,&nread,&pmt);
    if (err)
    {
      fprint_err("### Error looking for program %d PMT with PID %04x"
                 " after first PAT\n",reader->program_number,reader->pmt_pid);
      return 1;
    }

    if (pmt->program_number == reader->program_number)
      break;

    if (reader->give_info)
      fprint_msg("(Program is %d, not %d - ignoring it)\n",
                 pmt->program_number,reader->program_number);

    free_pmt(&pmt);
  }

  err = refine_TS_program_info(reader,pmt);
  if (err)
  {
    print_err("### Error refining TS program information from PMT\n");
    free_pmt(&pmt);
    return 1;
  }
  // Mustn't free `pmt` because it is remembered by the reader
  return 0;
}

/*
 * Find the first program information for a Transport Stream.
 *
 * Rewinds when it has finished.
 *
 * Find the first PAT, and then the first PMT following that, and thus
 * determine the program information. Then rewind. This won't hurt it the
 * TS is well formed, since one would expect PAT and PMT at the start of
 * the stream. However, if we're reading something taken from a broadcast
 * stream, then it is unlikely that the PAT/PMT would occur near the
 * beginning, but it *is* likely that the program information will be
 * the same throughout. Thus by rewinding, we can hope to interpret useful
 * packets that occur at the start of the stream.
 *
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 * - if `quiet` is not true, and the program number was given as 0, then
 *   the function will report on finding the first PAT and what program
 *   number it deduces therefrom.
 *
 * Note that it is assumed that a particular program number will refer
 * to the same program stream throughout a TS file.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int determine_TS_program_info(PES_reader_p   reader)
{
  int  err;
  err = find_first_PAT(reader);
  if (err)
  {
    print_err("### Error finding TS program information\n");
    return 1;
  }
  err = find_first_PMT(reader);
  if (err)
  {
    print_err("### Error finding TS program information\n");
    return 1;
  }
  // It's only possible to rewind if we're not reading from standard
  // input. If it's not feasible, don't try.
  if (reader->tsreader->file != STDIN_FILENO)
  {
    err = seek_using_TS_reader(reader->tsreader, 0);
    if (err)
    {
      print_err("### Error rewinding TS stream after finding initial"
                " program information\n");
      return 1;
    }
    // Having rewound, we mustn't forget to reset our notion of the TS packet
    // position
    reader->posn = 0;
  }
  return 0;
}

/*
 * Start a new PES packet, for a given PID/stream id
 *
 * - `reader` is our PES reader context
 * - `pid` is the PID of the TS packet we've just read
 * - `stream_type` is the stream type of this data
 * - `payload` is the TS packets payload
 * - `payload_len` is the length of said payload
 * - if the PES packet is finished (i.e., all of it has been read in)
 *   then `finished` will be its data, otherwise `finished` will be NULL.
 *
 * Returns 0 if all went well, 1 if something went wrong
 */
static int start_new_PES_packet(PES_reader_p       reader,
                                uint32_t           pid,
                                byte               payload[],
                                int                payload_len,
                                PES_packet_data_p *finished)
{
  int  err;
  int  index;
  PES_packet_data_p  just_ended = NULL;
  PES_packet_data_p  data;

  //fprint_msg("%c",(pid==reader->video_pid?'V':'A'));fflush(stdout);
#if DEBUG_PES_ASSEMBLY
  fprint_msg(": start new %s PES packet, payload_len = %d\n",
             (pid==reader->video_pid?"video":"audio"),payload_len);
  print_data(TRUE,"Data",payload,payload_len,payload_len);
#endif
  
  if (payload_len < 6)
  {
    // It is technically possible to start a PES packet with very few
    // bytes in its first TS packet, but I can't see why anyone would
    // do it (after all, the adaptation field itself cannot be more
    // than 30 bytes (I counted quickly, I might be off by one) long,
    // which leaves *lots* of space. So I shall assume that it is an
    // error if the first six bytes (at least) of PES data are not
    // in the first TS packet
    fprint_err("### Only first %d byte%s of PES packet in its first TS packet,"
               " packet at " OFFSET_T_FORMAT "\n",payload_len,
               (payload_len==1?"":"s"),reader->posn);
    return 1;
  }
  if (payload[0] != 0 || payload[1] != 0 || payload[2] != 1)
  {
    fprint_err("### PES data starting in TS packet at " OFFSET_T_FORMAT
               " starts %02X %02X %02X, not 00 00 01\n",
               reader->posn,payload[0],payload[1],payload[2]);
    return 1;
  }

  // If PES packets with lengths of zero (i.e., meaning "unbounded") are
  // being transmitted, then we can only tell that we have reached their
  // end when we start the next PES packet for that same PID (or when we
  // reach EOF).
  index = pid_index_in_peslist(reader->packets,pid);
  if (index != -1 &&
      reader->packets->data[index] != NULL &&
      reader->packets->data[index]->length == 0)
  {
#if DEBUG_PES_ASSEMBLY
    print_msg("@@@ just ended previous packet (by implication)\n");
#endif
    just_ended = reader->packets->data[index];
    reader->packets->data[index] = NULL;
  }

  // Anyway, start a new PES packet for this TS packet's data
#if DEBUG_PES_ASSEMBLY
  print_msg("@@@ start packet in PES list\n");
#endif
  err = start_packet_in_peslist(reader,pid,pid==reader->video_pid,&data);
  if (err)
  {
    fprint_err("### Error trying to start a new PES packet,"
               " for TS packet " OFFSET_T_FORMAT "\n",reader->posn);
    return 1;
  }

#if DEBUG_PES_ASSEMBLY
  fprint_msg("@@@ extend packet - data_len was %d\n",data->data_len);
#endif
  err = extend_PES_packet_data(data,payload,payload_len);
  if (err)
  {
    print_err("### Error remembering data at start of PES packet\n");
    return 1;
  }
#if DEBUG_PES_ASSEMBLY
  fprint_msg("@@@ data_len is now %d\n",data->data_len);
#endif

  data->length = ((payload[4] << 8) | payload[5]);
  if (data->length != 0)
    data->length += 6;  // correct to the actual packet length
#if DEBUG_PES_ASSEMBLY
  else
    print_msg("@@@ PES packet marked as length 0\n");
#endif
  data->posn = reader->posn;

  // Unlikely, but have we already finished our PES packet?
  if ((data->data_len > data->length) && data->length != 0)
  {
#if ALLOW_OVERLONG_PACKETS
    int extra;
    fprint_err("### Found %d bytes of PES data, but expected %d"
               " (PES packet length + 6)\n",data->data_len,data->length);
    extra = data->data_len - data->length;
    if (extra > 0)
    {
#if 0
      int from = payload_len - extra;
      print_data(FALSE,"   End of data",payload+from,extra,extra);
#endif
      fprint_err("    In %s PES packet, PID %x, starting at "
                 OFFSET_T_FORMAT "\n",(pid==reader->video_pid?"video":"audio"),
                 pid,reader->posn);
      print_err("!!! Accepting packet anyway\n");
      *finished = data;
      err = clear_packet_in_peslist(reader->packets,pid);
      if (err) return 1;
    }
    else
      return 1;
#else
    fprint_err("### Found %d bytes of PES data, but expected %d"
               " (PES packet length + 6)\n",data->data_len,data->length);
    return 1;
#endif
  }
  else if (data->length == data->data_len)
  {
    *finished = data;
    err = clear_packet_in_peslist(reader->packets,pid);
    if (err) return 1;
  }
  else
    *finished = NULL;

  // And if we had a packet ended by this packet starting, defer this
  // result until later...
  if (just_ended)
  {
    reader->deferred = *finished;  // which might *not* be NULL
    *finished = just_ended;
  }

  return 0;
}

/*
 * Continue a PES packet, for a given PID/stream id
 *
 * - `reader` is our PES reader context
 * - `pid` is the PID of the TS packet we've just read
 * - `stream_type` is the stream type of this data
 * - `payload` is the TS packets payload
 * - `payload_len` is the length of said payload
 * - if the PES packet is finished (i.e., all of it has been read in)
 *   then `finished` will be its data, otherwise `finished` will be NULL.
 *
 * Returns 0 if all went well, 1 if something went wrong
 */
static int continue_PES_packet(PES_reader_p       reader,
                               uint32_t           pid,
                               byte               payload[],
                               int                payload_len,
                               PES_packet_data_p *finished)
{
  int  err;
  PES_packet_data_p  data;

#if DEBUG_PES_ASSEMBLY
  fprint_msg(": continue %s PES packet\n",
             (pid==reader->video_pid?"video":"audio"));
#endif
  
  err = find_packet_in_peslist(reader->packets,pid,&data);
  if (err || data == NULL)
  {
    if (reader->give_warning)
      fprint_err("!!! TS packet with PID %04x at " OFFSET_T_FORMAT
                 " continues an unstarted PES packet  - ignoring it\n",
                 pid,reader->posn);
    *finished = NULL;
    return 0;
  }

  //fprint_msg("%c",(pid==reader->video_pid?'v':'a'));fflush(stdout);

  err = extend_PES_packet_data(data,payload,payload_len);
  if (err)
  {
    print_err("### Error remembering data to continue PES packet\n");
    return 1;
  }

  if ((data->data_len > data->length) && data->length != 0)
  {
#if ALLOW_OVERLONG_PACKETS
    int extra;
    fprint_err("### Found %d bytes of PES data, but expected %d"
               " (PES packet length + 6)\n",data->data_len,data->length);
    extra = data->data_len - data->length;
    if (extra > 0)
    {
#if 0
      int from = payload_len - extra;
      print_data(FALSE,"   End of data",payload+from,extra,extra);
#endif
      fprint_err("    In %s PES packet, PID %x, starting at "
                 OFFSET_T_FORMAT "\n",(pid==reader->video_pid?"video":"audio"),
                 pid,reader->posn);
      print_err("!!! Accepting packet anyway\n");
      *finished = data;
      err = clear_packet_in_peslist(reader->packets,pid);
      if (err) return 1;
    }
    else
      return 1;
#else
    fprint_err("### Found %d bytes of PES data, but expected %d"
               " (PES packet length + 6)\n",data->data_len,data->length);
    return 1;
#endif
  }
  else if (data->data_len == data->length)
  {
    *finished = data;
    err = clear_packet_in_peslist(reader->packets,pid);
    if (err) return 1;
  }
  else
    *finished = NULL;
  
  return 0;
}

/*
 * Check for a PES packet legitimately ended by EOF
  *
  * - `reader` is a PES reader context
  * - `packet_data` is either NULL (meaning no more packets to return),
  *   or a PES packet whose length was specified as 0, meaning that it
  *   would be ended by the next PES packet with the same PID, or by EOF.
  *
  * Note that a PES packet length of 0 is *only* allowed for video ES
  * within TS, and we are only attempting to cope with a single video
  * stream (per program), so we need only expect to have to check for
  * one "outstanding" stream when we hit EOF.
*/
static void check_for_EOF_packet(PES_reader_p       reader,
                                 PES_packet_data_p *packet_data)
{
  int  ii;
  // Not trying to be very efficient - shouldn't matter
  for (ii = 0; ii < reader->packets->length; ii++)
  {
    if (reader->packets->data[ii] != NULL &&
        reader->packets->data[ii]->length == 0)
    {
      *packet_data = reader->packets->data[ii];
      reader->packets->data[ii] = NULL;
      return;
    }
  }
  *packet_data = NULL;
  return;
}

/*
 * Return the next PES packet from the input TS file
 *
 * - `reader` is a PES reader context
 * - `packet_data` is the packet data (NULL if EOF is read)
 *
 * Returns 0 if all goes well, EOF if end of file is read, and 1 if
 * something goes wrong.
 */
static int read_next_PES_packet_from_TS(PES_reader_p       reader,
                                        PES_packet_data_p *packet_data)
{
  // If we have a packet "in hand" because we read it earlier, then
  // just return it
  if (reader->deferred)
  {
    if (reader->video_only && !reader->deferred->is_video)
    {
      free_PES_packet_data(&reader->deferred);
    }
    else
    {
      *packet_data = reader->deferred;
      reader->deferred = NULL;
#if DEBUG_PES_ASSEMBLY
      print_msg("@@@ returning deferred PES packet\n");
#endif
      return 0;
    }
  }

  // If we had read EOF earlier (but not said so because of a PES
  // packet being finished by EOF), then admit to it now
  if (reader->had_eof)
  {
    *packet_data = NULL;
    return EOF;
  }

  for (;;)
  {
    int     err;
    byte   *ts_packet;
    int     payload_unit_start_indicator;
    byte   *adapt;
    int     adapt_len;
    byte   *payload;
    int     payload_len;

    uint32_t pid;

    // Remember the position of the packet we're going to read
    reader->posn = reader->tsreader->posn;

    // And read it
    // Remember that `ts_packet` will not persist, as it is a pointer
    // into the TS buffering innards
    err = read_next_TS_packet(reader->tsreader,&ts_packet);
    if (err == EOF)
    {
      // If we've been given EOF, then either we're just *read* EOF
      // instead of a packet (the obvious case), or we read some data
      // that was terminated by EOF last time, and the EOF was "deferred"
      // by the underlying buffering methods.
      // So, just in case, we'll check for an unbounded (length marked as
      // zero) video stream PES packet
      check_for_EOF_packet(reader,packet_data);
      if (*packet_data == NULL)
        return EOF;
      else
        return 0;
    }
    else if (err)
    {
      fprint_err("### Error reading TS packet at " OFFSET_T_FORMAT "\n",
                 reader->posn);
      return 1;
    }

    err = split_TS_packet(ts_packet,&pid,&payload_unit_start_indicator,
                          &adapt,&adapt_len,&payload,&payload_len);
    if (err)
    {
      fprint_err("### Error interpreting TS packet at " OFFSET_T_FORMAT "\n",
                 reader->posn);
      return 1;
    }

#if DEBUG_PES_ASSEMBLY
    fprint_msg("@@@ TS packet at " OFFSET_T_FORMAT " with pid %3x",
               reader->posn,pid);
#endif

    // If we're writing out TS packets directly to a client, then this
    // is probably a sensible place to do it.
    if (reader->write_TS_packets && reader->tswriter != NULL &&
        !reader->suppress_writing)
    {
      err = tswrite_write(reader->tswriter,ts_packet,pid,FALSE,0);
      if (err)
      {
        fprint_err("### Error writing TS packet (PID %04x) at "
                   OFFSET_T_FORMAT "\n",pid,reader->posn);
        return 1;
      }
    }

    if (pid == 0)  // PAT
    {
      // XXX We should probably check that the PAT for our program
      // has not changed...
#if DEBUG_PES_ASSEMBLY
      print_msg(": PAT\n");
#endif
    }
    else if (pid == reader->pmt_pid)
    {
#if DEBUG_PES_ASSEMBLY
      print_msg(": PMT\n");
#endif

      if (payload_unit_start_indicator && reader->pmt_data)
      {
        // This is the start of a new PMT packet, but we'd already
        // started one, so throw its data away
        fprint_err("!!! Discarding previous (uncompleted) PMT data at "
                   OFFSET_T_FORMAT "\n",reader->posn);
        free(reader->pmt_data);
        reader->pmt_data = NULL; reader->pmt_data_len = reader->pmt_data_used = 0;
      }
      else if (!payload_unit_start_indicator && !reader->pmt_data)
      {
        // This is the continuation of a PMT packet, but we hadn't
        // started one yet
        fprint_err("!!! Discarding PMT continuation, no PMT started, at "
                   OFFSET_T_FORMAT "\n",reader->posn);
        continue;
      }

      err = build_psi_data(FALSE,payload,payload_len,pid,
                           &reader->pmt_data,
                           &reader->pmt_data_len,
                           &reader->pmt_data_used);
      if (err)
      {
        fprint_err("### Error %s PMT at " OFFSET_T_FORMAT "\n",
                   (payload_unit_start_indicator?"starting new":"continuing"),
                   reader->posn);
        if (reader->pmt_data) free(reader->pmt_data);
        return 1;
      }

      // Do we need more data to complete this PMT?
      if (reader->pmt_data_len > reader->pmt_data_used)
        continue;

      err = extract_and_refine_TS_program_info(reader,pid,
                                               reader->pmt_data,
                                               reader->pmt_data_len);
      if (err)
      {
        fprint_err("### Error updating program info from PMT"
                   " (TS packet at " OFFSET_T_FORMAT ")\n",reader->posn);
        if (reader->pmt_data) free(reader->pmt_data);
        return 1;
      }

      free(reader->pmt_data);
      reader->pmt_data = NULL; reader->pmt_data_len = reader->pmt_data_used = 0;

      if (reader->write_PES_packets && !reader->suppress_writing)
      {
        // XXX We *probably* should check if it's changed before doing this,
        //     but at least by outputting it again we ensure it's current
        err = write_program_data(reader,reader->tswriter);
        if (err) return 1;
        // At least make sure it doesn't get written again *too* soon
        reader->program_index = reader->program_freq;
      }
    }
    else if (payload_len > 0 &&
             (pid == reader->video_pid ||
              (pid == reader->audio_pid && !reader->video_only)))
    {
      // It's a packet we're interested in
      PES_packet_data_p  finished;
      if (payload_unit_start_indicator)
        err = start_new_PES_packet(reader,pid,payload,payload_len,
                                   &finished);
      else
        err = continue_PES_packet(reader,pid,payload,payload_len,
                                  &finished);
      if (err)
      {
        fprint_err("### Error %s PES packet (PID %04x)"
                   " with TS packet at " OFFSET_T_FORMAT "\n",
                   (payload_unit_start_indicator?"starting":"continuing"),
                   pid,reader->posn);
        print_data(FALSE,"    Data",payload,payload_len,20);
        return 1;
      }
      if (finished)
      {
#if DEBUG_PES_ASSEMBLY
        fprint_msg("@@@ PES packet with pid %x finished\n",pid);
        report_PES_data_array("    ",finished->data,finished->data_len,TRUE);
#endif

        if (pid == reader->audio_pid && reader->video_only)
        {
          // Actually, they aren't interested in audio at the moment
          // (we check this now because the user can alter this over
          // time, and when we *started* collecting the packet, they
          // might have said they *were* interested in audio)
          free_PES_packet_data(&finished);
        }
        else
        {
#if DEBUG_PES_ASSEMBLY
          print_msg("@@@ return it\n");
#endif
          *packet_data = finished;
          break;
        }
      }
    }
#if DEBUG_PES_ASSEMBLY
    else
      print_msg("\n");
#endif
  }
  return 0;
}

// ============================================================
// General functionality
// ============================================================
/*
 * Look at the start of a file to determine if it appears to be transport
 * stream. Rewinds the file when it is finished.
 *
 * The file is assumed to be Transport Stream if it starts with 0x47 as
 * the first byte, and 0x47 recurs at 188 byte intervals (in other words,
 * it appears to start with several TS packets).
 *
 * - `input` is the file to check
 * - `is_TS` is TRUE if it looks like TS, as described above.
 *
 * Returns 0 if all goes well, 1 if there was an error.
 */
extern int determine_if_TS_file(int    input,
                                int   *is_TS)
{
  int  err;
  int  ii;
  byte buf[TS_PACKET_SIZE];

  *is_TS = TRUE;

  for (ii = 0; ii < 100; ii++)
  {
    err = read_bytes(input,TS_PACKET_SIZE,buf);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error trying to check if file is TS\n");
      return 1;
    }
    if (buf[0] != 0x47)
    {
      *is_TS = FALSE;
      break;
    }
  }
  err = seek_file(input,0);
  if (err)
  {
    print_err("### Error rewinding file after determining if it is TS\n");
    return 1;
  }
  return 0;
}

/*
 * Given a PES stream (PS or TS), attempt to determine if it holds H.262 or
 * H.264 data.  Sets the `video_type` flag on the reader appropriately.
 *
 * (In fact, this is only needed for PS data, as TS data "says" what it
 * contains in the PAT/PMT.)
 *
 * - `input` is the file to check
 *
 * Returns 0 if all goes well, 1 if there was an error (including the
 * stream not appearing to be either).
 */
static int determine_PES_video_type(PES_reader_p  reader)
{
  int  err;
  ES_p es;
  int  old_video_only = reader->video_only;

  err = build_elementary_stream_PES(reader,&es);
  if (err)
  {
    print_err("### Error starting elementary stream before"
              " working out if PS is H.262 or H.264\n");
    return 1;
  }

  reader->video_only = TRUE;

  err = decide_ES_video_type(es,FALSE,FALSE,&reader->video_type);
  if (err)
  {
    print_err("### Error deciding on PS video type\n");
    free_elementary_stream(&es);
    return 1;
  }
  free_elementary_stream(&es);

  reader->is_h264 = (reader->video_type == VIDEO_H264);
  reader->video_only = old_video_only;

  err = rewind_program_stream(reader->psreader);
  if (err)
  {
    print_err("### Error rewinding PS stream after determining its type\n");
    return 1;
  }
  return 0;
}

/*
 * Build a PES reader datastructure
 *
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout).
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `reader` is the resulting PES reader
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int build_PES_reader_datastructure(int            give_info,
                                          int            give_warnings,
                                          PES_reader_p  *reader)
{
  int  err;
  PES_reader_p new = malloc(SIZEOF_PES_READER);
  if (new == NULL)
  {
    print_err("### Unable to allocate PES reader datastructure\n");
    return 1;
  }

  new->tsreader = NULL;  // for the moment, at least
  new->psreader = NULL;  // for the moment, at least
  new->is_TS = FALSE;    // for want of better
  new->give_info = give_info;
  new->give_warning = give_warnings;
  new->posn = 0;
  new->is_h264 = FALSE;
  new->video_type = VIDEO_UNKNOWN;
  new->packet = NULL;

  new->program_number = 0;
  new->program_map = NULL;
  new->video_only = FALSE;
  new->audio_stream_id = 0;

  new->pmt_data = NULL;
  new->pmt_data_len = 0;
  new->pmt_data_used = 0;

  new->video_pid = new->audio_pid = 0;
  new->pcr_pid = new->pmt_pid = 0;
  new->got_program_data = FALSE;

  new->output_program_number = 0;
  new->output_video_pid = new->output_audio_pid = 0;
  new->output_pcr_pid = new->output_pmt_pid = 0;
  new->override_program_data = FALSE;

  new->output_dolby_stream_type =
    new->dolby_stream_type = DVB_DOLBY_AUDIO_STREAM_TYPE;
  new->override_dolby_stream_type = FALSE;

  new->tswriter = NULL;
  new->write_PES_packets = FALSE;
  new->write_TS_packets = FALSE;
  new->suppress_writing = TRUE;
  new->dont_write_current_packet = FALSE;
  new->pes_padding = 0;

  new->debug_read_packets = FALSE;

  err = build_peslist(&new->packets);
  if (err)
  {
    print_err("### Error building PES reader datastructure\n");
    free(new);
    return 1;
  }

  new->deferred = NULL;
  new->had_eof = FALSE;
  *reader = new;
  return 0;
}

/*
 * Build a PES reader datastructure for PS data
 *
 * - `ps` is the Program Stream to read the PES data from
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout).
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `reader` is the resulting PES reader
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int build_PS_PES_reader(PS_reader_p    ps,
                               int            give_info,
                               int            give_warnings,
                               PES_reader_p  *reader)
{
  int  err;

  err = build_PES_reader_datastructure(give_info,give_warnings,reader);
  if (err) return 1;

  (*reader)->is_TS = FALSE;
  (*reader)->psreader = ps;

  // Try to determine what sort of video this is (particularly, is it H.264)
  err = determine_PES_video_type(*reader);
  if (err)
  {
    print_err("### Error determining PS stream type\n");
    (void) free_PES_reader(reader);
    return 1;
  }
  return 0;
}

/*
 * Build a PES reader datastructure for TS data
 *
 * - `tsreader` is the Transport Stream to read the PES data from
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout).
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `program_number` is only used for TS data, and identifies which program
 *   to read. If this is 0 then the first program encountered in the first PAT
 *   will be read.
 * - `reader` is the resulting PES reader
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int build_TS_PES_reader(TS_reader_p    tsreader,
                               int            give_info,
                               int            give_warnings,
                               uint16_t       program_number,
                               PES_reader_p  *reader)
{
  int  err;

  err = build_PES_reader_datastructure(give_info,give_warnings,reader);
  if (err) return 1;

  (*reader)->is_TS = TRUE;
  (*reader)->tsreader = tsreader;

  (*reader)->program_number = program_number;
  (*reader)->output_program_number = program_number;

  // Work out the program information by reading the first PAT and
  // the first (following) PMT
  err = determine_TS_program_info(*reader);
  if (err)
  {
    print_err("### Error determining/checking program number\n");
    (void) free_PES_reader(reader);
    return 1;
  }
  return 0;
}

/*
 * Build a PES reader datastructure
 *
 * - `input` is the file to read the PES data from
 * - `is_TS` should be TRUE if the data is TS, FALSE if it is PS
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout).
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `program_number` is only used for TS data, and identifies which program
 *   to read. If this is 0 then the first program encountered in the first PAT
 *   will be read.
 * - `reader` is the resulting PES reader
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int build_PES_reader(int            input,
                            int            is_TS,
                            int            give_info,
                            int            give_warnings,
                            uint16_t       program_number,
                            PES_reader_p  *reader)
{
  int  err;

  if (is_TS)
  {
    TS_reader_p  tsreader;
    err = build_TS_reader(input,&tsreader);
    if (err)
    {
      print_err("### Error building TS specific reader\n");
      return 1;
    }
    err = build_TS_PES_reader(tsreader,give_info,give_warnings,program_number,
                              reader);
    if (err)
    {
      print_err("### Error building TS specific reader\n");
      free_TS_reader(&tsreader);
      return 1;
    }
  }
  else
  {
    PS_reader_p  ps;
    err = build_PS_reader(input,!give_info,&ps);
    if (err)
    {
      print_err("### Error building PS specific reader\n");
      return 1;
    }
    err = build_PS_PES_reader(ps,give_info,give_warnings,reader);
    if (err)
    {
      print_err("### Error building PS specific reader\n");
      free_PS_reader(&ps);
      return 1;
    }
  }
  return 0;
}

/*
 * Open a Transport Stream file for PES packet reading
 *
 * - `filename` is the name of the file to open.
 * - `program_number` identifies which program to read. If this is 0
 *   then the first program encountered in the first PAT will be read.
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout). If information messages are requested, and the
 *   program number is given as 0, the actual program number chosen will
 *   be reported as well.
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 *
 * Note that it is assumed that a particular program number will refer
 * to the same program stream throughout a TS file.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int open_PES_reader_for_TS(char          *filename,
                                  uint16_t       program_number,
                                  int            give_info,
                                  int            give_warnings,
                                  PES_reader_p  *reader)
{
  int   err;
  int   input;

  input = open_binary_file(filename,FALSE);
  if (input == -1)
  {
    fprint_err("### Unable to open input TS file %s\n",filename);
    return 1;
  }
  err = build_PES_reader(input,TRUE,give_info,give_warnings,program_number,
                         reader);
  if (err) return 1;
  return 0;
}

/*
 * Open a Program Stream file for PES packet reading
 *
 * - `filename` is the name of the file to open.
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout).
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int open_PES_reader_for_PS(char          *filename,
                                  int            give_info,
                                  int            give_warnings,
                                  PES_reader_p  *reader)
{
  int input = open_binary_file(filename,FALSE);
  if (input == -1)
  {
    fprint_err("### Unable to open input PS file %s\n",filename);
    return 1;
  }
  return build_PES_reader(input,FALSE,give_info,give_warnings,0,reader);
}

/*
 * Open a Program Stream or Transport Stream file for PES packet reading
 *
 * - `filename` is the name of the file to open.
 * - `give_info` is TRUE if information about program data, etc., should be
 *   output (to stdout).
 * - `give_warnings` is TRUE if warnings (starting with "!!!") should be
 *   output (to stderr), FALSE if they should be suppressed.
 * - `reader` is the PES reader context corresponding to the newly
 *   opened file.
 *
 * If the file is Transport Stream, then this is equivalent to a call
 * of::
 *
 *    err = open_PES_reader_for_TS(filename,0,give_info,give_warnings,&reader);
 *
 * i.e., the first program found is the program that will be read.
 *
 * The default behaviour in retrieving video and audio is:
 *
 * - Assume a single video stream, and retrieve any video packets
 * - For TS data, assume a single audio stream in the requested
 *   program, and return that, regardless of its type. If there
 *   is more than one audio stream in the program, it is not
 *   defined which will be chosen.
 * - For PS data, return the first non-Dolby audio stream encountered
 *   (and thus no audio if no non-Dolby stream is found).
 *
 * To request video only, call `set_PES_reader_video_only()`.
 *
 * To request a specific audio stream from PS data, call
 * `set_PES_reader_audio_stream()` (this may be called for TS data
 * as well, but will have no effect).
 *
 * To request Dolby audio, call `set_PES_reader_audio_private1()`
 * (again this will have no effect on TS data).
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int open_PES_reader(char          *filename,
                           int            give_info,
                           int            give_warnings,
                           PES_reader_p  *reader)
{
  int   err;
  int   input;
  int   is_TS;

  input = open_binary_file(filename,FALSE);
  if (input == -1)
  {
    fprint_err("### Unable to open input file %s\n",filename);
    return 1;
  }
  err = determine_if_TS_file(input,&is_TS);
  if (err)
  {
    (void) close_file(input);
    return 1;
  }
  if (is_TS)
    return build_PES_reader(input,TRUE,give_info,give_warnings,0,reader);
  else
    return build_PES_reader(input,FALSE,give_info,give_warnings,0,reader);
}

/*
 * Tell the PES reader whether we only want video data
 *
 * - `video_only` should be TRUE if audio is to be ignored, FALSE
 *   if audio should be retained.
 *
 * By default, the PES reader returns video data and a single audio
 * stream (taken from the first audio stream encountered).
 */
extern void set_PES_reader_video_only(PES_reader_p  reader,
                                      int           video_only)
{
  reader->video_only = video_only;
  return;
}

/*
 * Tell the PES reader which audio stream we want.
 *
 * By default, the PES reader returns video data and a single audio
 * stream (taken from the first audio stream encountered).
 *
 * - `reader` is the PES reader context
 * - `stream_number` is the number of the audio stream to read, from
 *   0 to 31 (0x1F).
 *
 * This call only has effect if the input data is PS.
 *
 * Returns 0 if all went well, or 1 if there was an error (specifically,
 * that `stream_number` was not in the range 0-31).
 */
extern int set_PES_reader_audio_stream(PES_reader_p  reader,
                                       int           stream_number)
{
  if (stream_number < 0 || stream_number > 0x1F)
  {
    fprint_err("### Audio stream number %d is not in range 0-31\n",
               stream_number);
    return 1;
  }
  reader->audio_stream_id = 0xc0 | stream_number;
  return 0;
}

/*
 * Tell the PES reader to get its audio stream from private stream 1
 * (this is the stream that is conventionally used for Dolby (AC-3)
 * in DVD data).
 *
 * By default, the PES reader returns video data and a single audio
 * stream (taken from the first audio stream encountered).
 *
 * - `reader` is the PES reader context
 *
 * This call only has effect if the input data is PS.
 */
extern void set_PES_reader_audio_private1(PES_reader_p  reader)
{
  reader->audio_stream_id = PRIVATE1_AUDIO_STREAM_ID;
  return;
}

/*
 * Tell the PES reader to use the given program information when outputting.
 *
 * For PS data (which does not contain TS program information), this is simply
 * a means of setting up sensible values.
 *
 * For TS data, this sets the values to use for output when writing to TS,
 * so that they may be different from those being read in.
 *
 * The above means that it may only be called after it is known whether
 * the data being read is PS or TS data.
 *
 * Note that calling it more than once is allowed - it will happily
 * overwrite any previous values.
 *
 * - `reader` is the PES reader context
 * - `program_number` is the program number to assume. If this is 0,
 *   then 1 will be used.
 * - `pmt_pid` is the PID to use for the PMT.
 * - `video_pid` is the PID to use for video data
 * - `audio_pid` is the PID to use for audio data (if any)
 * - `pcr_pid` is the PID to use for PCR data - this will often
 *   be the same as the `video_pid`
 */
extern void set_PES_reader_program_data(PES_reader_p  reader,
                                        uint16_t      program_number,
                                        uint32_t      pmt_pid,
                                        uint32_t      video_pid,
                                        uint32_t      audio_pid,
                                        uint32_t      pcr_pid)
{
  if (program_number == 0)
    program_number = 1;

  if (reader->is_TS)
  {
    reader->override_program_data = TRUE;
    reader->output_program_number = program_number;
    reader->output_pmt_pid        = pmt_pid;
    reader->output_pcr_pid        = pcr_pid;
    reader->output_video_pid      = video_pid;
    reader->output_audio_pid      = audio_pid;
  }
  else
  {
    reader->output_program_number = reader->program_number = program_number;
    reader->output_pmt_pid        = reader->pmt_pid        = pmt_pid;
    reader->output_pcr_pid        = reader->pcr_pid        = pcr_pid;
    reader->output_video_pid      = reader->video_pid      = video_pid;
    reader->output_audio_pid      = reader->audio_pid      = audio_pid;
    reader->got_program_data = TRUE;
    // Ideally we might also set the reader->program_map datastructure up
  }
}

/*
 * Tell the PES reader that the PS data it is reading is MPEG-4/AVC,
 * as opposed to MPEG-1/MPEG-2.
 */
extern void set_PES_reader_h264(PES_reader_p  reader)
{
  reader->is_h264 = TRUE;
  reader->video_type = VIDEO_H264;
}

/*
 * Tell the PES reader that the PS data it is reading is of
 * type `video_type` (which is assumed to be a legitimate value
 * such as VIDEO_H264, etc.)
 */
extern void set_PES_reader_video_type(PES_reader_p  reader,
                                      int           video_type)
{
  reader->video_type = video_type;
  reader->is_h264 = (video_type == VIDEO_H264);
}

/*
 * Tell the PES reader whether to output any Dolby (AC-3) audio data
 * it may read using the DVB stream type (0x06) or the ATSC stream
 * type (0x81).
 *
 * If it is reading TS data, then the default is to use whatever stream type
 * the Dolby audio was read with.
 *
 * If it is reading PS data, then the default is to assume DVB data. 
 *
 * This call only has effect if Dolby audio data is actually selected.
 */
extern void set_PES_reader_dolby_stream_type(PES_reader_p  reader,
                                             int           is_dvb)
{
  reader->override_dolby_stream_type = TRUE;
  reader->output_dolby_stream_type = (is_dvb?DVB_DOLBY_AUDIO_STREAM_TYPE:
                                      ATSC_DOLBY_AUDIO_STREAM_TYPE);
}

/*
 * Reposition the PES reader to an earlier packet
 *
 * It is the caller's responsibility to choose a sensible `posn` to seek to.
 *
 * Note that using this to reposition in a PES reader does not affect any
 * "higher" reading context using this PES reader - specifically, if data
 * is being read via an ES reader, then calling this function directly
 * will result in the ES reader losing its positional information.
 *
 * In this case, `seek_ES` should be called.
 *
 * - `reader` is the PES reader context
 * - `posn` is a packet position obtained from an earlier PES packet
 *   datastructure (this should *not* be a random offset in the input
 *   file, as that will not in general work).
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int set_PES_reader_position(PES_reader_p  reader,
                                   offset_t      posn)
{
  int err;
  // The positioning is easy
  if (reader->is_TS)
    err = seek_using_TS_reader(reader->tsreader,posn);
  else
    err = seek_using_PS_reader(reader->psreader,posn);
  if (err) return 1;

  // (although it's important not to forget to set the TS packet position for
  // the next packet...)
  reader->posn = posn;
  
  // But we must also make sure that we've lost any memory of previous
  // PES packet data that we were building up
  if (reader->is_TS)
  {
    int ii;
    for (ii=0; ii<reader->packets->length; ii++)
      free_PES_packet_data(&reader->packets->data[ii]);
    if (reader->deferred)
      free_PES_packet_data(&reader->deferred);
    reader->had_eof = FALSE;
  }
  return 0;
}

/*
 * Free a PES reader, and the relevant datastructures. Does not close
 * the underlying file.
 *
 * - `reader` is the PES reader context. This will be freed, and
 *   returned as NULL.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int free_PES_reader(PES_reader_p  *reader)
{
  int err = 0;

  if ((*reader) == NULL)
    return 0;
  if ((*reader)->packet != NULL)
    free_PES_packet_data(&(*reader)->packet);

  // Forget any file
  (*reader)->tsreader = NULL;
  (*reader)->psreader = NULL;

  if ((*reader)->program_map != NULL)
  {
    free_pmt(&(*reader)->program_map);
  }
  if ((*reader)->pmt_data != NULL)
  {
    free((*reader)->pmt_data);
    (*reader)->pmt_data = NULL;
    (*reader)->pmt_data_len = 0;
    (*reader)->pmt_data_used = 0;
  }
  if ((*reader)->packets != NULL)
  {
    free_peslist(&(*reader)->packets);
  }
  if ((*reader)->is_TS)
    free_TS_reader(&(*reader)->tsreader);
  else
    free_PS_reader(&(*reader)->psreader);
  free(*reader);
  *reader = NULL;
  return err;
}

/*
 * Close a PES reader, and free the relevant datastructures.
 *
 * - `reader` is the PES reader context. This will be freed, and
 *   returned as NULL.
 *
 * Returns 0 if all goes well, 1 if something goes wrong with closing the
 * file (although in that case, the `reader` will still have been freed).
 */
extern int close_PES_reader(PES_reader_p  *reader)
{
  int err = 0;
  int err2;

  if ((*reader) == NULL)
    return 0;

  if ((*reader)->is_TS)
  {
    if ((*reader)->tsreader != NULL)
    {
      err = close_TS_reader(&(*reader)->tsreader);
      if (err) print_err("### Error closing TS reader\n");
    }
  }
  else
  {
    if ((*reader)->psreader != NULL)
    {
      err = close_PS_file(&(*reader)->psreader);
      if (err) print_err("### Error closing PS reader\n");
    }
  }

  err2 = free_PES_reader(reader);
  if (err)
    return err;
  else
    return err2;
}

/*
 * Read in the next PES packet from the input file
 *
 * - `reader` is a PES reader context
 *
 * Returns 0 if all goes well, EOF if end of file is read, and 1 if
 * something goes wrong.
 */
extern int read_next_PES_packet(PES_reader_p  reader)
{
  int err;

  if (reader->packet != NULL)
  {
    if (reader->write_PES_packets && reader->tswriter != NULL &&
        !reader->suppress_writing &&
        !reader->dont_write_current_packet)
    {
      // Aha - we need to output the previous PES packet
      uint32_t pid;
      byte    stream_id;
      if (reader->program_index == 0)
      {
        // Output the current program information
        err = write_program_data(reader,reader->tswriter);
        if (err) return 1;
        reader->program_index = reader->program_freq;
      }
      else
        reader->program_index --;
#if DEBUG_READ_PACKETS
      if (reader->debug_read_packets)
      {
        fprint_msg("<<write PES packet at " OFFSET_T_FORMAT " len %d",
                   reader->packet->posn,
                   reader->packet->data_len);
        if (reader->packet->is_video)
        {
          fprint_msg(" VIDEO eslen %d",reader->packet->es_data_len);
          if (reader->packet->data_alignment_indicator)
            print_msg(" aligned");
        }
        print_msg(">>\n");
      }
#endif
      if (reader->packet->is_video)
      {
        pid = reader->output_video_pid;
        stream_id = DEFAULT_VIDEO_STREAM_ID;
      }
      else
      {
        pid = reader->output_audio_pid;
        stream_id = DEFAULT_AUDIO_STREAM_ID;
      }
      err = write_PES_as_TS_PES_packet(reader->tswriter,
                                       reader->packet->data,
                                       reader->packet->data_len,
                                       pid,stream_id,FALSE,0,0);
      if (err)
      {
        print_err("### Error writing out PES packet as TS\n");
        return 1;
      }
      if (reader->pes_padding)
      {
        // Add some "dummy" PES packets to bulk out our output
	int  ii;
	PES_packet_data_p  dummy;
	err = build_dummy_PES_packet_data(&dummy,reader->packet->data_len);
	if (err) return 1;
	for (ii = 0; ii < reader->pes_padding; ii++)
	{
	  err = write_PES_as_TS_PES_packet(reader->tswriter,
			                   dummy->data,dummy->data_len,
			                   pid,STREAM_ID_PADDING_STREAM,FALSE,0,0);
	  if (err)
	  {
	    print_err("### Error writing out dummy PES packet as TS\n");
	    return 1;
	  }
	}
      }
    }
    // And it's our job to free each PES packet as it is no longer needed
    free_PES_packet_data(&reader->packet);
  }
  // We always undo the "don't write the current packet flag" after we (might)
  // have written it out
  reader->dont_write_current_packet = FALSE;
 
  if (reader->is_TS)
    err = read_next_PES_packet_from_TS(reader,&reader->packet);
  else
    err = read_next_PES_packet_from_PS(reader,&reader->packet);
#if DEBUG_READ_PACKETS
  if (reader->debug_read_packets)
  {
    if (err==EOF)
      print_msg("<<EOF>>\n");
    else if (!err)
      fprint_msg("<<new   PES packet at " OFFSET_T_FORMAT ">>\n",
                 reader->packet->posn);
  }
#endif

  // Higher layers want to know if a particular PES packet had a PTS or not
  if (!err)
    reader->packet->has_PTS = PES_packet_has_PTS(reader->packet);
  return err;
}

// ============================================================
// Reading bytes from PES packets
// ============================================================
/*
 * Given an MPEG-1 PES packet, determine the offset of the ES data.
 *
 * - `data` is the PES packet data, starting "00 00 01 <stream_id>
 *   <packet_length>"
 * - `data_len` is the actual length of the data therein
 *
 * Returns the required offset (i.e., packet[offset] is the first byte
 * of the ES data within the PES packet).
 */
extern int calc_mpeg1_pes_offset(byte  *data, int data_len)
{
  int posn = 6;

  while (posn < data_len && data[posn] == 0xFF) // ignore padding bytes
    posn++;                                      // (should be <= 16, but...)

  if (posn < data_len)
  {
    if ((data[posn] & 0xC0) == 0x40)         // ignore buffer scale/size
      posn += 2;

    if ((data[posn] & 0xF0) == 0x20)         // ignore PTS
      posn += 5;
    else if ((data[posn] & 0xF0) == 0x30)    // ignore PTS and DTS
      posn += 10;
    else if (data[posn] == 0x0F)             // check for paranoia
      posn ++;
    else
    {
      fprint_err("### MPEG-1 PES packet has 0x%1xX"
                 " instead of 0x40, 0x2X, 0x3X or 0x0F\n",(data[posn]&0xF0)>>4);
      posn ++;    // what else can we do?
    }
  }
  return posn;
}
/*
 * Set up ES data access for this PES packet - i.e., set up the `es_data`
 * array as an offset into the PES packet's `data` array.
 *
 * For packets that do not contain any ES data (including PSM, etc.),
 * a zero length ES data array will be set.
 *
 * Note that only packets that appear to be video (i.e., have their
 * `is_video` flag set) will be considered as potential ES data packets.
 *
 * - `packet` is the PES packet datastructure
 */
static inline void setup_PES_as_ES(PES_packet_data_p  packet)
{
  byte  stream_id;
  int   offset;

  if (!packet->is_video)
  {
    packet->es_data = packet->data + 6;  // Perhaps safer than using NULL
    packet->es_data_len = 0;
    return;
  }
  
  stream_id = packet->data[3];

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    // There is data, but it's not ES data
    packet->es_data = packet->data + 6;  // Perhaps safer than using NULL
    packet->es_data_len = 0;
    return;
  case STREAM_ID_PADDING_STREAM:
    // There's no data, it's just padding bytes
    packet->es_data = packet->data + 6;  // Perhaps safer than using NULL
    packet->es_data_len = 0;
    return;
  default:
    break;     // Otherwise, we assume ES data
  }

  // We shan't "pull apart" PTS and DTS unless the user asks specifically
  // So we just need to work out where out data is...

  // The first two bits of the PES header flags should be '10' for H.222.0
  if (IS_H222_PES(packet->data))
  {
    // Yes, it's H.222.0
    // We have to discount:
    //   * 3 bytes of packet_start_code_prefix (00 00 01)
    //   * 1 byte  of stream_id
    //   * 2 bytes of PES_packet_length
    //   * 2 bytes of PES_header_flags
    //   * 1 byte  of PES_header_data_length  -- i.e., 9 bytes thus far
    //   * PES_header_data_length bytes of PES header data
    // before we get to our ES data
    int PES_header_data_length = packet->data[8];
    offset = 9 + PES_header_data_length;
    // The data alignment indicator seems like a sensible thing to remember
    packet->data_alignment_indicator = (packet->data[6] & 0x04) >> 2;
  }
  else
  {
    // We assume it's MPEG-1
    offset = calc_mpeg1_pes_offset(packet->data,packet->data_len);
  }
  packet->es_data = packet->data + offset;
  packet->es_data_len = packet->data_len - offset;
#if 0 // XXX
  print_data(TRUE,"      ",packet->es_data,packet->es_data_len,20);
#endif

#ifdef DEBUG
  if (reader->give_info)
    print_data(TRUE,".. ES data",packet->es_data,packet->es_data_len,20);
#endif

  return;
}

/*
 * Read in the next PES packet that contains ES data we are interested in.
 * Ignores non-video packets.
 *
 * - `reader` is a PES reader context
 *
 * Returns 0 if all goes well, EOF if end of file is read, and 1 if
 * something goes wrong.
 */
extern int read_next_PES_ES_packet(PES_reader_p       reader)
{
  for (;;)
  {
    int err = read_next_PES_packet(reader);
    if (err) return err;  // either 1 or EOF

#ifdef DEBUG
    if (reader->give_info)
    {
      fprint_msg(".. PES packet at " OFFSET_T_FORMAT " is %x (",
                 reader->packet->posn,reader->packet->data[3]);
      print_stream_id(TRUE,reader->packet->data[3]);
      fprint_msg(")%s\n",(reader->packet->is_video?" VIDEO":""));
    }
#endif

    if (reader->packet->is_video)
    {
      if (reader->debug_read_packets)
        report_PES_data_array("",reader->packet->data,reader->packet->data_len,
                              TRUE);
      // Locate its ES data, and check we have some...
      setup_PES_as_ES(reader->packet);
      if (reader->packet->es_data_len > 0)
        break;
    }
  }
  return 0;
}

// ============================================================
// PES dissection
// ============================================================
/*
 * Decode a PTS or DTS value.
 *
 * - `data` is the 5 bytes containing the encoded PTS or DTS value
 * - `required_guard` should be 2 for a PTS alone, 3 for a PTS before
 *   a DTS, or 1 for a DTS after a PTS
 * - `value` is the PTS or DTS value as decoded
 *
 * Returns 0 if the PTS/DTS value is decoded successfully, 1 if an error occurs
 */
extern int decode_pts_dts(byte     data[],
                          int      required_guard,
                          uint64_t *value)
{
  uint64_t      pts1,pts2,pts3;
  int           marker;
  char         *what;
  int           guard = (data[0] & 0xF0) >> 4;

  // Rather than try to use casts to make the arithmetic come out right on both
  // Linux-with-gcc (old-style C rules) and Windows-with-VisualC++ (C99 rules),
  // it's simpler just to use intermediates that won't get cast to "int".
  unsigned int  data0 = data[0];
  unsigned int  data1 = data[1];
  unsigned int  data2 = data[2];
  unsigned int  data3 = data[3];
  unsigned int  data4 = data[4];

  switch (required_guard)
  {
  case 2:  what = "PTS"; break;  // standalone
  case 3:  what = "PTS"; break;  // before a DTS
  case 1:  what = "DTS"; break;  // always after a PTS
  default: what = "PTS/DTS"; break;  // surely some mistake?
  }

  if (guard != required_guard)
  {
    fprint_err("!!! Guard bits at start of %s data are %x, not %x\n",
               what,guard,required_guard);
  }

  pts1 = (data0 & 0x0E) >> 1;
  marker = data0 & 0x01;
  if (marker != 1)
  {
    fprint_err("### First %s marker is not 1",what);
    return 1;
  }

  pts2 = (data1 << 7) | ((data2 & 0xFE) >> 1);
  marker = data2 & 0x01;
  if (marker != 1)
  {
    fprint_err("### Second %s marker is not 1",what);
    return 1;
  }

  pts3 = (data3 << 7) | ((data4 & 0xFE) >> 1);
  marker = data4 & 0x01;
  if (marker != 1)
  {
    fprint_err("### Third %s marker is not 1",what);
    return 1;
  }
  
  *value = (pts1 << 30) | (pts2 << 15) | pts3;
  return 0;
}

/*
 * Encode a PTS or DTS.
 *
 * - `data` is the array of 5 bytes into which to encode the PTS/DTS
 * - `guard_bits` are the required guard bits: 2 for a PTS alone, 3 for
 *   a PTS before a DTS, or 1 for a DTS after a PTS
 * - `value` is the PTS or DTS value to be encoded
 */
extern void encode_pts_dts(byte    data[],
                           int     guard_bits,
                           uint64_t value)
{
  int   pts1,pts2,pts3;

#define MAX_PTS_VALUE 0x1FFFFFFFFLL
  
  if (value > MAX_PTS_VALUE)
  {
    char        *what;
    uint64_t     temp = value;
    while (temp > MAX_PTS_VALUE)
      temp -= MAX_PTS_VALUE;
    switch (guard_bits)
    {
    case 2:  what = "PTS alone"; break;
    case 3:  what = "PTS before DTS"; break;
    case 1:  what = "DTS after PTS"; break;
    default: what = "PTS/DTS/???"; break;
    }
    fprint_err("!!! value " LLU_FORMAT " for %s is more than " LLU_FORMAT
               " - reduced to " LLU_FORMAT "\n",value,what,MAX_PTS_VALUE,temp);
    value = temp;
  }

  pts1 = (int)((value >> 30) & 0x07);
  pts2 = (int)((value >> 15) & 0x7FFF);
  pts3 = (int)( value        & 0x7FFF);

  data[0] =  (guard_bits << 4) | (pts1 << 1) | 0x01;
  data[1] =  (pts2 & 0x7F80) >> 7;
  data[2] = ((pts2 & 0x007F) << 1) | 0x01;
  data[3] =  (pts3 & 0x7F80) >> 7;
  data[4] = ((pts3 & 0x007F) << 1) | 0x01;
}

/*
 * Does the given PES packet contain a PTS?
 *
 * - `packet` is the PES packet datastructure
 *
 * Returns TRUE if it does, FALSE if it does not (or is in error)
 */
extern int PES_packet_has_PTS(PES_packet_data_p  packet)
{
  byte *data = packet->data;
  
  byte  stream_id;
  int   packet_length;
  byte *bytes;

  int PTS_DTS_flags;

  if (data[0] != 0 || data[1] != 0 || data[2] != 1)
  {
    fprint_err("### PES_packet_has_PTS: "
               "PES packet start code prefix is %02x %02x %02x, not 00 00 01",
               data[0],data[1],data[2]);
    return FALSE;
  }

  stream_id = data[3];
  packet_length = (data[4] << 8) | data[5];
  bytes = data + 6;

  // if (packet_length == 0)  // Elementary video data of unspecified length
  //   return 0;

  if (packet_length == 0)
    packet_length = packet->data_len - 6;

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    return FALSE;  // Just data bytes
  case STREAM_ID_PADDING_STREAM:
    return FALSE;  // Just padding bytes
  default:
    break;     // Some sort of data we might be interested in dissecting
  }
  
  if (IS_H222_PES(data))
  {
    // It's H.222.0
    PTS_DTS_flags = (bytes[1] & 0xC0) >> 6;
  }
  else
  {
    // We assume it's MPEG-1
    // Note that the following duplicates code in calc_mpeg1_pes_offset,
    // since it wants to look partway through the data offset...
    int posn = 0;
    // Ignore any up-front padding bytes
    while (posn < packet_length && bytes[posn] == 0xFF)
      posn++;
    if (posn == packet_length)
      return FALSE;     // no space for anything else
    if ((bytes[posn] & 0xC0) == 0x40)         // ignore buffer scale/size
      posn += 2;
    if (posn == packet_length)
      return FALSE;     // no space for PTS/DTS
    if ((bytes[posn] & 0xF0) == 0x20)         // ignore PTS
      PTS_DTS_flags = 2;
    else if ((bytes[posn] & 0xF0) == 0x30)    // ignore PTS and DTS
      PTS_DTS_flags = 3;
    else
      PTS_DTS_flags = 0;
  }

  return (PTS_DTS_flags == 2 || PTS_DTS_flags == 3);
}

/*
 * Report on the content of a PES packet - specifically, its header data.
 *
 * - `prefix` is a string to put before each line of output
 * - `data` is the packet data, and `data_len` its length
 * - `show_data` should be TRUE if the start of the data for each packet should
 *   be shown
 *
 * Returns 0 if all went well, 1 if an error occurs.
 */
extern int report_PES_data_array(char   *prefix,
                                 byte   *data,
                                 int     data_len,
                                 int     show_data)
{
  // This code was originally translated from the Python code in TS.py
  
  byte  stream_id;
  int   packet_length;
  byte *bytes;

  int err;
  uint64_t pts, dts;
  
  int got_pts = FALSE;  // pessimistic
  int got_dts = FALSE;  // pessimistic

  if (data[0] != 0 || data[1] != 0 || data[2] != 1)
  {
    fprint_err("### PES packet start code prefix is %02x %02x %02x, not 00 00 01",
               data[0],data[1],data[2]);
    return 1;
  }

  stream_id = data[3];
  packet_length = (data[4] << 8) | data[5];
  bytes = data + 6;

  // if (packet_length == 0)  // Elementary video data of unspecified length
  //   return 0;

  fprint_msg("%sPES packet: stream id %02x (",prefix,stream_id);
  print_stream_id(TRUE,stream_id);
  fprint_msg("), packet length %d",packet_length);
  if (packet_length == 0)
  {
    packet_length = data_len - 6;
    fprint_msg(" (actual length %d)",packet_length);
  }
  else if (packet_length != data_len - 6)
  {
    fprint_msg(" (actual length %d)",data_len - 6);
  }

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    print_msg("\n    Just data bytes\n");
    print_data(TRUE,"    ",bytes,packet_length,20);
    return 0;  // Just data bytes
  case STREAM_ID_PADDING_STREAM:
    print_msg("\n");
    return 0;  // Just padding bytes
  default:
    break;     // Some sort of data we might be interested in dissecting
  }

  if (IS_H222_PES(data))
  {
    // Yes, it's H.222.0
    int PES_scrambling_control;
    int PES_priority;
    int data_alignment_indicator;
    int copyright;
    int original_or_copy;
    int PTS_DTS_flags;
    int ESCR_flag;
    int ES_rate_flag;
    int DSM_trick_mode_flag;
    int additional_copy_info_flag;
    int PES_CRC_flag;
    int PES_extension_flag;
    int PES_header_data_length;
    print_msg("\n");

    PES_scrambling_control = (bytes[0] & 0x30) >> 4;
    PES_priority = (bytes[0] & 0x08) >> 3;
    data_alignment_indicator = (bytes[0] & 0x04) >> 2;
    copyright = (bytes[0] & 0x02) >> 1;
    original_or_copy = bytes[0] & 0x01;

    fprint_msg("%s    scrambling %d, priority %d, data %s, %s, %s\n",
               prefix,
               PES_scrambling_control,
               PES_priority,
               (data_alignment_indicator?"aligned":"unaligned"),
               (copyright?"copyrighted":"copyright undefined"),
               (original_or_copy?"original":"copy"));

    PTS_DTS_flags = (bytes[1] & 0xC0) >> 6;
    ESCR_flag = (bytes[1] & 0x20) >> 5;
    ES_rate_flag = (bytes[1] & 0x10) >> 4;
    DSM_trick_mode_flag = (bytes[1] & 0x08) >> 3;
    additional_copy_info_flag = (bytes[1] & 0x04) >> 2;
    PES_CRC_flag = (bytes[1] & 0x02) >> 1;
    PES_extension_flag = bytes[1] & 0x01;

    fprint_msg("%s    %s, ESCR %d, ES_rate %d, DSM trick mode %d, additional copy"
               " info %d, PES CRC %d, PES extension %d\n",
               prefix,
               (PTS_DTS_flags==2?"PTS":
                PTS_DTS_flags==3?"PTS & DTS":
                PTS_DTS_flags==0?"no PTS/DTS":"<bad PTS/DTS flag>"),
               ESCR_flag,ES_rate_flag,DSM_trick_mode_flag,
               additional_copy_info_flag,PES_CRC_flag,PES_extension_flag);

    PES_header_data_length = bytes[2];

    fprint_msg("%s    PES header data length %d\n",prefix,PES_header_data_length);

    if (PTS_DTS_flags == 2)
    {
      err = decode_pts_dts(&bytes[3],2,&pts);
      if (err) return 1;
      got_pts = TRUE;
    }
    if (PTS_DTS_flags == 3)
    {
      err = decode_pts_dts(&bytes[3],3,&pts);
      if (err) return 1;
      got_pts = TRUE;
      err = decode_pts_dts(&bytes[8],1,&dts);
      if (err) return 1;
      got_dts = TRUE;
    }
    if (got_pts || got_dts)
    {
      fprint_msg("%s    PTS " LLU_FORMAT,prefix,pts);
      if (got_dts)
        fprint_msg(", DTS " LLU_FORMAT,dts);
      print_msg("\n");
    }

    if (show_data)
    {
      bytes += 3 + PES_header_data_length;
      if (prefix && strlen(prefix) > 0)
        fprint_msg("%s",prefix);
      print_data(TRUE,"    ",bytes,packet_length-3-PES_header_data_length,20);
    }
  }
  else
  {
    // We assume it's MPEG-1
    int posn = 0;
    print_msg(" (MPEG-1)\n");
    // Ignore any up-front padding bytes
    while (posn < packet_length && bytes[posn] == 0xFF)
      posn++;
    if (posn < packet_length)
    {
      if ((bytes[posn] & 0xC0) == 0x40)         // ignore buffer scale/size
        posn += 2;
      if (posn == packet_length)
        return 0;     // no space for PTS/DTS

      if ((bytes[posn] & 0xF0) == 0x20)         // PTS
      {
        err = decode_pts_dts(&bytes[posn],2,&pts);
        if (err) return 1;
        got_pts = TRUE;
        posn += 5;
      }
      else if ((bytes[posn] & 0xF0) == 0x30)    // PTS and DTS
      {
        err = decode_pts_dts(&bytes[posn],3,&pts);
        if (err) return 1;
        got_pts = TRUE;
        posn += 5;
        err = decode_pts_dts(&bytes[posn],1,&dts);
        if (err) return 1;
        got_dts = TRUE;
        posn += 5;
      }
      else if (bytes[posn] == 0x0F)             // check for paranoia
        posn ++;
      else
      {
        fprint_err("### MPEG-1 PES packet has 0x%1xX"
                   " instead of 0x40, 0x2X, 0x3X or 0x0F\n",(bytes[posn]&0xF0)>>4);
        posn ++;    // what else can we do?
      }
      if (got_pts || got_dts)
      {
        fprint_msg("%s    PTS " LLU_FORMAT,prefix,pts);
        if (got_dts)
          fprint_msg(", DTS " LLU_FORMAT,dts);
        print_msg("\n");
      }

      if (show_data)
      {
        bytes += posn;
        if (prefix && strlen(prefix) > 0)
          fprint_msg("%s",prefix);
        print_data(TRUE,"    ",bytes,packet_length-posn,20);
      }
    }
  }

  return 0;
}

/*
 * Report on the content of a PES packet.
 *
 * This gives a longer form of report than report_PES_data_array(), and
 * can also present substream data for audio stream_types.
 *
 * - `stream_type` is the stream type of the data, or -1 if it is not
 *   known (i.e., if this packet is from PS data).
 * - `payload` is the packet data.
 * - `payload_len` is the actual length of the payload (for a TS packet,
 *   this will generally be less than the PES packet's length).
 * - if `show_data_len` is non-0 then the data for the PES packet will
 *   also be shown, up to that length
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern void report_PES_data_array2(int         stream_type,
                                   byte       *payload,
                                   int         payload_len,
                                   int         show_data_len)
{
  int      err;
  int      with_pts = FALSE;
  int      with_dts = FALSE;
  uint64_t pts, dts;
  int      PES_packet_length;
  byte    *data = NULL;
  int      data_len = 0;
  byte     stream_id;

  if (payload_len == 0)
  {
    print_msg("  Payload has length 0\n");
    return;
  }
  else if (payload == NULL)
  {
    fprint_msg("  Payload is NULL, but should be length %d\n",payload_len);
    return;
  }

  stream_id = payload[3];
  PES_packet_length = (payload[4] << 8) | payload[5];
  print_msg("  PES header\n");
  fprint_msg("    Start code:        %02x %02x %02x\n",
             payload[0],payload[1],payload[2]);
  fprint_msg("    Stream ID:         %02x   (%d) ",stream_id,stream_id);
  print_h262_start_code_str(stream_id);
  print_msg("\n");
  fprint_msg("    PES packet length: %04x (%d)\n",
             PES_packet_length,PES_packet_length);

  if (IS_H222_PES(payload))
  {
    // Looks like H.222.0
    switch (stream_id)
    {
    case STREAM_ID_PROGRAM_STREAM_MAP:
    case STREAM_ID_PRIVATE_STREAM_2:
    case STREAM_ID_ECM_STREAM:
    case STREAM_ID_EMM_STREAM:
    case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
    case STREAM_ID_DSMCC_STREAM:
    case STREAM_ID_H222_E_STREAM:
      print_msg("    Just data bytes\n");
      print_data(TRUE,"    Data",payload+6,payload_len-6,1000);
      return;  // Just data bytes
    case STREAM_ID_PADDING_STREAM:
      print_msg("    Padding stream\n");
      return;  // Just padding bytes
    default:
      break;     // Some sort of data we might be interested in dissecting
    }

    fprint_msg("    Flags:             %02x %02x",payload[6],payload[7]);
    if (payload[6] != 0)
    {
      int scramble = (payload[6] & 0x30) >> 8;
      if (scramble != 0) fprint_msg(" scramble-control %d",scramble);
      if (ON(payload[6],0x08)) print_msg(" PES-priority");
      if (ON(payload[6],0x04)) print_msg(" data-aligned");
      if (ON(payload[6],0x02)) print_msg(" copyright");
      if (ON(payload[6],0x01)) print_msg(" original/copy");
    }
    if (payload[7] != 0)
    {
      print_msg(" :");
      if (ON(payload[7],0x80))
      {
        with_pts = TRUE;
        print_msg(" PTS");
      }
      if (ON(payload[7],0x40))
      {
        with_dts = TRUE;
        print_msg(" DTS");
      }
      if (ON(payload[7],0x20)) print_msg(" ESCR");
      if (ON(payload[7],0x10)) print_msg(" ES-rate");
      if (ON(payload[7],0x08)) print_msg(" DSM-trick-mode");
      if (ON(payload[7],0x04)) print_msg(" more-copy-info");
      if (ON(payload[7],0x02)) print_msg(" CRC");
      if (ON(payload[7],0x01)) print_msg(" extension");
    }
    print_msg("\n");
    fprint_msg("    PES header len %d\n", payload[8]);

    if (with_pts)
    {
      err = decode_pts_dts(&(payload[9]),(with_dts?3:2),&pts);
      if (!err)
        fprint_msg("    PTS " LLU_FORMAT "\n",pts);
    }
    if (with_dts)
    {
      err = decode_pts_dts(&(payload[14]),1,&dts);
      if (!err)
        fprint_msg("    DTS " LLU_FORMAT "\n",dts);
    }

    data = payload + 9 + payload[8];
    data_len = payload_len - 9 - payload[8];

    // We know this is the start of a packet. If it is private_stream_1,
    // look to see if it is AC3 or DTS
    // If it is stream type 0x81, then do the same...
    // (maybe should do this for *any* of the 0x8N private streams?)
    if (stream_type == 0x06 || stream_type == 0x81)
    {
      if (data_len >= 2 && data[0] == 0x0B && data[1] == 0x77)
        print_msg("  AC-3 audio data\n");
      else if (data_len >= 4 &&
               data[0] == 0x7F && data[1] == 0xFE &&
               data[1] == 0x80 && data[2] == 0x01)
        print_msg("  DTS audio data\n");
    }
  }
  else
  {
    // We assume it's MPEG-1
    int posn = 0;
    print_msg("    MPEG-1 packet layer packet\n");

    if (stream_id != STREAM_ID_PRIVATE_STREAM_2)
    {
      // Skip any up-front padding bytes
      while (posn < PES_packet_length && payload[6+posn] == 0xFF)
        posn++;
      if (posn != 0)
        fprint_msg("      %d stuffing byte%s\n",posn,posn==1?"":"s");

      if (posn < PES_packet_length)
      {
        if ((payload[6+posn] & 0xC0) == 0x40)
        {
          fprint_msg("      STD buffer scale %d\n",ON(payload[6+posn],5));
          fprint_msg("      STD buffer size %d\n",(payload[6+posn] & 0x1F) << 8 |
                 (payload[6+posn+1]));
          posn += 2;
        }
        if (posn == PES_packet_length)
          return;     // no space for PTS/DTS

        if ((payload[6+posn] & 0xF0) == 0x20)         // PTS
        {
          err = decode_pts_dts(&payload[6+posn],2,&pts);
          if (err) return;
          with_pts = TRUE;
          posn += 5;
        }
        else if ((payload[6+posn] & 0xF0) == 0x30)    // PTS and DTS
        {
          err = decode_pts_dts(&payload[6+posn],3,&pts);
          if (err) return;
          with_pts = TRUE;
          posn += 5;
          err = decode_pts_dts(&payload[6+posn],1,&dts);
          if (err) return;
          with_dts = TRUE;
          posn += 5;
        }
        else if (payload[6+posn] == 0x0F)             // check for paranoia
          posn ++;
        else
        {
          fprint_err("### MPEG-1 PES packet has 0x%1xX"
                     " instead of 0x40, 0x2X, 0x3X or 0x0F\n",(payload[posn]&0xF0)>>4);
          posn ++;    // what else can we do?
        }
        if (with_pts || with_dts)
        {
          fprint_msg("      PTS " LLU_FORMAT "\n",pts);
          if (with_dts)
            fprint_msg("      DTS " LLU_FORMAT "\n",dts);
          print_msg("\n");
        }

        data = payload + 6 + posn;
        data_len = payload_len - 6 - posn;
      }
    }
    else
    {
      data = payload + 6;
      data_len = payload_len - 6;
    }
  }
  if (show_data_len)
    print_data(TRUE,"    Data",data,data_len,show_data_len);
}

/*
 * If the given PES packet data contains a PTS field, return it
 *
 * - `data` is the data for this PES packet
 * - `data_len` is its length
 * - `got_pts` is TRUE if a PTS field was found, in which case
 * - `pts` is that PTS value
 *
 * Returns 0 if all went well, 1 if an error occurs.
 */
extern int find_PTS_in_PES(byte      data[],
                           int32_t   data_len,
                           int      *got_pts,
                           uint64_t *pts)
{
  byte  stream_id;
  int   packet_length;
  byte *bytes;

  int PTS_DTS_flags;

  *got_pts = FALSE;  // pessimistic

  if (data[0] != 0 || data[1] != 0 || data[2] != 1)
  {
    fprint_err("### find_PTS_in_PES:"
               " PES packet start code prefix is %02x %02x %02x, not 00 00 01\n",
            data[0],data[1],data[2]);
    return 1;
  }

  stream_id = data[3];
  packet_length = (data[4] << 8) | data[5];

  // if (packet_length == 0)  // Elementary video data of unspecified length
  //   return 0;

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    return 0;  // Just data bytes
  case STREAM_ID_PADDING_STREAM:
    return 0;  // Just padding bytes
  default:
    break;     // Some sort of data we might be interested in dissecting
  }

  bytes = data + 6;
  if (IS_H222_PES(data))
  {
    // Yes, it's H.222.0
    PTS_DTS_flags = (bytes[1] & 0xC0) >> 6;

    if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3)
    {
      int err = decode_pts_dts(&bytes[3],PTS_DTS_flags,pts);
      if (err) return 1;
      *got_pts = TRUE;
    }
  }
  else
  {
    int posn = 0;
    int marker;
    // We assume it's MPEG-1
    // Ignore any up-front padding bytes
    while (posn < packet_length && bytes[posn] == 0xFF)
      posn++;
    if (posn < packet_length)
    {
      if ((bytes[posn] & 0xC0) == 0x40)         // ignore buffer scale/size
        posn += 2;
      if (posn == packet_length)
        return 0;     // no space for PTS/DTS
      marker = (bytes[posn] & 0xF0) >> 4;
      if (marker == 2 ||       // PTS
          marker == 3)         // PTS and DTS
      {
        int err = decode_pts_dts(&bytes[posn],marker,pts);
        if (err) return 1;
        *got_pts = TRUE;
      }
    }
  }
  return 0;
}

/*
 * If the given PES packet data contains a DTS field, return it
 *
 * - `data` is the data for this PES packet
 * - `data_len` is its length
 * - `got_dts` is TRUE if a DTS field was found, in which case
 * - `dts` is that DTS value
 *
 * Returns 0 if all went well, 1 if an error occurs.
 */
extern int find_DTS_in_PES(byte      data[],
                           int32_t   data_len,
                           int      *got_dts,
                           uint64_t *dts)
{
  byte  stream_id;
  int   packet_length;
  byte *bytes;

  int PTS_DTS_flags;

  *got_dts = FALSE;  // pessimistic

  if (data[0] != 0 || data[1] != 0 || data[2] != 1)
  {
    fprint_err("### find_DTS_in_PES:"
               " PES packet start code prefix is %02x %02x %02x, not 00 00 01\n",
               data[0],data[1],data[2]);
    return 1;
  }

  stream_id = data[3];
  packet_length = (data[4] << 8) | data[5];

  // if (packet_length == 0)  // Elementary video data of unspecified length
  //   return 0;

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    return 0;  // Just data bytes
  case STREAM_ID_PADDING_STREAM:
    return 0;  // Just padding bytes
  default:
    break;     // Some sort of data we might be interested in dissecting
  }

  bytes = data + 6;
  if (IS_H222_PES(data))
  {
    // Yes, it's H.222.0
    PTS_DTS_flags = (bytes[1] & 0xC0) >> 6;

    if (PTS_DTS_flags == 3)
    {
      // err = decode_pts_dts(&bytes[3],3,&pts);
      int err = decode_pts_dts(&bytes[8],1,dts);
      if (err) return 1;
      *got_dts = TRUE;
    }
  }
  else
  {
    int posn = 0;
    // We assume it's MPEG-1
    // Ignore any up-front padding bytes
    while (posn < packet_length && bytes[posn] == 0xFF)
      posn++;
    if (posn < packet_length)
    {
      if ((bytes[posn] & 0xC0) == 0x40)         // ignore buffer scale/size
        posn += 2;
      if (posn == packet_length)
        return 0;     // no space for PTS/DTS

      if ((bytes[posn] & 0xF0) == 0x30)    // PTS and DTS
      {
        int err = decode_pts_dts(&bytes[posn+5],1,dts);
        if (err) return 1;
        *got_dts = TRUE;
      }
    }
  }
  return 0;
}

/*
 * If the given PES packet data contains a PTS and/or DTS field, return it
 *
 * - `data` is the data for this PES packet
 * - `data_len` is its length
 * - `got_pts` is TRUE if a PTS field was found, in which case
 * - `pts` is that PTS value
 * - `got_dts` is TRUE if a DTS field was found, in which case
 * - `dts` is that DTS value
 *
 * Returns 0 if all went well, 1 if an error occurs.
 */
extern int find_PTS_DTS_in_PES(byte      data[],
                               int32_t   data_len,
                               int      *got_pts,
                               uint64_t *pts,
                               int      *got_dts,
                               uint64_t *dts)
{
  byte  stream_id;
  int   packet_length;
  byte *bytes;

  int PTS_DTS_flags;

  *got_pts = FALSE;  // pessimistic
  *got_dts = FALSE;

  if (data[0] != 0 || data[1] != 0 || data[2] != 1)
  {
    fprint_err("### find_PTS_DTS_in_PES"
               ": PES packet start code prefix is %02x %02x %02x, not 00 00 01\n",
               data[0],data[1],data[2]);
    return 1;
  }

  stream_id = data[3];
  packet_length = (data[4] << 8) | data[5];

  // if (packet_length == 0)  // Elementary video data of unspecified length
  //   return 0;

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    return 0;  // Just data bytes
  case STREAM_ID_PADDING_STREAM:
    return 0;  // Just padding bytes
  default:
    break;     // Some sort of data we might be interested in dissecting
  }

  bytes = data + 6;
  if (IS_H222_PES(data))
  {
    // Yes, it's H.222.0
    PTS_DTS_flags = (bytes[1] & 0xC0) >> 6;

    if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3)
    {
      int err = decode_pts_dts(&bytes[3],PTS_DTS_flags,pts);
      if (err) return 1;
      *got_pts = TRUE;
    }
    if (PTS_DTS_flags == 3)
    {
      // err = decode_pts_dts(&bytes[3],3,&pts);
      int err = decode_pts_dts(&bytes[8],1,dts);
      if (err) return 1;
      *got_dts = TRUE;
    }
  }
  else
  {
    int posn = 0;
    int marker;
    // We assume it's MPEG-1
    // Ignore any up-front padding bytes
    while (posn < packet_length && bytes[posn] == 0xFF)
      posn++;
    if (posn < packet_length)
    {
      if ((bytes[posn] & 0xC0) == 0x40)         // ignore buffer scale/size
        posn += 2;
      if (posn == packet_length)
        return 0;     // no space for PTS/DTS
      marker = (bytes[posn] & 0xF0) >> 4;
      if (marker == 2 ||       // PTS
          marker == 3)         // PTS and DTS
      {
        int err = decode_pts_dts(&bytes[posn],marker,pts);
        if (err) return 1;
        *got_pts = TRUE;
      }
      if (marker == 3)    // PTS and DTS
      {
        int err = decode_pts_dts(&bytes[posn+5],1,dts);
        if (err) return 1;
        *got_dts = TRUE;
      }
    }
  }

  // If we have no DTS then it is the same as PTS
  if (*got_pts && !*got_dts)
  {
    *dts = *pts;
    *got_dts = TRUE;
  }
  return 0;
}

/*
 * If the given PES packet data contains an ESCR field, return it
 *
 * - `data` is the data for this PES packet
 * - `data_len` is its length
 * - `got_escr` is TRUE if an ESCR field was found, in which case
 * - `escr` is that ESCR value
 *
 * Returns 0 if all went well, 1 if an error occurs.
 */
extern int find_ESCR_in_PES(byte      data[],
                            int32_t   data_len,
                            int      *got_escr,
                            uint64_t *escr)
{
  byte  stream_id;
//  int   packet_length;
  byte *bytes;

  *got_escr = FALSE;  // pessimistic
  *escr = 0;

  if (data[0] != 0 || data[1] != 0 || data[2] != 1)
  {
    fprint_err("### find_ESCR_in_PES:"
               " PES packet start code prefix is %02x %02x %02x, not 00 00 01\n",
               data[0],data[1],data[2]);
    return 1;
  }

  stream_id = data[3];
//  packet_length = (data[4] << 8) | data[5];

  // if (packet_length == 0)  // Elementary video data of unspecified length
  //   return 0;

  switch (stream_id)
  {
  case STREAM_ID_PROGRAM_STREAM_MAP:
  case STREAM_ID_PRIVATE_STREAM_2:
  case STREAM_ID_ECM_STREAM:
  case STREAM_ID_EMM_STREAM:
  case STREAM_ID_PROGRAM_STREAM_DIRECTORY:
  case STREAM_ID_DSMCC_STREAM:
  case STREAM_ID_H222_E_STREAM:
    return 0;  // Just data bytes
  case STREAM_ID_PADDING_STREAM:
    return 0;  // Just padding bytes
  default:
    break;     // Some sort of data we might be interested in dissecting
  }

  bytes = data + 6;
  if (IS_H222_PES(data))    // H.222.0 may have ESCR, MPEG-1 mayn't
  {
    // Yes, it's H.222.0
    *got_escr = (bytes[1] & 0x20) == 0x20;
    if (*got_escr)
    {
      uint64_t  ESCR_base;
      uint32_t  ESCR_extn;
      int PTS_DTS_flags = (bytes[1] & 0xC0) >> 6;
      int offset;
      if (PTS_DTS_flags == 2)
        offset = 2 + 5;
      else if (PTS_DTS_flags == 3)
        offset = 2 + 10;
      else
        offset = 2 + 0;     // or so we hope
      ESCR_base =
        (bytes[offset+4] >>  3) |
        (bytes[offset+3] <<  5) |
        (bytes[offset+2] << 13) |
        (bytes[offset+1] << 20) |
        ((((uint64_t)bytes[offset]) & 0x03) << 28) |
        ((((uint64_t)bytes[offset]) & 0x38) << 27);
      ESCR_extn =
        (bytes[offset+5] >> 1) |
        (bytes[offset+4] << 7);
      *escr = ESCR_base * 300 + ESCR_extn;
    }
  }
  return 0;
}

// ============================================================
// Server support
// ============================================================
/*
 * Packets can be written out to a client via a TS writer, as a
 * "side effect" of reading them. The original mechanism was to
 * write out PES packets (as TS) as they are read. This will work
 * for PS or TS data, and writes out only those PES packets that
 * have been read for video or audio data.
 *
 * An alternative, which will only work for TS input data, is
 * to write out TS packets as they are read. This will write all
 * TS packets to the client.
 *
 * - `reader` is our PES reader context
 * - `tswriter` is the TS writer
 * - if `write_PES`, then write PES packets out as they are read from
 *   the input, otherwise write TS packets.
 * - `program_freq` is how often to write out program data (PAT/PMT)
 *   if we are writing PES data (if we are writing TS data, then the
 *   program data will be in the original TS packets)
 */
extern void set_server_output(PES_reader_p  reader,
                              TS_writer_p   tswriter,
                              int           write_PES,
                              int           program_freq)
{
  reader->tswriter = tswriter;
  reader->program_freq = program_freq;
  reader->program_index = 0;
  reader->write_PES_packets = write_PES;
  reader->write_TS_packets = !write_PES;
  reader->suppress_writing = FALSE;
  return;
}

/*
 * Start packets being written out to a TS writer (again).
 *
 * If packets were already being written out, this does nothing.
 *
 * If set_server_output() has not been called to define a TS writer
 * context, this will have no effect.
 *
 * If `reader` is NULL, nothing is done.
 */
extern void start_server_output(PES_reader_p  reader)
{
  if (reader != NULL)
    reader->suppress_writing = FALSE;
  return;
}

/*
 * Stop packets being written out to a TS writer.
 *
 * If packets were already not being written out, this does nothing.
 *
 * If `reader` is NULL, nothing is done.
 */
extern void stop_server_output(PES_reader_p  reader)
{
  if (reader != NULL)
    reader->suppress_writing = TRUE;
  return;
}

/*
 * When outputting PES packets in "normal play" mode, add ``extra`` PES
 * packets (of the same size as each real packet) to the output. This
 * makes the amount of data output be about ``extra``+1 times the amount
 * read (the discrepancy is due to any program data being written).
 *
 * This "expansion" or "padding" of the data can be useful for benchmarking
 * the recipient, as the extra data (which has an irrelevant stream id)
 * will be ignored by the video processor, but not by preceding systems.
 *
 * This does nothing if TS packets are being output directly.
 *
 * - `reader` is our PES reader context
 * - `extra` is how many extra packets to output per "real" packet.
 */
extern void set_server_padding(PES_reader_p  reader,
                               int           extra)
{
  reader->pes_padding = extra;
  return;
}

/*
 * Write out TS program data based on the information we have within the given
 * PES reader context (as amended by any calls of
 * `set_PES_reader_program_data`).
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int write_program_data(PES_reader_p  reader,
                              TS_writer_p   output)
{
  // We know we support at most two program streams for output
  int      num_progs = 0;
  uint32_t prog_pids[2];
  byte     prog_type[2];
  int      err;
  uint32_t pcr_pid;

  // If we are writing out TS data as a side effect of reading TS when
  // assembling our PES packets, we should not write out any program
  // data ourselves, as it is (or should be) already in the TS data
  if (reader->write_TS_packets &&
      !reader->suppress_writing)        // should we care about suppression?
    return 0;

  // Of course, if we haven't *found* any program information yet,
  // there's not much we can do (even if the user is overriding the
  // program information for TS data, we still won't have worked out
  // exactly what we're doing until we've read the program information,
  // so this is probably still a sensible restriction)
  if (!reader->got_program_data)
    return 0;

  if (reader->is_TS)
  {
    // For TS, we can use the stream types from the PMT itself
    if (reader->video_pid != 0)
    {
      pmt_stream_p stream = pid_stream_in_pmt(reader->program_map,
                                              reader->video_pid);
      if (stream == NULL)
      {
        fprint_err("### Cannot find video PID %04x in program map\n",
                   reader->video_pid);
        return 1;
      }
      prog_pids[0] = reader->output_video_pid; // may not be the same
      prog_type[0] = stream->stream_type;
      num_progs = 1;
    }
    if (reader->audio_pid != 0)
    {
      pmt_stream_p stream = pid_stream_in_pmt(reader->program_map,
                                              reader->audio_pid);
      if (stream == NULL)
      {
        fprint_err("### Cannot find audio PID %04x in program map\n",
                   reader->audio_pid);
        return 1;
      }
      prog_pids[num_progs] = reader->output_audio_pid; // may not be the same
      prog_type[num_progs] = stream->stream_type;
      num_progs++;
    }
  }
  else
  {
    // For PS, we have to be given appropriate PIDs (which we'll assume the
    // user has done via the reader), and we need to deduce stream types from
    // the stream ids.

    // XXX For audio data, we can't yet tell what sort of audio we're reading,
    // so we'll make a (quiet possibly wrong) guess.

    num_progs = 1;
    prog_pids[0] = reader->output_video_pid;
    switch (reader->video_type)
    {
    case VIDEO_H264:
      prog_type[0] = AVC_VIDEO_STREAM_TYPE;
      break;
    case VIDEO_H262:
      prog_type[0] = MPEG2_VIDEO_STREAM_TYPE;
      break;
    case VIDEO_AVS:
      prog_type[0] = AVS_VIDEO_STREAM_TYPE;
      break;
    default:
      prog_type[0] = MPEG2_VIDEO_STREAM_TYPE;   // what else to do?
      break;
    }

    prog_pids[1] = reader->output_audio_pid;
    if (reader->audio_stream_id == 0)
    {
      // The user has asked for (not private data) audio, but we haven't
      // found it yet. Make something sensible up...
      prog_type[1] = MPEG2_AUDIO_STREAM_TYPE; // a random guess
    }
    else
    {
      if (reader->audio_stream_id == PRIVATE1_AUDIO_STREAM_ID)
        prog_type[1] = reader->output_dolby_stream_type;
      else
        prog_type[1] = MPEG2_AUDIO_STREAM_TYPE; // a random guess
    }
    num_progs = 2;
  }

  pcr_pid = reader->output_pcr_pid;
  if (pcr_pid == 0)
    pcr_pid = reader->output_video_pid;

#if SHOW_PROGRAM_INFO
  if (reader->give_info)
  {
    fprint_msg("PROGRAM %d: pmt %x (%d), pcr %x (%d)\n"
               "           video %x (%d) type %02x (%s)\n",
               reader->output_program_number,
               reader->output_pmt_pid,reader->output_pmt_pid,
               pcr_pid,pcr_pid,
               reader->output_video_pid,reader->output_video_pid,
               prog_type[0],h222_stream_type_str(prog_type[0]));
    if (num_progs == 2)
      fprint_msg("         audio %x (%d) type %02x (%s)\n",
                 reader->output_audio_pid,reader->output_audio_pid,
                 prog_type[1],h222_stream_type_str(prog_type[1]));
  }
#endif

  err = write_TS_program_data2(output,
                               1, // transport stream id
                               reader->output_program_number,
                               reader->output_pmt_pid,pcr_pid,
                               num_progs,prog_pids,prog_type);
  if (err)
  {
    print_err("### Error writing out TS program data\n");
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

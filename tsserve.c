/*
 * Serve TS packets from TS or PS data, supporting playing forwards
 * at normal and accelerated speeds, and reverse play.
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
#include <math.h>

#ifdef _WIN32
#include <stddef.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#else // _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>    // WNOHANG
#include <netinet/in.h>  // sockaddr_in
#include <signal.h>      // sigaction, etc.
#endif // _WIN32

#include <time.h>       // Sleeping and timing

#include "compat.h"
#include "ts_fns.h"
#include "ps_fns.h"
#include "pes_fns.h"
#include "accessunit_fns.h"
#include "nalunit_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "tswrite_fns.h"
#include "es_fns.h"
#include "h262_fns.h"
#include "filter_fns.h"
#include "reverse_fns.h"
#include "version.h"

//#define DEBUG
#define SHOW_REVERSE_DATA 1
#define DEBUG_COMMANDS 1

#define TIME_SKIPPING 1
#if TIME_SKIPPING
#include <time.h>
#endif


#define DEFAULT_REVERSE_FREQUENCY 8
#define DEFAULT_FORWARD_FREQUENCY 8
#define FRAMES_FOR_ONE_SECOND     25

#define SMALL_SKIP_DISTANCE  10*FRAMES_FOR_ONE_SECOND   // 10 seconds
#define BIG_SKIP_DISTANCE    3*60*FRAMES_FOR_ONE_SECOND //  3 minutes

static int extra_info = 0;

// What we are to do
enum ACTION
{
  ACTION_SERVER,   // The default action is to be a server
  ACTION_CMD,      // An alternative is to connect and read commands
  ACTION_TEST,     // One of the testing modes
};

#define MAX_INPUT_FILES 10  // i.e., 0..9

// Command line data
// There's a lot of data from the command line that needs passing down
// from the top level to the main processing functions, so let's package
// it up neatly
struct tsserve_context
{
  char    *input_names[MAX_INPUT_FILES];  // The files to read from
  int      default_file_index;            // Which one is the default

  int      video_only;   // As it says - no audio?
  int      pad_start;    // Number of filler packets to output at start
  
  int      ffrequency;   // Fast forward frequency when filtering
  int      rfrequency;   // Base reverse frequency
  int      with_seq_hdrs;// For H.262, output sequence headers when not
                         // doing normal play?

  int      pes_padding;   // Number of dummy PES packets to output per real packet
  int      drop_packets;  // 0 or drop TS packets every <n> on output
  int      drop_number;   // how many packets to drop

  // Program Stream specific options
  uint32_t pmt_pid;
  uint32_t audio_pid;
  uint32_t video_pid;
  uint32_t pcr_pid;
  int      want_h262;
  int      dolby_is_dvb;
  int      force_stream_type;
  int      repeat_program_every;

  // Transport Stream specific options
  int      tsdirect;
};
typedef struct tsserve_context *tsserve_context_p;


// ============================================================
// Unions to give us a single view of the two forms of data stream
// ============================================================
// The form of this single view is limited solely to what is needed
// in this program - it is not intended to be a general unification
// of the two types of data.

// Accessing the data stream
union u_stream_context
{
  h262_context_p         h262;
  access_unit_context_p  h264;
};
struct _stream_context
{
  int   is_h262;
  int   program_number;
  union u_stream_context u;
};
typedef struct _stream_context  stream_context;
typedef struct _stream_context *stream_context_p;

// Filtering
union u_filter_context
{
  h262_filter_context_p  h262;
  h264_filter_context_p  h264;
};
struct _filter_context
{
  int   is_h262;
  union u_filter_context u;
};
typedef struct _filter_context  filter_context;
typedef struct _filter_context *filter_context_p;

// Pictures
union u_picture
{
  int   is_h262;
  h262_picture_p h262;
  access_unit_p  h264;
};

struct _picture
{
  int   is_h262;
  union u_picture u;
  int type; // For H.262, the picture coding type. 0xFF means seq hdr
};
typedef struct _picture  picture;
typedef struct _picture *picture_p;
  

// ============================================================
// Utilities to hide the difference between the two data stream types
// ============================================================

// A macro to avoid my mistyping this on the several occasions I need it
#define EXTRACT_ES_FROM_STREAM(stream) \
  ((stream).is_h262?(stream).u.h262->es:(stream).u.h264->nac->es)

// A related macro for reverse data
#define EXTRACT_REVERSE_FROM_STREAM(stream) \
  ((stream).is_h262?(stream).u.h262->reverse_data:(stream).u.h264->reverse_data)


/*
 * Note that `program_number` should be 1 or more.
 */
static int build_stream(ES_p             es,
                        int              is_h262,
                        int              program_number,
                        stream_context  *stream)
{
  int err;
  stream->is_h262 = is_h262;
  stream->program_number = program_number;
  if (is_h262)
  {
    err = build_h262_context(es,&(stream->u.h262));
    if (err)
    {
      print_err("### Error building H.262 context\n");
      return 1;
    }
  }
  else
  {
    err = build_access_unit_context(es,&(stream->u.h264));
    if (err)
    {
      print_err("### Error building H.264 access unit context\n");
      return 1;
    }
  }
  return 0;
}

static void close_stream(stream_context  stream)
{
  if (stream.is_h262)
    free_h262_context(&(stream.u.h262));
  else
    free_access_unit_context(&(stream.u.h264));
}

static int build_and_attach_reverse(stream_context   stream,
                                    reverse_data_p  *reverse_data)
{
  int err;
  err = build_reverse_data(reverse_data,!stream.is_h262);
  if (err)
  {
    print_err("### Unable to build reverse memory\n");
    return 1;
  }

  if (stream.is_h262)
    add_h262_reverse_context(stream.u.h262,*reverse_data);
  else
    add_access_unit_reverse_context(stream.u.h264,*reverse_data);

  return 0;
}

static int build_filter_context(stream_context   stream,
                                int              is_strip,
                                int              frequency,
                                filter_context  *fcontext)
{
  int err;
  fcontext->is_h262 = stream.is_h262;
  if (stream.is_h262)
  {
    if (is_strip)
      err = build_h262_filter_context_strip(&(fcontext->u.h262),
                                            stream.u.h262,TRUE);
    else
      err = build_h262_filter_context(&(fcontext->u.h262),
                                      stream.u.h262,frequency);
  }
  else
  {
    if (is_strip)
      err = build_h264_filter_context_strip(&(fcontext->u.h264),
                                            stream.u.h264,TRUE);
    else
      err = build_h264_filter_context(&(fcontext->u.h264),
                                      stream.u.h264,frequency);
  }
  return err;
}

static void free_filter_context(filter_context  fcontext)
{
  if (fcontext.is_h262)
    free_h262_filter_context(&(fcontext.u.h262));
  else
    free_h264_filter_context(&(fcontext.u.h264));
}

/*
 * "Reset" a stream, such that the picture reading contexts do not contain
 * any past memory.
 */
static inline void reset_stream(stream_context  stream)
{
  if (stream.is_h262)
  {
    if (stream.u.h262->last_item)
      free_h262_item(&stream.u.h262->last_item);
  }
  else
  {
    reset_access_unit_context(stream.u.h264);
  }
}

/*
 * Retrieve the next picture. Doesn't distinguish H.262 sequence headers
 * and pictures.
 */
static inline int get_next_picture(stream_context   stream,
                                   int              verbose,
                                   int              quiet,
                                   picture         *pic)
{
  int  err;
  if (stream.is_h262)
  {
    h262_picture_p  picture;
    err = get_next_h262_frame(stream.u.h262,verbose,quiet,&picture);
    if (err) return err;
    pic->is_h262 = TRUE;
    pic->u.h262 = picture;
    if (picture->is_picture)
      pic->type = picture->picture_coding_type;
    else
      pic->type = 0xff;
  }
  else
  {
    access_unit_p  unit;
    err = get_next_h264_frame(stream.u.h264,quiet,verbose,&unit);
    if (err) return err;
    pic->is_h262 = FALSE;
    pic->u.h264 = unit;
  }
  return 0;
}

static inline void free_picture(picture   *pic)
{
  if (pic->is_h262)
    free_h262_picture(&pic->u.h262);
  else
    free_access_unit(&pic->u.h264);
  pic->type = 0;
}

/*
 * Needs to be told if the picture is H.262 or not, because this may be
 * called on an unused instance of the picture data structure.
 */
static inline void unset_picture(int      is_h262,
                                 picture *pic)
{
  if (is_h262)
    pic->u.h262 = NULL;
  else
    pic->u.h264 = NULL;
  pic->is_h262 = is_h262;
}

static inline int is_null_picture(picture  pic)
{
  if (pic.is_h262)
    return pic.u.h262 == NULL;
  else
    return pic.u.h264 == NULL;
}

// NB: there is already a macro called "is_seq_header"
static inline int is_non_frame(picture  pic)
{
  return (pic.is_h262 && pic.type == 0xff);
}

static inline int is_reference_picture(picture  pic)
{
  if (pic.is_h262)
    return (pic.type == 1 || pic.type == 2);
  else
    return (pic.u.h264->primary_start != NULL &&
            pic.u.h264->primary_start->nal_ref_idc != 0);
}

static inline int is_I_or_IDR_picture(picture  pic)
{
  if (pic.is_h262)
    return (pic.type == 1);
  else
    return (pic.u.h264->primary_start != NULL &&
            pic.u.h264->primary_start->nal_ref_idc != 0 &&
            (pic.u.h264->primary_start->nal_unit_type == NAL_IDR ||
             all_slices_I(pic.u.h264)));
}

static void print_picture(picture  pic)
{
  if (pic.is_h262)
  {
    if (pic.type == 0xff)
      print_msg("sequence header");
    else
      fprint_msg("%s picture",H262_PICTURE_CODING_STR(pic.type));
  }
  else
  {
    if (pic.u.h264->primary_start == NULL)
      print_msg("<null>");
    else
      fprint_msg("idc %d/type %d (%s)",
                 pic.u.h264->primary_start->nal_ref_idc,
                 pic.u.h264->primary_start->nal_unit_type,
                 NAL_UNIT_TYPE_STR(pic.u.h264->primary_start->nal_unit_type));
  }
}

static inline int write_picture_as_TS(stream_context stream,
                                      TS_writer_p    output,
                                      picture        pic)
{
  ES_p  es = EXTRACT_ES_FROM_STREAM(stream);
  if (stream.is_h262)
    return write_h262_picture_as_TS(output,pic.u.h262,
                                    es->reader->output_video_pid);
  else
    return write_access_unit_as_TS(pic.u.h264,stream.u.h264,
                                   output,es->reader->output_video_pid);
}

static inline void reset_filter_context(filter_context  fcontext,
                                        int             frequency)
{
  if (fcontext.is_h262)
  {
    reset_h262_filter_context(fcontext.u.h262);
    fcontext.u.h262->freq = frequency;
  }
  else
  {
    reset_h264_filter_context(fcontext.u.h264);
    fcontext.u.h264->freq = frequency;
  }
}

static inline int get_next_stripped(filter_context  fcontext,
                                    int             verbose,
                                    int             quiet,
                                    picture        *seq_hdr,
                                    picture        *this_picture,
                                    int            *delta_pictures_seen)
{
  int  err;

  unset_picture(fcontext.is_h262,seq_hdr);
  unset_picture(fcontext.is_h262,this_picture);

  if (fcontext.is_h262)
  {
    h262_picture_p   _this_picture = NULL;
    h262_picture_p   _seq_hdr = NULL;
    err = get_next_stripped_h262_frame(fcontext.u.h262,verbose,quiet,&_seq_hdr,
                                       &_this_picture,delta_pictures_seen);
    seq_hdr->u.h262 = _seq_hdr;
    this_picture->u.h262 = _this_picture;
  }
  else
  {
    access_unit_p  this_unit = NULL;
    err = get_next_stripped_h264_frame(fcontext.u.h264,verbose,quiet,
                                       &this_unit,delta_pictures_seen);
    this_picture->u.h264 = this_unit;
  }
  return err;
}

static inline int get_next_filtered(filter_context  fcontext,
                                    int             verbose,
                                    int             quiet,
                                    picture        *seq_hdr,
                                    picture        *this_picture,
                                    int            *delta_pictures_seen)
{
  int  err;

  unset_picture(fcontext.is_h262,seq_hdr);
  unset_picture(fcontext.is_h262,this_picture);

  if (fcontext.is_h262)
  {
    h262_picture_p   _this_picture = NULL;
    h262_picture_p   _seq_hdr = NULL;
    err = get_next_filtered_h262_frame(fcontext.u.h262,verbose,quiet,&_seq_hdr,
                                       &_this_picture,delta_pictures_seen);
    seq_hdr->u.h262 = _seq_hdr;
    this_picture->u.h262 = _this_picture;
  }
  else
  {
    access_unit_p   this_unit = NULL;
    err = get_next_filtered_h264_frame(fcontext.u.h264,verbose,quiet,
                                       &this_unit,delta_pictures_seen);
    this_picture->u.h264 = this_unit;
  }
  return err;
}

// ============================================================
// A common view of handling the two types of data stream
// ============================================================
/*
 * Playing at normal speed happens as a "side effect" of gathering
 * information to allow us to reverse. Basically, each time a PES packet
 * is read in, it gets automatically written out for us, whilst we
 * analyse its contents.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int play_normal(stream_context  stream,
                       TS_writer_p     output,
                       int             verbose,
                       int             quiet,
                       int             num_normal,
                       int             tsdirect,
                       reverse_data_p  reverse_data)
{
  int  err;
  ES_p es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;

  if (extra_info) print_msg("Playing at normal speed\n");

  /* Do not write program data if we're in tsdirect mode -
   *  it'll change and some programs can't cope
   */
  if (!tsdirect)
    {
      err = write_program_data(reader,output);
      if (err) return err;
    }

  start_server_output(reader);

  if (stream.is_h262)
  {
    err = collect_reverse_h262(stream.u.h262,num_normal,verbose,quiet);
    if (err) return err;
  }
  else
  {
    err = collect_reverse_access_units(stream.u.h264,num_normal,verbose,quiet);
    if (err) return err;
  }
  return 0;
}

/*
 * Flush our PES packet after normal play
 *
 * Call this with server output still on.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int flush_after_normal(stream_context  stream,
                              TS_writer_p     output,
                              int             verbose,
                              int             quiet)
{
  int          err;
  ES_p         es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;
  ES_offset    item_start;

  if (extra_info)  print_msg("Flushing PES data after normal play\n");

  if (reader->packet == NULL)
  {
    // We're apparently at the end of file, so there's not much we can do
    return 0;
  }
  
  // When playing forwards at normal speed, each PES packet is read in,
  // processed to extract information, and then (automatically) written
  // out again when the next PES packet is read in.
  //
  // However, when we start doing fast forward or reverse, that automatic
  // output of PES packets is switched off. Thus it is up to us to ensure
  // that any outstanding data gets output before that.
  //
  // A command character is received when a write (of a PES packet) is made,
  // and such a write is (as said above) triggered when a new PES packet has
  // to be read in. *That* happens when reading the next ES item requires
  // reading a byte from the next PES packet. Thus we know that the current
  // picture started in the last PES packet, and that the ES item that
  // comes after it ends in the next packet.
  //
  // To terminate the data for that picture neatly in the output, we thus
  // need to output the ES data from this PES packet up to the end of the
  // current picture.

  if (stream.is_h262)
  {
    if (stream.u.h262->last_item == NULL)
    {
      if (extra_info) print_msg(".. no H.262 last item\n");
      return 0;  // not much else we can do
    }
    // The ES item that comes after (and thus marks the end of) the
    // last picture *starts* at:
    item_start = stream.u.h262->last_item->unit.start_posn;
  }
  else
  {
    if (stream.u.h264->pending_nal == NULL)
    {
      // We ended the previous access unit for some reason that didn't
      // need to read the next NAL unit, or we've not read anything in yet
      item_start = es->posn_of_next_byte;
    }
    else
    {
      // The previous access unit was ended by this "pending" NAL unit,
      // so the "next" item presumably starts with that...
      item_start = stream.u.h264->pending_nal->unit.start_posn;
    }
  }

  if (extra_info) fprint_msg(".. last item starts at " OFFSET_T_FORMAT "/%d\n",
                             item_start.infile,item_start.inpacket);

  // We know we haven't written out any data for the current PES packet
  // - do we need to?
  if (item_start.infile < reader->packet->posn)
  {
    // The terminating item started in the previous packet, which we've
    // already output. We should read in the next picture, and output
    // that part of it which hasn't already been output.
    picture  picture;
    if (extra_info) print_msg(".. which is in the previous packet - "
                              "reading spanning picture into next packet\n");
    
    err = get_next_picture(stream,verbose,quiet,&picture);
    if (err == EOF)
    {
      // Clearly there is no next picture
      if (extra_info) print_msg("End of file\n");
      return err;
    }
    else if (err)
    {
      print_err("### Error trying to read into next packet whilst"
                " flushing after normal play\n");
      return 1;
    }
    free_picture(&picture);
    // We now know that we want to output from the current PES packet to the
    // end of this picture, which is one byte before the new terminating
    // item
    if (stream.is_h262)
      item_start = stream.u.h262->last_item->unit.start_posn;
    else
    {
      if (stream.u.h264->pending_nal == NULL)
        item_start = es->posn_of_next_byte;
      else
        item_start = stream.u.h264->pending_nal->unit.start_posn;
    }
    if (extra_info) fprint_msg(".. new last item starts at " OFFSET_T_FORMAT
                               "/%d\n",item_start.infile,item_start.inpacket);
  }

  if (item_start.inpacket == 0)
  {
    // The terminating item started at the beginning of this packet,
    // so we don't have any outstanding data to output.
    if (extra_info) print_msg(".. so there's no need to output any of this"
                              " packet\n");
    return 0;
  }

  // We need to output whatever came before the terminating item
  if (extra_info) fprint_msg(".. so need to output %d bytes of this"
                             " packet\n",item_start.inpacket);

  err = write_ES_as_TS_PES_packet(output,
                                  reader->packet->es_data,
                                  item_start.inpacket,
                                  reader->output_video_pid,
                                  DEFAULT_VIDEO_STREAM_ID);
  if (err)
  {
    print_err("### Error flushing start of PES packet after normal play\n");
    return 1;
  }
  return 0;
}

/*
 * Output the next reference picture. If `I_only` then only output the next
 * I (or IDR) picture.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 */
static int output_next_reference_picture(stream_context  stream,
                                         TS_writer_p     output,
                                         int             verbose,
                                         int             quiet,
                                         int             I_only)
{
  int      err;
  picture  picture;

  if (extra_info)  print_msg(".. outputting next reference picture\n");
  
  for (;;)
  {
    err = get_next_picture(stream,verbose,quiet,&picture);
    if (err == EOF)
    {
      // Clearly there is no next picture - so we can't output it
      if (extra_info) print_msg("End of file\n");
      return err;
    }
    else if (err)
    {
      print_err("### Error trying to resynchronise after fast forward\n");
      return 1;
    }

    if (extra_info)
    {
      print_msg(".. read next picture: ");
      print_picture(picture);
      print_msg("\n");
    }

    if (is_non_frame(picture))
    {
      // A sequence header doesn't help us directly, but we can output
      // it as it will in practise be followed by an I picture
      // A sequence end will be followed by a sequence header, so we can
      // treat it similarly
      if (extra_info) print_msg(".. writing it out\n");
      err = write_picture_as_TS(stream,output,picture);
      if (err)
      {
        print_err("### Error writing out picture list\n");
        free_picture(&picture);
        return 1;
      }
    }
    else if (( I_only && is_I_or_IDR_picture(picture)) ||
             (!I_only && is_reference_picture(picture)))
    {
      if (extra_info) print_msg(".. picture acceptable\n");
      break;
    }
    free_picture(&picture);
  }
  // So we've got something sensible to continue with
  // - don't forget to write it out!
  if (extra_info) print_msg(".. writing it out\n");
  err = write_picture_as_TS(stream,output,picture);
  if (err)
  {
    print_err("### Error writing out picture list\n");
    free_picture(&picture);
    return 1;
  }
  free_picture(&picture);
  return 0;
}

/*
 * Resynchronise after reverse, ready for forwards playing (at whatever speed)
 *
 * Always call this immediately after reversing.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int resync_after_reverse(stream_context  stream,
                                TS_writer_p     output,
                                int             verbose,
                                int             quiet)
{
  int   err;
  ES_p  es = EXTRACT_ES_FROM_STREAM(stream);

  if (extra_info)  print_msg("\nResynchronising PES packets after reverse\n");

  // When reversing, data is read directly from the required locations
  // in the input file, without using the normal "get next picture"
  // mechanisms.
  //
  // When we *stop* reversing, we need to "pretend" to have read to the
  // (end of the) last picture output by the normal mechanisms

  // Undo any memory of previous pictures/context
  reset_stream(stream);

  if (extra_info)
    fprint_msg("   triple byte = %02x,%02x,%02x, next byte to be from "
               OFFSET_T_FORMAT "/%d\n",
               es->prev2_byte,es->prev1_byte,es->cur_byte,
               es->posn_of_next_byte.infile,
               es->posn_of_next_byte.inpacket);

  // @@@ The following is not true, methinks, as we've been outputting IDR
  //     and I frames (since there are not enough I frames, in hp-trail
  //     at least). On the other hand, there's not much we can do about it.
  // If we've just been reversing H.264 data, we know we've just output an
  // IDR, and we also know that IDRs act as "backstops" for B pictures - they
  // can't refer "through" them. Thus we don't need to worry about outputting
  // anything extra.

  // @@@ Even for H.264, it may be safer to output another reference picture,
  //     and it does help get the internal datastructures back in synch.
  //if (!stream.is_h262)
  //  return 0;

  // However, if it's H.262 data, we know we've just output a reference
  // picture (specifically, an I picture), but that we can't safely output
  // a B picture until we've output the *next* reference picture, since B
  // pictures need to refer "back" (in decoding order - back and forwards
  // in "play" order) to two reference pictures.
  err = output_next_reference_picture(stream,output,verbose,quiet,FALSE);
  if (err == EOF)
    return EOF;
  else if (err)
  {
    print_err("### Error outputting next reference picture,"
              " after reversing\n");
    return 1;
  }
  return 0;
}

/*
 * "Rewind" to the start of our stream, ready to start again from the
 * beginning.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int rewind_stream(stream_context  stream)
{
  if (extra_info)  print_msg("\nRewinding\n");
  if (stream.is_h262)
    return rewind_h262_context(stream.u.h262);
  else
    return rewind_access_unit_context(stream.u.h264);
}

/*
 * Resynchronise playing after fast fast forwarding. Always call this when
 * changing from fast forward back to normal speed playing. We want to
 * be in video only mode for this function.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int resync_after_filter(stream_context  stream,
                               TS_writer_p     output,
                               int             verbose,
                               int             quiet)
{
  int  err;

  if (extra_info)  print_msg("\nResynchronising after fast fast forward\n");
  
  // Fast forwarding with "filter" drops reference frames.
  // B pictures refer "back" (in decoding order) to two the last two
  // reference frames (although, in H.264 an IDR acts as a "stop" to this).
  //
  // If we've just been filtering, we know we just output *some* reference
  // picture, but we probably (almost certainly) hadn't output the preceding
  // reference picture.
  //
  // Specifically, typical data might be laid out as (using the output
  // of esdots, slightly massaged)::
  //
  //    [E>iE bE bE pE bE bE pE bE bE ... <next sequence>
  //
  // get_next_filtered_picture() returns us the "i" picture, and if we
  // then get the 'obvious' next data, we'll end up with a "b" picture,
  // which is not what we want. Thus we need to read forwards until
  // we reach the next "i" or "p" picture (in this case, it would be the
  // next "p" picture).

  // @@@ For H.264, it might perhaps make more sense to "reverse" to the
  // last IDR, output *that*, and then just continue playing. We know that
  // B pictures can't refer backwards "through" an IDR. This might also
  // *look* better when we've finished fast forwarding...

  // So...
  err = output_next_reference_picture(stream,output,verbose,quiet,FALSE);
  if (err == EOF)
    return EOF;
  else if (err)
  {
    print_err("### Error outputting next reference picture,"
              " after fast forwarding\n");
    return 1;
  }
  return 0;
}

/*
 * Resynchronise playing ready for normal playing again.
 *
 * Call this with server output off, but turn it on again immediately
 * after this call.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int back_to_normal(stream_context  stream,
                          TS_writer_p     output,
                          int             tsdirect)
{
  int          err;
  ES_p         es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;
  ES_offset    item_start;

  if (extra_info)  print_msg("\nResynchronising PES packets for normal play\n");


  if (reader->packet == NULL)
  {
    // We appear to have reached the end of file - there's not much
    // we can do about this, nor (probably) should we
    return 0;
  }
  
  if (!tsdirect)
    {
      // It can't hurt to reiterate the program data, and if we were just
      // playing a different program stream, it's a good idea
      err = write_program_data(reader,output);
      if (err) return err;
    }

  // When playing forwards at normal speed, each PES packet is read in,
  // processed to extract information, and then (automatically) written
  // out again when the next PES packet is read in.
  //
  // However, if we have been fast forwarding (at whatever speed), then we
  // will have been outputting only some pictures, and not outputting PES
  // packets automatically.
  //
  // In this case, we need to make it appear as if the (rest of the)
  // current PES packet had been output by the automatic mechanisms.
  //
  // In this context, "the rest of the PES packet" means all the data
  // (in this PES packet) from the start of the H.262 item that stopped us
  // reading the last picture
  //
  // However, if we have instead been reversing, then we do not have a last
  // item (since reversing just outputs uninterpreted chunks of data).  We do,
  // however, still know the first byte of the next piece of information after
  // that chunk of data, and that should be enough.
  if (stream.is_h262)
  {
    if (stream.u.h262->last_item == NULL)
    {
      if (extra_info) print_msg(".. no H.262 last item, presumably been"
                                " reversing\n");
      item_start = es->posn_of_next_byte;
      // In which case, we've already output the data for our "last" item
      // and only some of the following cases can occur...
    }
    else
    {
      // The ES item that comes after (and thus marks the end of) the last
      // picture *starts* at:
      item_start = stream.u.h262->last_item->unit.start_posn;
    }
  }
  else
  {
    if (stream.u.h264->pending_nal == NULL)
    {
      // Either we ended the previous access unit for some reason that
      // didn't need to read the next NAL unit, or we've been reversing
      // (or we just started and there was no previous access unit)
      item_start = es->posn_of_next_byte;
    }
    else
    {
      // The previous access unit was ended by this "pending" NAL unit,
      // so the "next" item presumably starts with that...
      item_start = stream.u.h264->pending_nal->unit.start_posn;
    }
  }
  
  if (extra_info)
  {
    fprint_msg(".. posn_of_next_byte is " OFFSET_T_FORMAT "/%d\n",
               es->posn_of_next_byte.infile,es->posn_of_next_byte.inpacket);

    if (stream.is_h262)
    {
      if (stream.u.h262->last_item)
      {
        fprint_msg("   last item starts at " OFFSET_T_FORMAT "/%d,\n",
                   stream.u.h262->last_item->unit.start_posn.infile,
                   stream.u.h262->last_item->unit.start_posn.inpacket);
        print_data(TRUE,"   last item",
                   stream.u.h262->last_item->unit.data,
                   stream.u.h262->last_item->unit.data_len,20);
      }
    }
    else
    {
      if (stream.u.h264->pending_nal)
      {
        fprint_msg("   last item starts at " OFFSET_T_FORMAT "/%d,\n",
                   stream.u.h264->pending_nal->unit.start_posn.infile,
                   stream.u.h264->pending_nal->unit.start_posn.inpacket);
        print_data(TRUE,"   pending NAL unit",
                   stream.u.h264->pending_nal->unit.data,
                   stream.u.h264->pending_nal->unit.data_len,20);
      }
    }
    fprint_msg(".. i.e., last item starts at " OFFSET_T_FORMAT "/%d\n",
               item_start.infile,item_start.inpacket);
    fprint_msg("   PES ES data length is %d\n"
               "   difference is %d\n",
               reader->packet->es_data_len,
               reader->packet->es_data_len-item_start.inpacket);
    fprint_msg("   reader->packet->posn is " OFFSET_T_FORMAT "\n",
               reader->packet->posn);
  }

  if (item_start.infile < reader->packet->posn)
  {
    // Said last item started in the previous PES packet
    // - we need to output the part of it that is in that previous packet
    // Given the next byte to be read (from this PES packet)
    int32_t curposn = es->posn_of_next_byte.inpacket;
    // we can work out how much of the item was in the previous packet
    // (sanity check - if the next byte to read was 1, then we've read one
    // byte from the current packet, and the following should indeed be right
    // - look at pes.c:read_PES_ES_byte and es.c:next_triple_byte for details)
    int32_t length_wanted;

    if (stream.is_h262)
    {
      length_wanted = stream.u.h262->last_item->unit.data_len - curposn;
      if (extra_info) fprint_msg(".. next byte is %d, so length wanted is %d"
                                 " - outputting it\n",curposn,length_wanted);
      err = write_ES_as_TS_PES_packet(output,
                                      stream.u.h262->last_item->unit.data,
                                      length_wanted,
                                      reader->output_video_pid,
                                      DEFAULT_VIDEO_STREAM_ID);
    }
    else
    {
      // @@@ For H.264, do we know, when we get here, that we always
      // have a pending NAL unit?
      length_wanted = stream.u.h264->pending_nal->unit.data_len - curposn;
      if (extra_info) fprint_msg(".. next byte is %d, so length wanted is %d"
                                 " - outputting it\n",curposn,length_wanted);
      err = write_ES_as_TS_PES_packet(output,
                                      stream.u.h264->pending_nal->unit.data,
                                      length_wanted,
                                      reader->output_video_pid,
                                      DEFAULT_VIDEO_STREAM_ID);
    }
    if (err)
    {
      print_err("### Error flushing (start of) last item after fast forward\n");
      return 1;
    }
    // That leaves us with the whole of this packet still to output,
    // and we can leave that to the automated mechanism next time it
    // reads in a new PES packet
  }
  else if (item_start.inpacket == 0)
  {
    // Said last item started at the start of this PES packet
    // so there's nothing to flush, and we can leave the automated
    // mechanism to sort out this packet, as above
    if (extra_info) print_msg(".. i.e., at start of packet, nothing to do\n");
  }
  else
  {
    // Said last item starts part way through this PES packet
    int32_t start_offset = item_start.inpacket;
    int32_t length_wanted = reader->packet->es_data_len - start_offset;
    if (extra_info)
    {
      fprint_msg(".. so output %d bytes at end of PES packet\n",length_wanted);
      print_data(TRUE,".. end bytes",&reader->packet->es_data[start_offset],
                 length_wanted,20);
    }

    err = write_ES_as_TS_PES_packet(output,
                                    &reader->packet->es_data[start_offset],
                                    length_wanted,
                                    reader->output_video_pid,
                                    DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      print_err("### Error flushing rest of PES packet after fast forward\n");
      return 1;
    }

    // That's all very well, but when the server restarts, and a call is made
    // to read (the next) PES packet in, it will attempt to write *this* PES
    // packet out again. So tell it not to do that...
    reader->dont_write_current_packet = TRUE;
  }
  return 0;
}

/*
 * Read PES packets and write them out to the target, fast forward.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int play_stripped(stream_context  stream,
                         filter_context  fcontext,
                         TS_writer_p     output,
                         int             verbose,
                         int             quiet,
                         int             tsdirect,
                         int             num_fast,
                         int             with_seq_hdrs)
{
  int  err;
  ES_p es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;
  int  total_pictures = 0;
  
//  stop_server_output(reader);

  // And then reset our filter context so that we start filtering without
  // remembering anything about last time we filtered
  reset_filter_context(fcontext,0);
  
  if (!tsdirect)
    {
      // Ensure we've got program data available (probably not necessary,
      // but unlikely to hurt)
      err = write_program_data(reader,output);
      if (err) return err;
    }

  if (extra_info) print_msg("Fast forwarding (strip)\n");

  for (;;)
  {
    picture  this_picture;
    picture  seq_hdr;        // H.262 only - *we* mustn't free this one
    int      delta_pictures_seen;

    if (tswrite_command_changed(output))
      return COMMAND_RETURN_CODE;

    err = get_next_stripped(fcontext,verbose,quiet,
                            &seq_hdr,&this_picture,&delta_pictures_seen);
    if (err == EOF || err == COMMAND_RETURN_CODE)
    {
      return err;
    }
    else if (err)
    {
      print_err("### Error getting next stripped picture\n");
      return 1;
    }
    if (with_seq_hdrs && !is_null_picture(seq_hdr))
    {
      err = write_picture_as_TS(stream,output,seq_hdr);
      if (err)
      {
        print_err("### Error writing out sequence header\n");
        free_picture(&this_picture);
        return 1;
      }
    }
    err = write_picture_as_TS(stream,output,this_picture);
    if (err)
    {
      print_err("### Error writing out picture list\n");
      free_picture(&this_picture);
      return 1;
    }
    free_picture(&this_picture);

    total_pictures += delta_pictures_seen;
    if (num_fast > 0 && total_pictures > num_fast)
      break;
  }
  return 0;
}

/*
 * Read PES packets and write them out to the target, fast fast forward.
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int play_filtered(stream_context  stream,
                         filter_context  fcontext,
                         TS_writer_p     output,
                         int             verbose,
                         int             quiet,
                         int             tsdirect,
                         int             num_faster,
                         int             frequency,
                         int             with_seq_hdrs)
{
  int  err;
  ES_p es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;

  picture  this_picture;
  picture  last_picture;
  picture  seq_hdr;  // H.262 only - *we* mustn't free this one

  int  total_pictures = 0;
  
//  stop_server_output(reader);
  // Reset our filter context so that we start filtering without remembering
  // anything about last time we filtered
  reset_filter_context(fcontext,frequency);
  
  if (!tsdirect)
    {
      // Ensure we've got program data available (probably not necessary,
      // but unlikely to hurt)
      err = write_program_data(reader,output);     
      if (err) return err;
    }

  if (extra_info) print_msg("Fast forwarding (filter)\n");

  unset_picture(stream.is_h262,&this_picture);
  unset_picture(stream.is_h262,&last_picture);
  for (;;)
  {
    int  delta_pictures_seen;
    if (tswrite_command_changed(output))
    {
      free_picture(&last_picture);
      err = COMMAND_RETURN_CODE;
      break;
    }
    
    err = get_next_filtered(fcontext,verbose,quiet,
                            &seq_hdr,&this_picture,&delta_pictures_seen);
    if (err == EOF || err == COMMAND_RETURN_CODE)
    {
      free_picture(&last_picture);
      break;
    }
    else if (err)
    {
      print_err("### Error getting next filtered picture\n");
      free_picture(&last_picture);
      return 1;
    }
    if (is_null_picture(this_picture))
    {
      // We need to repeat the last picture
      this_picture = last_picture;
      unset_picture(stream.is_h262,&last_picture);
    }
    if (!is_null_picture(this_picture))
    {
      if (with_seq_hdrs && !is_null_picture(seq_hdr))
      {
        err = write_picture_as_TS(stream,output,seq_hdr);
        if (err)
        {
          print_err("### Error writing out sequence header\n");
          free_picture(&this_picture);
          free_picture(&last_picture);
          return 1;
        }
      }
      err = write_picture_as_TS(stream,output,this_picture);
      if (err)
      {
        print_err("### Error writing out picture\n");
        free_picture(&this_picture);
        free_picture(&last_picture);
        return 1;
      }
    }
    free_picture(&last_picture);
    last_picture = this_picture;

    total_pictures += delta_pictures_seen;
    if (num_faster > 0 && total_pictures > num_faster)
      break;
  }

  // We *do* end up here if we run out of for loop...
  free_picture(&last_picture);

  if (err == EOF)
  {
    // If we reached the end of the file, then back up to the final
    // picture in the reversing arrays, and output that (so that the
    // user *sees* that final picture).
    //
    // (The last picture in the reversing arrays will be the last I or IDR
    // frame. We know that we are only outputting I or IDR frames, so we
    // know that this would also be the last frame we'd have considered
    // outputting. It's possible we've already output it, but on the whole
    // that shouldn't be terribly obvious to the user, I think.)
    reverse_data_p  reverse_data = EXTRACT_REVERSE_FROM_STREAM(stream);
    // Try going back 2 I/IDR pictures...
    err = output_from_reverse_data_as_TS(es,output,verbose,quiet,2,
                                         reverse_data);
    if (err && err != COMMAND_RETURN_CODE)
    {
      print_err("### Error outputting 'last' picture at EOF\n");
      return err;
    }
    // Which means we need to adjust back to normal playing *this* way
    err = resync_after_reverse(stream,output,verbose,quiet);
    if (err) return err;
    // Let the caller know what we did/where we are
    return EOF;
  }
  else
  {
    // Adjust back to normal playing
    err = resync_after_filter(stream,output,verbose,quiet);
    if (err) return err;
  }
  return 0;
}

/*
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * - `num_to_skip` is the number of frames to skip
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int skip_forwards(stream_context  stream,
                         TS_writer_p     output,
                         filter_context  fcontext,
                         int             with_seq_hdrs,
                         int             num_to_skip,
                         int             verbose,
                         int             quiet, 
                         int             tsdirect)
{
  int  err;
  ES_p es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;
  picture  this_picture;
  picture  seq_hdr;  // H.262 only - *we* mustn't free this one
  int      delta_pictures_seen;

#if TIME_SKIPPING
  time_t  start_time,end_time;
  clock_t start_clock,end_clock;
  start_time = time(NULL);
  start_clock = clock();
#endif

  // Reset our filter context so that we start filtering without remembering
  // anything about last time we filtered
  reset_filter_context(fcontext,num_to_skip);
  
  if (!tsdirect)
    {
      // Ensure we've got program data available (probably not necessary,
      // but unlikely to hurt)
      err = write_program_data(reader,output);
      if (err) return err;
    }

  if (extra_info) fprint_msg("Skipping forwards (%d frames)\n",num_to_skip);

  unset_picture(stream.is_h262,&this_picture);

  // Say that we don't want our skipping to be interrupted by the next command
  tswrite_set_command_atomic(output,TRUE);
    
  err = get_next_filtered(fcontext,verbose,quiet,
                          &seq_hdr,&this_picture,&delta_pictures_seen);
  if (err && err != EOF)
  {
    tswrite_set_command_atomic(output,FALSE);
    if (err == COMMAND_RETURN_CODE)
      return err;
    else
    {
      print_err("### Error skipping pictures\n");
      return 1;
    }
  }

  if (err == EOF)
  {
    // We hit the end of file before finding anything - so we should make
    // sure to display the "last" picture (actually, the last I/IDR picture)
    // Luckily, we can do that by "reversing" to it...
    reverse_data_p  reverse_data = EXTRACT_REVERSE_FROM_STREAM(stream);
    // Try going back 2 I/IDR pictures...
    err = output_from_reverse_data_as_TS(es,output,verbose,quiet,2,
                                         reverse_data);
    if (err)
    {
      print_err("### Error outputting 'last' picture at EOF\n");
      tswrite_set_command_atomic(output,FALSE);
      return err;
    }
    // Which means we need to adjust back to normal playing *this* way
    err = resync_after_reverse(stream,output,verbose,quiet);
    if (err)
    {
      tswrite_set_command_atomic(output,FALSE);
      return err;
    }
    // Let the caller know what we did/where we are
    return EOF;
  }
  else
  {
    // Since we're only skipping once, we shouldn't get a NULL (repeat)
    // picture back
    if (is_null_picture(this_picture))
    {
      print_err("### Skipping returned a NULL picture\n");
      free_picture(&this_picture);
      tswrite_set_command_atomic(output,FALSE);
      return 1;
    }
    if (with_seq_hdrs && !is_null_picture(seq_hdr))
    {
      err = write_picture_as_TS(stream,output,seq_hdr);
      if (err)
      {
        print_err("### Error writing out sequence header\n");
        free_picture(&this_picture);
        tswrite_set_command_atomic(output,FALSE);
        return 1;
      }
    }
    err = write_picture_as_TS(stream,output,this_picture);
    if (err)
    {
      print_err("### Error writing out picture\n");
      free_picture(&this_picture);
      tswrite_set_command_atomic(output,FALSE);
      return 1;
    }
    free_picture(&this_picture);

    // And remember to adjust back to normal playing
    err = resync_after_filter(stream,output,verbose,quiet);
    if (err)
    {
      tswrite_set_command_atomic(output,FALSE);
      return 1;
    }
  }

#if TIME_SKIPPING
  end_clock = clock();
  end_time = time(NULL);
  fprint_msg("Started  skipping at %s",ctime(&start_time));
  fprint_msg("Finished skipping at %s",ctime(&end_time));
  fprint_msg("Elapsed time %.3fs\n",difftime(end_time,start_time));
  fprint_msg("Process time %.3fs\n",
             ((double)(end_clock-start_clock)/CLOCKS_PER_SEC));
#endif

  // Remember to allow future commands to be interrupted
  tswrite_set_command_atomic(output,FALSE);
  return 0;
}

/*
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * - `num_to_skip` is the number of frames to skip
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int skip_backwards(stream_context  stream,
                          TS_writer_p     output,
                          int             num_to_skip,
                          int             verbose,
                          int             quiet,
                          int             tsdirect,
                          reverse_data_p  reverse_data)
{
  int  err;
  ES_p es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;
  
  if (!tsdirect)
    {
      // Ensure we've got program data available (probably not necessary,
      // but unlikely to hurt)
      err = write_program_data(reader,output);
      if (err) return err;
    }

  if (extra_info) fprint_msg("Skipping backwards (%d frames)\n",num_to_skip);

  // Say that we don't want our skipping to be interrupted by the next command
  tswrite_set_command_atomic(output,TRUE);

  err = output_in_reverse_as_TS(es,output,num_to_skip,verbose,quiet,
                                -1,num_to_skip,reverse_data);
  if (err && err != COMMAND_RETURN_CODE)
  {
    print_err("### Error skipping backwards\n");
    tswrite_set_command_atomic(output,FALSE);
    return err;
  }

  err = resync_after_reverse(stream,output,verbose,quiet);
  if (err)
  {
    tswrite_set_command_atomic(output,FALSE);
    return err;
  }
  
  // Remember to allow future commands to be interrupted
  tswrite_set_command_atomic(output,FALSE);
  return 0;
}

/*
 * Write pictures out to the target, in reverse
 *
 * Returns 0 if all went well, EOF if the end of file is reached,
 * otherwise 1 if an error occurred.
 *
 * If command input is enabled, then it can also return COMMAND_RETURN_CODE
 * if the current command has changed.
 */
static int play_reverse(stream_context   stream,
                        TS_writer_p      output,
                        int              verbose,
                        int              quiet,
                        int              tsdirect,
                        int              frequency,
                        int              num_reverse,
                        reverse_data_p   reverse_data)
{
  int  err;
  ES_p es = EXTRACT_ES_FROM_STREAM(stream);
  PES_reader_p reader = es->reader;

  if (extra_info) print_msg("Reversing\n");

  if (tsdirect)
    {
      // Ensure we've got program data available (probably not necessary,
      // but unlikely to hurt)
      err = write_program_data(reader,output);
      if (err) return err;
    }

#if SHOW_REVERSE_DATA
  if (extra_info)
  {
    int ii;
    for (ii=0; ii<reverse_data->length; ii++)
      if (stream.is_h262 && reverse_data->seq_offset[ii] == 0)
        fprint_msg("%3d: seqh at " OFFSET_T_FORMAT "/%d for %d\n",
                   ii,
                   reverse_data->start_file[ii],
                   reverse_data->start_pkt[ii],
                   reverse_data->data_len[ii]);
      else
        fprint_msg("%3d: %4d at " OFFSET_T_FORMAT "/%d for %d\n",
                   ii,reverse_data->index[ii],
                   reverse_data->start_file[ii],
                   reverse_data->start_pkt[ii],
                   reverse_data->data_len[ii]);
  }
#endif

  err = output_in_reverse_as_TS(es,output,frequency,verbose,quiet,
                                -1,num_reverse,reverse_data);
  if (err && err != COMMAND_RETURN_CODE)
  {
    print_err("### Error outputting reversed data\n");
    return err;
  }

  // Adjust back to normal playing
  err = resync_after_reverse(stream,output,verbose,quiet);
  if (err) return err;
  return err;
}

/*
 * Read PES packets and write them out to the target, obeying user
 * commands as to what to do.
 *
 * Returns 0 if all went well, EOF if the 'q'uit command has been given,
 * 1 if an error occurred.
 */
static int obey_command(char            this_command,
                        char            last_command,
                        int            *index,
                        int             started[MAX_INPUT_FILES],
                        PES_reader_p    reader[MAX_INPUT_FILES],
                        stream_context  stream[MAX_INPUT_FILES],
                        filter_context  fcontext[MAX_INPUT_FILES],
                        filter_context  scontext[MAX_INPUT_FILES],
                        reverse_data_p  reverse_data[MAX_INPUT_FILES],
                        TS_writer_p     tswriter,
                        int             video_only,
                        int             verbose,
                        int             quiet,
                        int             tsdirect,
                        int             with_seq_hdrs,
                        int             ffrequency,
                        int             rfrequency)
{
  int  err = 0;
  int  new_stream;
  int  which = *index;  // which stream we're reading
  
  // Loop obeying our given command and any "imaginary" commands that
  // result therefrom
  for (;;)
  {
#ifdef DEBUG_COMMANDS
    fprint_msg("__ obeying command '%c'\n",this_command);
#endif
    switch (this_command)
    {
    case COMMAND_NORMAL:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Forwards, normal speed\n",
                             tswriter->where.socket,which);
      if (last_command != COMMAND_NORMAL && started[which])
      {
        err = back_to_normal(stream[which],tswriter,tsdirect);
        if (err) return 1;
      }
      started[which] = TRUE;
      set_PES_reader_video_only(reader[which],video_only);
      err = play_normal(stream[which],tswriter,verbose,quiet,0,
                        tsdirect, reverse_data[which]);
      // If we've had a new command, and it's not 'n' again...
      if (err == COMMAND_RETURN_CODE && tswriter->command != COMMAND_NORMAL)
        err = flush_after_normal(stream[which],tswriter,verbose,quiet);
      break;

    case COMMAND_PAUSE:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Pause\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      err = wait_for_command(tswriter);
      break;

    case COMMAND_FAST:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Fast forwards\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = play_stripped(stream[which],scontext[which],tswriter,
                          verbose,quiet,tsdirect,0,with_seq_hdrs);
      break;

    case COMMAND_FAST_FAST:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Fast fast forwards\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = play_filtered(stream[which],fcontext[which],tswriter,
                          verbose,quiet,tsdirect,0,ffrequency,with_seq_hdrs);
      break;

    case COMMAND_REVERSE:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Reverse\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = play_reverse(stream[which],tswriter,verbose,quiet,
                         tsdirect,
                         rfrequency,0,reverse_data[which]);
      if (err == 0)
      {
        if (!quiet) fprint_msg("Start of file %d\n",which);
        this_command = COMMAND_PAUSE;
        break;
      }
      break;

    case COMMAND_FAST_REVERSE:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Reverse (faster)\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = play_reverse(stream[which],tswriter,verbose,quiet,
                         tsdirect,
                         2*rfrequency,0,reverse_data[which]);
      if (err == 0)
      {
        if (!quiet) fprint_msg("Start of file %d\n",which);
        this_command = COMMAND_PAUSE;
        break;
      }
      break;
      
    case COMMAND_SKIP_FORWARD:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Skip forwards 10 seconds\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = skip_forwards(stream[which],tswriter,
                          fcontext[which],with_seq_hdrs,
                          SMALL_SKIP_DISTANCE,verbose,quiet,tsdirect);
      this_command = COMMAND_NORMAL;  // aim to continue with normal play
      break;
      
    case COMMAND_SKIP_BACKWARD:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Skip backwards 10 seconds\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = skip_backwards(stream[which],tswriter,SMALL_SKIP_DISTANCE,
                           verbose,quiet,tsdirect,reverse_data[which]);
      this_command = COMMAND_NORMAL;  // aim to continue with normal play
      break;
      
    case COMMAND_SKIP_FORWARD_LOTS:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Skip forwards 3 minutes\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = skip_forwards(stream[which],tswriter,
                          fcontext[which],with_seq_hdrs,
                          BIG_SKIP_DISTANCE,verbose,quiet,tsdirect);
      this_command = COMMAND_NORMAL;  // aim to continue with normal play
      break;
      
    case COMMAND_SKIP_BACKWARD_LOTS:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Skip backwards 3 minutes\n",
                             tswriter->where.socket,which);
      stop_server_output(reader[which]);
      set_PES_reader_video_only(reader[which],TRUE);
      err = skip_backwards(stream[which],tswriter,BIG_SKIP_DISTANCE,
                           verbose,quiet,tsdirect,reverse_data[which]);
      this_command = COMMAND_NORMAL;  // aim to continue with normal play
      break;

    case COMMAND_SELECT_FILE_0:
      new_stream = 0;
      goto change_stream;

    case COMMAND_SELECT_FILE_1:
      new_stream = 1;
      goto change_stream;

    case COMMAND_SELECT_FILE_2:
      new_stream = 2;
      goto change_stream;

    case COMMAND_SELECT_FILE_3:
      new_stream = 3;
      goto change_stream;

    case COMMAND_SELECT_FILE_4:
      new_stream = 4;
      goto change_stream;

    case COMMAND_SELECT_FILE_5:
      new_stream = 5;
      goto change_stream;

    case COMMAND_SELECT_FILE_6:
      new_stream = 6;
      goto change_stream;

    case COMMAND_SELECT_FILE_7:
      new_stream = 7;
      goto change_stream;

    case COMMAND_SELECT_FILE_8:
      new_stream = 8;
      goto change_stream;

    case COMMAND_SELECT_FILE_9:
      new_stream = 9;
      goto change_stream;

    change_stream:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Select file\n",
                             tswriter->where.socket,new_stream);
      if (reader[new_stream] == NULL)
      {
        fprint_msg(".. No input file defined for stream %d - ignored\n",
                   new_stream);
      }
      else
      {
#if 0 // The following would only make sense if we *knew* we'd just been doing 'n'ormal play...
        // Try to ensure we finish at the end of a picture...
        err = flush_after_normal(stream[which],tswriter,verbose,quiet);
        if (err && err != COMMAND_RETURN_CODE && err != EOF)
          return err;
#endif
        // Pause the current stream
        stop_server_output(reader[which]);
        // Change to the new stream
        *index = which = new_stream;
        // @@@ For the moment, changing channel also means rewinding
        //     the "new" channel. This can become a "pure" channel change
        //     when we have the ability to "go back one(ish) reverse item(s)"
        //     and guarantee to end up at a sensible place to continue from
        //     (an IDR for H.264, or a GOP for H.262)
        // Rewind it
        err = rewind_stream(stream[which]);
        if (err) return 1;
        // And note that we *are* starting from the beginning again
        started[which] = FALSE;
      }
      // And return to normal playing
      this_command = COMMAND_NORMAL;
      break;
      
    case COMMAND_QUIT:
      if (!quiet) fprint_msg("****************************************\n"
                             "** [%3d] File %d: Quitting\n",
                             which,tswriter->where.socket);
      return EOF;
      
    default:
      fprint_err("!!! Command '%c' ignored\n",this_command);
      this_command = COMMAND_NORMAL;
      break;
    }

    // Work out what to do next
    switch (err)
    {
    case 0:
    case COMMAND_RETURN_CODE:
      break;
    case EOF:
      if (!quiet) fprint_msg("End of file %d\n",which);
      this_command = COMMAND_PAUSE;
      break;
    default:
      fprint_err("!!! Error playing file %d - pausing\n",which);
      this_command = COMMAND_PAUSE;
      break;
      // return 1;
    }
    if (tswriter->command_changed)
      return 0;
  }
}

/*
 * Read PES packets and write them out to the target, obeying user
 * commands as to what to do.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 */
static int play(int             default_index,
                PES_reader_p    reader[MAX_INPUT_FILES],
                stream_context  stream[MAX_INPUT_FILES],
                filter_context  fcontext[MAX_INPUT_FILES],
                filter_context  scontext[MAX_INPUT_FILES],
                reverse_data_p  reverse_data[MAX_INPUT_FILES],
                TS_writer_p     tswriter,
                int             video_only,
                int             verbose,
                int             quiet,
                int             tsdirect,
                int             with_seq_hdrs,
                int             ffrequency,
                int             rfrequency)
{
  int  err;
  int  ii;
  int  started[MAX_INPUT_FILES];
  int  which = default_index;  // which stream we're reading

  // Any function which writes to the output may read a new command character,
  // but only if tswriter->command_changed is FALSE. Such a function will then
  // return COMMAND_RETURN_CODE.
  // When a new command character is read, tswriter->command_changed is set to
  // TRUE. It is up to us to set it back to FALSE when we have finished
  // dealing with the new command letter.

  byte this_command = tswriter->command;
  byte last_command = COMMAND_NOT_A_COMMAND;
  
  for (ii=0; ii<MAX_INPUT_FILES; ii++)
    started[ii] = FALSE;
  
  // Select our current PES reader
  if (reader[which] == NULL)
  {
    fprint_err("### Default input stream %d has no associated file\n",
               which);
    return 1;
  }

  if (!quiet)
    fprint_msg("Starting with input stream %d\n",which);

#if 0  // Shouldn't need to do this, as any command that *does* anything will output it
  // Ensure we output program data before anything else "sensible"
  err = write_program_data(reader[which],tswriter);
  if (err) return err;
#endif

  for (;;)
  {
    // It is our job to let the underlying interface know that we are
    // ready to read a new command character (i.e., that we have "heard"
    // the last one). We do that by unsetting the command-changed flag.
    tswriter->command_changed = FALSE;

    this_command = tswriter->command;

#ifdef DEBUG_COMMANDS
    fprint_msg("xx Command is '%c', last command '%c'\n",
               this_command,last_command);
#endif

    err = obey_command(this_command,last_command,&which,
                       started,reader,stream,fcontext,scontext,reverse_data,
                       tswriter,video_only,verbose,quiet,tsdirect,with_seq_hdrs,
                       ffrequency,rfrequency);
    if (err == EOF)
      return 0;  // The user gave the 'q'uit command
    else if (err)
    {
      print_err("### Error terminated play\n");
      return 1;
    }
    last_command = this_command;
  }
}

/*
 * Read PES packets and write them out to the target, obeying user
 * commands as to what to do.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 */
static int play_pes_packets(PES_reader_p       reader[MAX_INPUT_FILES],
                            TS_writer_p        tswriter,
                            tsserve_context_p  context,
                            int                verbose,
                            int                quiet)
{
  int  err;
  int  ii;
  ES_p            es[MAX_INPUT_FILES]; // A view of our PES packets as ES units
  reverse_data_p  reverse_data[MAX_INPUT_FILES];
  stream_context  stream[MAX_INPUT_FILES];
  filter_context  fcontext[MAX_INPUT_FILES];
  filter_context  scontext[MAX_INPUT_FILES];

  if (!quiet)
    print_msg("\nSetting up environment\n");

  // Request that packets be written out to the TS writer as a "side effect" of
  // reading them in. The default is to write PES packets (just for the video
  // and audio data), but the alternative is to write all TS packets (if the
  // data *is* TS)
  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
  {
    if (reader[ii] != NULL)
    {
      set_server_output(reader[ii],tswriter,!context->tsdirect,
                        context->repeat_program_every);
      set_server_padding(reader[ii],context->pes_padding);
    }
  }

  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
  {
    es[ii] = NULL;
    reverse_data[ii] = NULL;

    // Closing uninitialised things is a bit dodgy if we don't indicate
    // what *type* of unset value is being used. However, in practice
    // it doesn't matter much, as both the H.262 and H.264 "destroy"
    // functions for streams and filter contexts sensibly do nothing
    // with a NULL value - so we might as well just say the same for all...
    stream[ii].is_h262 = fcontext[ii].is_h262 = scontext[ii].is_h262 = FALSE;
    stream[ii].u.h262  = NULL;
    fcontext[ii].u.h262  = scontext[ii].u.h262  = NULL;
  }

  // Start off our output with some null packets - this is in case the
  // reader needs some time to work out its byte alignment before it starts
  // looking for 0x47 bytes
  for (ii=0; ii<context->pad_start; ii++)
  {
    err = write_TS_null_packet(tswriter);
    if (err) return 1;
  }

  // And sort out our stack-of-streams atop each input file
  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
  {
    if (reader[ii] == NULL)
      continue;

    if (!quiet)
      fprint_msg("Setting up stream %d\n",ii);
    

    // Wrap our PES stream up as an ES stream
    // Note that this has the side-effect of reading the first packet
    // from the file (so that the ES reader can prime its 3-byte buffer).
    // This means that we will have read in the first PES packet, and
    // thus (for TS data) potentially quite a few TS packets, which
    // may also have included PAT/PMT. Luckily, we rely upon our caller
    // to have aleady set up PES or TS mirroring.
    err = build_elementary_stream_PES(reader[ii],&es[ii]);
    if (err)
    {
      fprint_err("### Error trying to build ES reader for PES reader %d\n",ii);
      goto tidy_up;
    }

    // Put an access unit or H.262 unit context around that
    err = build_stream(es[ii],!(reader[ii]->is_h264),ii+1,&stream[ii]);
    if (err)
    {
      fprint_err("### Unable to build input stream %d\n",ii);
      goto tidy_up;
    }

    // Build our reverse memory datastructure
    err = build_and_attach_reverse(stream[ii],&reverse_data[ii]);
    if (err)
    {
      fprint_err("### Unable to build reverse memory for stream %d\n",ii);
      goto tidy_up;
    }


    // Tell it what PID and stream id to use when outputting reversed data
    set_reverse_pid(reverse_data[ii],reader[ii]->output_video_pid,
                    DEFAULT_VIDEO_STREAM_ID);

    if (!context->with_seq_hdrs)
      reverse_data[ii]->output_sequence_headers = FALSE;

    // Build our fast forwards filter contexts
    err = build_filter_context(stream[ii],FALSE,context->ffrequency,&fcontext[ii]);
    if (err)
    {
      fprint_err("### Unable to build filter context for stream %d\n",ii);
      goto tidy_up;
    }


    err = build_filter_context(stream[ii],TRUE,0,&scontext[ii]);
    if (err)
    {
      fprint_err("### Unable to build strip context for stream %d\n",ii);
      goto tidy_up;
    }
  }

  // And, at last, do what we came for
  err = play(context->default_file_index,reader,stream,fcontext,scontext,reverse_data,
             tswriter,context->video_only,verbose,quiet,context->tsdirect,
             context->with_seq_hdrs,context->ffrequency,context->rfrequency);

tidy_up:
  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
  {
      close_elementary_stream(&es[ii]);
      free_reverse_data(&reverse_data[ii]);
      close_stream(stream[ii]);
      free_filter_context(fcontext[ii]);
      free_filter_context(scontext[ii]);
  }

  return err;
}

/*
 * Read PES packets and write them out to the target. Alternate normal
 * speed, fast forward and reverse (in some sequence).
 *
 * Returns 0 if all went well, 1 if an error occurred.
 */
static int test_play(PES_reader_p    reader,
                     stream_context  stream,
                     filter_context  fcontext,
                     filter_context  scontext,
                     reverse_data_p  reverse_data,
                     TS_writer_p     tswriter,
                     int             video_only,
                     int             verbose,
                     int             quiet,
                     int             tsdirect,
                     int             num_normal,
                     int             num_fast,
                     int             num_faster,
                     int             num_reverse,
                     int             ffrequency,
                     int             rfrequency,
                     int             with_seq_hdrs)
{
  int  err = 0;
  int  started = FALSE;
  int  ii;

  if (num_fast == 0 && num_faster == 0 && num_reverse == 0)
  {
    // Special case -- just play through
    print_msg(">> Just playing at normal speed\n");
    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,0,reverse_data);
    if (err == EOF)
      return 0;
    else
      return err;
  }

  print_msg(">> Going through sequence twice\n");

  for (ii=0; ii<2; ii++)
  {
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Normal speed for %d\n",num_normal);
    if (started)
    {
      err = back_to_normal(stream,tswriter,tsdirect);
      if (err) return 1;
    }
    started = TRUE;

    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    err = flush_after_normal(stream,tswriter,verbose,quiet);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    stop_server_output(reader);
    
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Fast forward for %d\n",num_fast);
    set_PES_reader_video_only(reader,TRUE);
    err = play_stripped(stream,scontext,tswriter,verbose,quiet,tsdirect,
                        num_fast,
                        with_seq_hdrs);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Normal speed for %d\n",num_normal);

    err = back_to_normal(stream,tswriter,tsdirect);
    if (err) return 1;

    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    err = flush_after_normal(stream,tswriter,verbose,quiet);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    stop_server_output(reader);
    
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Faster forward for %d\n",num_faster);
    set_PES_reader_video_only(reader,TRUE);
    err = play_filtered(stream,fcontext,tswriter,verbose,quiet,tsdirect,
                        num_faster,
                        ffrequency,with_seq_hdrs);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Normal speed for %d\n",num_normal);

    err = back_to_normal(stream,tswriter,tsdirect);
    if (err) return 1;

    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    err = flush_after_normal(stream,tswriter,verbose,quiet);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    stop_server_output(reader);
    
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Reverse for %d\n",num_reverse);
    set_PES_reader_video_only(reader,TRUE);
    err = play_reverse(stream,tswriter,verbose,quiet,rfrequency,
                       tsdirect,
                       num_reverse,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;
  }

  if (verbose || extra_info) print_msg("\n\n");
  if (err == EOF)
    print_msg("** End of file\n");
  else
    print_msg(">> End of sequences\n");
  return 0;
}

/*
 * Read PES packets and write them out to the target. Test skipping forwards
 * and back.
 *
 * Returns 0 if all went well, 1 if an error occurred.
 */
static int test_skip(PES_reader_p    reader,
                     stream_context  stream,
                     filter_context  fcontext,
                     filter_context  scontext,
                     reverse_data_p  reverse_data,
                     TS_writer_p     tswriter,
                     int             video_only,
                     int             verbose,
                     int             quiet,
                     int             tsdirect,
                     int             with_seq_hdrs)
{
  int  err = 0;
  int  num_normal = 100;
  int  started = FALSE;
  int  ii;
  
  print_msg(">> Going through sequence once\n");

  for (ii=0; ii<1; ii++)
  {
    fprint_msg("\n>> Iteration %d\n\n",ii);

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Normal speed for %d\n",num_normal);
    if (started)
    {
      err = back_to_normal(stream,tswriter,tsdirect);
      if (err) return 1;
    }
    started = TRUE;

    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    err = flush_after_normal(stream,tswriter,verbose,quiet);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    print_msg("** Skip forwards\n");
    stop_server_output(reader);
    set_PES_reader_video_only(reader,TRUE);
    err = skip_forwards(stream,tswriter,fcontext,with_seq_hdrs,
                        SMALL_SKIP_DISTANCE,verbose,quiet,tsdirect);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    print_msg("** Skip forwards\n");
    stop_server_output(reader);
    set_PES_reader_video_only(reader,TRUE);
    err = skip_forwards(stream,tswriter,fcontext,with_seq_hdrs,
                        SMALL_SKIP_DISTANCE,verbose,quiet,tsdirect);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Normal speed for %d\n",num_normal);

    err = back_to_normal(stream,tswriter,tsdirect);
    if (err) return 1;

    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    err = flush_after_normal(stream,tswriter,verbose,quiet);
    if (err == EOF)
      break;
    else if (err)
      return 1;
    
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    print_msg("** Skip backwards\n");
    stop_server_output(reader);
    set_PES_reader_video_only(reader,TRUE);
    err = skip_backwards(stream,tswriter,1,verbose,quiet,tsdirect,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;
    
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    print_msg("** Skip backwards\n");
    stop_server_output(reader);
    set_PES_reader_video_only(reader,TRUE);
    err = skip_backwards(stream,tswriter,1,verbose,quiet,tsdirect,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    fprint_msg("** Normal speed for %d\n",num_normal);

    err = back_to_normal(stream,tswriter,tsdirect);
    if (err) return 1;

    set_PES_reader_video_only(reader,video_only);
    err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    err = flush_after_normal(stream,tswriter,verbose,quiet);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    print_msg("** Skip forwards\n");
    stop_server_output(reader);
    set_PES_reader_video_only(reader,TRUE);
    err = skip_forwards(stream,tswriter,fcontext,with_seq_hdrs,
                        SMALL_SKIP_DISTANCE,verbose,quiet,tsdirect);
    if (err == EOF)
      break;
    else if (err)
      return 1;
    
    // ------------------------------------------------------------
    if (verbose || extra_info) print_msg("\n\n");
    print_msg("** Skip backwards\n");
    stop_server_output(reader);
    set_PES_reader_video_only(reader,TRUE);
    err = skip_backwards(stream,tswriter,1,verbose,quiet,tsdirect,reverse_data);
    if (err == EOF)
      break;
    else if (err)
      return 1;
  }

  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  num_normal = 100;
  if (verbose || extra_info) print_msg("\n\n");
  fprint_msg("** Normal speed for %d\n",num_normal);

  err = back_to_normal(stream,tswriter,tsdirect);
  if (err) return 1;

  set_PES_reader_video_only(reader,video_only);
  err = play_normal(stream,tswriter,verbose,quiet,tsdirect,num_normal,reverse_data);
  if (err == EOF)
  {
    print_msg("** End of file\n");
    return 0;
  }
  else if (err)
    return 1;

  err = flush_after_normal(stream,tswriter,verbose,quiet);
  if (err == EOF)
  {
    print_msg("** End of file\n");
    return 0;
  }
  else if (err)
    return 1;

  // ------------------------------------------------------------
  if (verbose || extra_info) print_msg("\n\n");
  if (err == EOF)
    print_msg("** End of file\n");
  else
    print_msg(">> End of sequences\n");
  return 0;
}

/*
 * Read PES packets and write them out to the target. Alternate normal
 * speed, fast forward and reverse (in some sequence).
 *
 * Returns 0 if all went well, 1 if an error occurred.
 */
static int test_play_pes_packets(PES_reader_p       reader,
                                 TS_writer_p        tswriter,
                                 tsserve_context_p  context,
                                 int                pad_start,
                                 int                video_only,
                                 int                verbose,
                                 int                quiet,
                                 int                tsdirect,
                                 int                num_normal,
                                 int                num_fast,
                                 int                num_faster,
                                 int                num_reverse,
                                 int                ffrequency,
                                 int                rfrequency,
                                 int                skiptest,
                                 int                with_seq_hdrs)
{
  int  err;
  int  ii;
  ES_p            es;  // A view of our PES packets as ES units
  reverse_data_p  reverse_data = NULL;
  stream_context  stream;
  filter_context  fcontext;
  filter_context  scontext;


  // Start off our output with some null packets - this is in case the
  // reader needs some time to work out its byte alignment before it starts
  // looking for 0x47 bytes
  for (ii=0; ii<pad_start; ii++)
  {
    err = write_TS_null_packet(tswriter);
    if (err) return 1;
  }

#if 0
  // Ensure we output program data before anything else "sensible"
  stream.program_number = 1;
  err = write_program_data(reader,tswriter);
  if (err) return err;
#endif

  // Request that PES packets be written out to the TS writer as
  // a "side effect" of reading them in
  set_server_output(reader,tswriter,!context->tsdirect,
                    context->repeat_program_every);
  set_server_padding(reader,context->pes_padding);

  // Wrap our PES stream up as an ES stream
  err = build_elementary_stream_PES(reader,&es);
  if (err)
  {
    print_err("### Error trying to build ES reader from PES reader\n");
    return 1;
  }

  // Build our reverse memory datastructure
  err = build_reverse_data(&reverse_data,reader->is_h264);
  if (err)
  {
    print_err("### Unable to build reverse memory\n");
    close_elementary_stream(&es);
    return 1;
  }

  stream.is_h262 = fcontext.is_h262 = scontext.is_h262 = !(reader->is_h264);

  if (reader->is_h264)
  {
    access_unit_context_p  acontext;  // Our ES data as access units
    h264_filter_context_p  fcontext4 = NULL;  // And a filter over that
    h264_filter_context_p  scontext4 = NULL;  // And another
    
    err = build_access_unit_context(es,&acontext);
    if (err)
    {
      print_err("### Error trying to build access unit reader from ES reader\n");
      close_elementary_stream(&es);
      free_reverse_data(&reverse_data);
      return 1;
    }
    add_access_unit_reverse_context(acontext,reverse_data);
  
    err = build_h264_filter_context(&fcontext4,acontext,ffrequency);
    if (err)
    {
      print_err("### Unable to build filter context\n");
      close_elementary_stream(&es);
      free_reverse_data(&reverse_data);
      free_access_unit_context(&acontext);
      return 1;
    }
  
    err = build_h264_filter_context_strip(&scontext4,acontext,TRUE);
    if (err)
    {
      print_err("### Unable to build strip context\n");
      close_elementary_stream(&es);
      free_reverse_data(&reverse_data);
      free_access_unit_context(&acontext);
      free_h264_filter_context(&fcontext4);
      return 1;
    }

    stream.u.h264 = acontext;
    fcontext.u.h264 = fcontext4;
    scontext.u.h264 = scontext4;

    if (skiptest)
      err = test_skip(reader,stream,fcontext,scontext,reverse_data,tswriter,
                      video_only,verbose,quiet,tsdirect,FALSE);
    else
      err = test_play(reader,stream,fcontext,scontext,reverse_data,tswriter,
                      video_only,verbose,quiet,tsdirect,
                      num_normal,num_fast,num_faster,num_reverse,
                      ffrequency,rfrequency,FALSE);

    free_access_unit_context(&acontext);
    free_h264_filter_context(&fcontext4);
    free_h264_filter_context(&scontext4);
  }
  else
  {
    h262_context_p    h262;  // Our ES data as H.262 items
    h262_filter_context_p  fcontext2 = NULL;  // And a filter over that
    h262_filter_context_p  scontext2 = NULL;  // And another

    if (!with_seq_hdrs)
      reverse_data->output_sequence_headers = FALSE;
    
    err = build_h262_context(es,&h262);
    if (err)
    {
      print_err("### Error trying to build H.262 reader from ES reader\n");
      close_elementary_stream(&es);
      free_reverse_data(&reverse_data);
      return 1;
    }
    add_h262_reverse_context(h262,reverse_data);
  
    err = build_h262_filter_context(&fcontext2,h262,ffrequency);
    if (err)
    {
      print_err("### Unable to build filter context\n");
      close_elementary_stream(&es);
      free_reverse_data(&reverse_data);
      free_h262_context(&h262);
      return 1;
    }
  
    err = build_h262_filter_context_strip(&scontext2,h262,TRUE);
    if (err)
    {
      print_err("### Unable to build strip context\n");
      close_elementary_stream(&es);
      free_reverse_data(&reverse_data);
      free_h262_context(&h262);
      free_h262_filter_context(&fcontext2);
      return 1;
    }

    stream.u.h262 = h262;
    fcontext.u.h262 = fcontext2;
    scontext.u.h262 = scontext2;

    if (skiptest)
      err = test_skip(reader,stream,fcontext,scontext,reverse_data,tswriter,
                      video_only,verbose,quiet,tsdirect,with_seq_hdrs);
    else
      err = test_play(reader,stream,fcontext,scontext,reverse_data,tswriter,
                      video_only,verbose,quiet,tsdirect,
                      num_normal,num_fast,num_faster,num_reverse,
                      ffrequency,rfrequency,with_seq_hdrs);

    free_h262_context(&h262);
    free_h262_filter_context(&fcontext2);
    free_h262_filter_context(&scontext2);
  }

  close_elementary_stream(&es);
  free_reverse_data(&reverse_data);

  return err;
}

static int open_input_file(tsserve_context_p context,
                           int               quiet,
                           int               verbose,
                           PES_reader_p     *reader)
{
  int err = open_PES_reader(context->input_names[context->default_file_index],
                            !quiet,verbose,reader);
  if (err)
  {
    fprint_err("### Error opening file %s\n",
               context->input_names[context->default_file_index]);
    return 1;
  }

  if (!quiet)
    fprint_msg("Opened input file %s (as %s)\n",
               context->input_names[context->default_file_index],
               ((*reader)->is_TS?"TS":"PS"));

  // If it's PS data, check if we're overriding its stream type
  if (!(*reader)->is_TS && context->force_stream_type &&
      (*reader)->is_h264 == context->want_h262)
  {
    if (!quiet)
      fprint_msg("File appeared to contain %s, forcing %s\n",
                 (*reader)->is_h264?"MPEG-4/AVC (H.264)":"MPEG-2 (H.272)",
                 context->want_h262?"MPEG-2":"MPEG-4/AVC");
    set_PES_reader_h264(*reader);
  }
  
  // If it's PS data, pretend to have read in a PAT and PMT
  if (!(*reader)->is_TS)
  {
    set_PES_reader_program_data(*reader,1,
                                context->pmt_pid,  context->video_pid,
                                context->audio_pid,context->pcr_pid);
    set_PES_reader_dolby_stream_type(*reader,context->dolby_is_dvb);
  }

  // If we're wanting extra information, also ask to be told about
  // the reading and writing of underlying PES packets.
  (*reader)->debug_read_packets = extra_info;
  return 0;
}

static int open_input_files(tsserve_context_p context,
                            int               quiet,
                            int               verbose,
                            PES_reader_p      reader[MAX_INPUT_FILES])
{
  int ii;
  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
  {
    int err;
    if (context->input_names[ii] == NULL)
    {
      reader[ii] = NULL;
      continue;
    }

    if (!quiet)
      fprint_msg("\nLooking at input file %d, %s\n",ii,context->input_names[ii]);

    err = open_PES_reader(context->input_names[ii],!quiet,verbose,&reader[ii]);
    if (err)
    {
      fprint_err("!!! Error opening file %d (%s)\n",
                 ii,context->input_names[ii]);
      // return 1;
      reader[ii] = NULL;
      continue;
    }

    if (!quiet)
      fprint_msg("Opened input file %2d, %s, as %s\n",ii,context->input_names[ii],
                 (reader[ii]->is_TS?"TS":"PS"));

    // If it's PS data, check if we're overriding its stream type
    // (for the moment, we only allow overriding of *all* files,
    // which is clumsy, but may be sufficient for our needs)
    if (!reader[ii]->is_TS && context->force_stream_type &&
        reader[ii]->is_h264 == context->want_h262)
    {
      if (!quiet)
        fprint_msg("File appeared to contain %s, forcing %s\n",
                   reader[ii]->is_h264?"MPEG-4/AVC (H.264)":"MPEG-2 (H.272)",
                   context->want_h262?"MPEG-2":"MPEG-4/AVC");
      set_PES_reader_h264(reader[ii]);
    }
  
    // Ensure that different input files get written out as different
    // programs (with differing PIDs)
    set_PES_reader_program_data(reader[ii],
                                ii+1,  // program number: 1 upwards
                                DEFAULT_VIDEO_PID+ii+20, // PMT
                                DEFAULT_VIDEO_PID+ii,    // video
                                DEFAULT_VIDEO_PID+ii+10, // audio
                                DEFAULT_VIDEO_PID+ii);   // PCR==video

    // If we're wanting extra information, also ask to be told about
    // the reading and writing of underlying PES packets.
    reader[ii]->debug_read_packets = extra_info;
  }
  return 0;
}

// ============================================================
// Serving multiple clients
// ============================================================
// Arguments for passing to the child server process
struct server_args
{
  tsserve_context_p  context;   // Various arguments we might need
  TS_writer_p        tswriter;  // Where we're writing to
  int                verbose;
  int                quiet;
};

static int tsserve_child_process(struct server_args *args)
{
  int  ii, err;
  int  had_err;
  tsserve_context_p  context = args->context;
  TS_writer_p        tswriter = args->tswriter;
  int                verbose = args->verbose;
  int                quiet = args->quiet;
  PES_reader_p       reader[MAX_INPUT_FILES];

  if (!quiet) fprint_msg("Establishing connection with client on socket %d\n",
                         tswriter->where.socket);
  
  err = tswrite_start_input(tswriter,tswriter->where.socket);
  if (err) 
  {
    print_err("### Unable to start command input from client\n");
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }

  err = open_input_files(context,quiet,verbose,reader);
  if (err)
  {
    print_err("### Unable to open input file\n");
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }

  // And play...
  if (!quiet) fprint_msg("Playing to client via socket %d\n",
                         tswriter->where.socket);

  err = play_pes_packets(reader,tswriter,context,verbose,quiet);
  if (err)
  {
    print_err("!!! Error playing PES packets to client\n");
    (void) tswrite_close(tswriter,TRUE);
    for (ii=0;ii<MAX_INPUT_FILES;ii++)
      (void) close_PES_reader(&reader[ii]);
    return 0;  // Treat as normal completion, so we continue
  }

  if (!quiet) print_msg("Finished talking to client\n");
  err = tswrite_close(tswriter,quiet);
  if (err)
  {
    for (ii=0;ii<MAX_INPUT_FILES;ii++)
      (void) close_PES_reader(&reader[ii]);
    return 1;
  }

  had_err = FALSE;
  for (ii=0;ii<MAX_INPUT_FILES;ii++)
  {
    err = close_PES_reader(&reader[ii]);
    if (err)
    {
      fprint_err("### Error closing input file %d, %s\n",ii,
                 context->input_names[ii]);
      had_err = TRUE;
    }
  }

#ifdef _WIN32
  // The original ("parent") thread does not know when we have finished,
  // so it cannot free the resources we are using. Of course, *we* know
  // we've now finished, so we can...
  free(args);
#endif

  return (had_err?1:0);
}

#ifdef _WIN32
// ============================================================
// Windows threading ("fork" alternative)
// ============================================================
/*
 * Wrapper for tsserve_child_process, used to coerce args, etc.
 */
static void child_thread_fn(void_p varg)
{
  struct server_args *args = (struct server_args *)varg;
  (void) tsserve_child_process(args);
}

/*
 * Start up the child thread, to serve a single client
 */
static int start_child(tsserve_context_p  context,
                       TS_writer_p        tswriter,
                       int                verbose,
                       int                quiet)
{
  HANDLE  child_thread;
  struct server_args *args;

  args = malloc(sizeof(struct server_args));
  if (args == NULL)
  {
    print_err("### Unable to allocate memory for child datastructure\n");
    return 1;
  }
  
  args->context = context;
  args->tswriter = tswriter;
  args->verbose = verbose;
  args->quiet = quiet;
  
  child_thread = (HANDLE) _beginthread(child_thread_fn,0,(void_p)args);
  if (child_thread == (HANDLE) -1)
  {
    fprint_err("Error creating child process: %s\n",strerror(errno));
    return 1;
  }
  return 0;
}
#else  // _WIN32
// ============================================================
// Unix forking ("thread" alternative)
// ============================================================
/*
 * Start up the child fork, to handle the circular buffering
 */
static int start_child(tsserve_context_p  context,
                       TS_writer_p        tswriter,
                       int                verbose,
                       int                quiet)
{
  pid_t pid;
  struct server_args args = {context,tswriter,verbose,quiet};    

  pid = fork();
  if (pid == -1)
  {
    fprint_err("Error forking: %s\n",strerror(errno));
    return 1;
  }
  else if (pid == 0)
  {
    // Aha - we're the child
    _exit(tsserve_child_process(&args));
  }
  tswriter->child = pid;
  return 0;
}

static void set_child_exit_handler();
/*
 * Signal handler - catch children and stop them becoming zombies
 */
static void on_child_exit()
{
#if 0
  print_msg("sighandler: starting\n");
#endif
  for (;;)
  {
    int status;
    int pid = waitpid(-1, &status, WNOHANG);
#if 0
    if (pid > 0)    
      fprint_msg("sighandler: finished with child %08x\n",pid);
    else
      fprint_msg("sighandler: finished with %d\n",pid);
#endif
    if (pid <= 0)
      break;
  }
}

/*
 * Setup the "on child exit" signal handler
 */
static void set_child_exit_handler()
{
  int ret;
  struct sigaction action;
  action.sa_handler = on_child_exit;
  action.sa_flags   = SA_NOCLDSTOP;  // we only want terminated children, not stopped children
#ifdef SA_RESTART
  action.sa_flags  |= SA_RESTART;
#endif
  sigemptyset(&action.sa_mask);
  // If it goes wrong, there's not much we can do apart from grumble...
#if 0
  print_msg("sighandler: Setting up signal handler to reap child processes\n");
#endif
  ret = sigaction(SIGCHLD,&action,0);
  if (ret < 0) print_err("!!! tsserve: Error starting signal handler to reap child processes\n");
}
#endif  // _WIN32

/*
 * Run as a server
 */
static int run_server(tsserve_context_p  context,
                      int                listen_port,
                      int                verbose,
                      int                quiet)
{
  int     err;
  SOCKET  server_socket;
  struct sockaddr_in ipaddr;

#ifdef _WIN32
  err = winsock_startup();
  if (err) return 1;
#else
  set_child_exit_handler();
#endif  
  
  // Create a socket.
  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket == -1)
  {
#ifdef _WIN32
    err = WSAGetLastError();
    print_err("### Unable to create socket: ");
    print_winsock_err(err);
    print_err("\n");
#else  // _WIN32      
    fprint_err("### Unable to create socket: %s\n",strerror(errno));
#endif // _WIN32
    return 1;
  }

  // Bind it to port `listen_port` on this machine
  memset(&ipaddr,0,sizeof(ipaddr));
#if !defined(__linux__) && !defined(_WIN32)
  // On BSD, the length is defined in the datastructure
  ipaddr.sin_len = sizeof(struct sockaddr_in);
#endif
  ipaddr.sin_family = AF_INET;
  ipaddr.sin_port = htons(listen_port);
  ipaddr.sin_addr.s_addr = INADDR_ANY;  // any interface

  err = bind(server_socket,(struct sockaddr*)&ipaddr,sizeof(ipaddr));
  if (err == -1)
  {
#ifdef _WIN32
    err = WSAGetLastError();
    fprint_err("### Unable to bind to port %d: ",listen_port);
    print_winsock_err(err);
    print_err("\n");
#else  // _WIN32      
    fprint_err("### Unable to bind to port %d: %s\n",
               listen_port,strerror(errno));
#endif // _WIN32
    return 1;
  }

  for (;;)
  {
    TS_writer_p  tswriter = NULL;

    if (!quiet) fprint_msg("\nListening for a connection on port %d"
                           " with socket %d\n",listen_port,server_socket);

#ifdef _WIN32
    // tswrite_close calls winsock_cleanup(), so we need to make sure that
    // we call an *extra* winsock_startup to match that (and leave the
    // call made before this loop "in scope")
    err = winsock_startup();
    if (err)
    {
      print_err("### Error calling winsock_startup before listening\n");
      return 1;
    }
#endif // _WIN32
    
    err = tswrite_wait_for_client(server_socket,quiet,&tswriter);
    if (err)
    {
      fprint_err("### Error listening for client on port %d\n",
                 listen_port);
      return 1;
    }

    if (context->drop_packets)
    {
      tswriter->drop_packets = context->drop_packets;
      tswriter->drop_number  = context->drop_number;
    }

    err = start_child(context,tswriter,verbose,quiet);
    if (err)
    {
      print_err("### Error spawning child server\n");
      return 1;
    }
#if 0 // The following was a temporary fix to stop zombies without a signal handler
#ifndef _WIN32
    // If we've forked, then we need to free our "copy" of the tswriter
    err = tswrite_close(tswriter,TRUE);
    if (err)
    {
      print_err("### Error closing socket in parent process\n");
      return 1;
    }
#endif
#endif
  }
  return 0;
}

/*
 * Run tests
 */
static int test_reader(tsserve_context_p  context,
                       int                output_to_file,
                       char              *output_name,
                       int                port,
                       int                num_normal,
                       int                num_fast,
                       int                num_faster,
                       int                num_reverse,
                       int                skiptest,
                       int                verbose,
                       int                quiet,
                       int                tsdirect)
{
  int  err;
  TS_writer_p   tswriter = NULL;
  PES_reader_p  reader = NULL;

  err = tswrite_open((output_to_file?TS_W_FILE:TS_W_TCP),
                     output_name,NULL,port,quiet,&tswriter);
  if (err)
  {
    fprint_err("### Unable to connect to %s\n",output_name);
    return 1;
  }

  if (context->drop_packets)
  {
    tswriter->drop_packets = context->drop_packets;
    tswriter->drop_number  = context->drop_number;
  }

  err = open_input_file(context,quiet,verbose,&reader);
  if (err)
  {
    print_err("### Unable to open input file\n");
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }

  // And play...
  err = test_play_pes_packets(reader,tswriter,context,
                              context->pad_start,context->video_only,
                              verbose,quiet,tsdirect,
                              num_normal,num_fast,num_faster,num_reverse,
                              context->ffrequency,context->rfrequency,
                              skiptest,context->with_seq_hdrs);
  if (err)
  {
    print_err("### Error playing PES packets\n");
    (void) tswrite_close(tswriter,TRUE);
    (void) close_PES_reader(&reader);
    return 1;
  }

  err = tswrite_close(tswriter,quiet);
  if (err)
  {
    fprint_err("### Error closing output %s: %s\n",output_name,
               strerror(errno));
    (void) close_PES_reader(&reader);
    return 1;
  }
  err = close_PES_reader(&reader);
  if (err)
  {
    fprint_err("### Error closing input file %s\n",
               context->input_names[context->default_file_index]);
    return 1;
  }
  return 0;
}

/*
 * Run as a player, possibly reading commands via a socket
 */
static int command_reader(tsserve_context_p  context,
                          char              *output_name,
                          int                port,
                          int                use_stdin,
                          int                verbose,
                          int                quiet)
{
  int  err;
  int  ii, had_err;
  TS_writer_p   tswriter = NULL;
  PES_reader_p  reader[MAX_INPUT_FILES];

  err = tswrite_open(TS_W_TCP,output_name,NULL,port,quiet,&tswriter);
  if (err)
  {
    fprint_err("### Unable to connect to %s\n",output_name);
    return 1;
  }

  if (context->drop_packets)
  {
    tswriter->drop_packets = context->drop_packets;
    tswriter->drop_number  = context->drop_number;
  }

#ifndef _WIN32
  // Maybe enable command input from stdin
  if (use_stdin)
  {
    if (!quiet)
      print_msg("Commands from standard input:\n"
                "   q    = quit\n"
                "   n    = normal speed\n"
                "   p    = pause (the initial state)\n"
                "   f    = fast forward\n"
                "   F    = fast fast forward\n"
                "   r    = reverse\n"
                "   R    = fast reverse\n"
                "   > <  = skip forwards, back by 10 seconds\n"
                "   ] [  = skip forwards, back by 3 minutes\n"
                "   0..9 = select file 0 through 9 (if defined),\n"
                "          rewind it and play at normal speed\n"
                "Use newline to 'send' a command or sequence of commands.\n");
    err= tswrite_start_input(tswriter,STDIN_FILENO);
    if (err) 
    {
      print_err("### Unable to start command input from stdin\n");
      (void) tswrite_close(tswriter,TRUE);
      return 1;
    }
  }
  else
#endif  // _WIN32
  {
    err= tswrite_start_input(tswriter,tswriter->where.socket);
    if (err) 
    {
      fprint_err("### Unable to start command input from %s\n",
                 output_name);
      (void) tswrite_close(tswriter,TRUE);
      return 1;
    }
  }

  err = open_input_files(context,quiet,verbose,reader);
  if (err)
  {
    print_err("### Unable to open input file\n");
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }

  // And play...
  err = play_pes_packets(reader,tswriter,context,verbose,quiet);
  if (err)
  {
    print_err("### Error playing PES packets\n");
    (void) tswrite_close(tswriter,TRUE);
    for (ii=0;ii<MAX_INPUT_FILES;ii++)
      (void) close_PES_reader(&reader[ii]);
    return 1;
  }

  err = tswrite_close(tswriter,quiet);
  if (err)
  {
    fprint_err("### Error closing output %s: %s\n",output_name,
               strerror(errno));
    for (ii=0;ii<MAX_INPUT_FILES;ii++)
      (void) close_PES_reader(&reader[ii]);
    return 1;
  }
  had_err = FALSE;
  for (ii=0;ii<MAX_INPUT_FILES;ii++)
  {
    err = close_PES_reader(&reader[ii]);
    if (err)
    {
      fprint_err("### Error closing input file %d, %s\n",ii,
                 context->input_names[ii]);
      had_err = TRUE;
    }
  }
  return (had_err?1:0);
}

static void print_usage()
{
  print_msg(
    "Usage:\n"
    "           tsserve <infile>\n"
    "           tsserve <infile> -port <n>\n"
    "           tsserve [switches] <infile> [switches]\n"
    "\n"
    );
  REPORT_VERSION("tsserve");
  print_msg(
    "\n"
    "  Act as a server which plays the given file (containing Transport\n"
    "  Stream or Program Stream data). The output is always Transport\n"
    "  Stream.\n"
    "\n"
    "Input:\n"
    "  <infile>          An H.222.0 TS or PS file to serve to the client.\n"
    "                    This will be treated as file 0 (see below).\n"
    "\n"
    "  -0 <file0> .. -9 <file9>\n"
    "                    Specify files 0 through 9, selectable with command\n"
    "                    characters 0 through 9. The lowest numbered file\n"
    "                    will be the default for display.\n"
    "\n"
    "General Switches:\n"
    "  -details          Print out more detailed help information,\n"
    "                    including some less common options.\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -quiet, -q        Suppress informational and warning messages.\n"
    "  -verbose, -v      Output additional diagnostic messages\n"
    "  -port <n>         Listen for a client on port <n> (default 88)\n"
    "  -noaudio          Ignore any audio data\n"
    "  -pad <n>          Pad the start of the output with <n> filler TS\n"
    "                    packets, to allow the client to synchronize with\n"
    "                    the datastream. Defaults to 8.\n"
    "\n"
    "  -noseqhdr         Do not output sequence headers for fast forward/reverse\n"
    "                    data. Only relevant to H.262 data.\n"
    "\n"
    "Program Stream Switches:\n"
    "\n"
    "  -prepeat <n>      Output the program data (PAT/PMT) after every <n>\n"
    "                    PS packs. Defaults to 100.\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    "  Both of these affect the stream type of the output data.\n"
    "\n"
    "  If the audio stream being output is Dolby (AC-3), then the stream type\n"
    "  used to output it differs for DVB (European) and ATSC (USA) data. It\n"
    "  may be specified as follows:\n"
    "\n"
    "  -dolby dvb       Use stream type 0x06 (the default)\n"
    "  -dolby atsc      Use stream type 0x81\n"
    "\n"
    "  For information on using the program in other modes, see -details.\n"
    );
}

static void print_detailed_usage()
{
  print_msg(
    "Usage: tsserve [switches] <infile>\n"
    "\n"
    "  Copyright (c) 2004 SJ Consulting Ltd.\n"
    "\n"
    "  Reads from a file containing H.222.0 (ISO/IEC 13818-1) Transport\n"
    "  Stream or Program Stream data (converting PS to TS as it goes),\n"
    "  and 'plays' the Transport Stream 'at' a client.\n"
    "\n"
    "  Assumes a single program in the file, and for PS assumes that the\n"
    "  program stream is well formed - i.e., that it starts with a pack\n"
    "  header. A PS stream that ends after a PES packet, but without an\n"
    "  MPEG_program_end_code will cause a warning message, but will not\n"
    "  be treated as an error.\n"
    "\n"
    "  In the default mode, the program acts as a server, listening for\n"
    "  clients on port 88 (or the port specified with -port). When a\n"
    "  client connects to the port, the program starts listening for\n"
    "  commands from the client, and acting appropriately. When the\n"
    "  client sends the 'q'uit command, the program disconnects from\n"
    "  the client, and listens for another.\n"
    "\n"
    "  Alternative modes may be specified with -cmd, -cmdstdin and\n"
    "  -test.\n"
    "\n"
    "Input:\n"
    "  <infile>          An H.222.0 TS or PS file.\n"
    "                    If given before any of -0..-9, this will be treated\n"
    "                    as a specification of file 0. If given after -0..-9,\n"
    "                    it will be treated as an error.\n"
    "\n"
    "  -0 <file0> .. -9 <file9>\n"
    "                    Specify files 0 through 9, selectable with command\n"
    "                    characters 0 through 9. The lowest numbered name\n"
    "                    will be selected as the default.\n"
    "\n"
    "General Switches:\n"
    "  -details          Present this text.\n"
    "  -quiet, -q        Only output error messages.\n"
    "  -verbose, -v      Output progress messages.\n"
    "\n"
    "  Normal operation outputs some messages summarising the command line\n"
    "  choices, information about data from the input file, confirmation\n"
    "  when the program is ending, etc.\n"
    "  Quiet operation endeavours only to output error messages.\n"
    "  Verbose operation outputs diagnostic information, not intended for\n"
    "  normal use.\n"
    "\n"
    "  -x                Output *extra* information."
    "\n"
    "  The extra information output gives more details about what the\n"
    "  server is doing in reaction to the commands given by the client.\n"
    "  It is intended as a diagnostic aid during development.\n"
    "\n"
    "  -port <n>         Listen for a client on port <n> (default 88)\n"
    "                    Ignored if -cmd, -cmdstdin or -test is\n"
    "                    specified\n"
    "\n"
    "  -noaudio          Don't output audio data\n"
    "\n"
    "  -pad <n>          Pad the start of the output with <n> filler TS\n"
    "                    packets, to allow the client to synchronize with\n"
    "                    the datastream. Defaults to 8.\n"
    "\n"
    "  -noseqhdr         Do not output sequence headers for fast forward/reverse\n"
    "                    data. Only relevant to H.262 data.\n"
    "\n"
    "Program Stream Switches:\n"
    "\n"
    "  The following switches are only applicable if the input data is PS.\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    "\n"
    "  If input is from a file, then the program will look at the start of\n"
    "  the file to determine if the stream is H.264 or H.262 data. This\n"
    "  process may occasionally come to the wrong conclusion, in which case\n"
    "  the user can override the choice using the switches above.\n"
    "\n"
    "  If the audio stream being output is Dolby (AC-3), then the stream type\n"
    "  used to output it differs for DVB (European) and ATSC (USA) data. It\n"
    "  may be specified as follows:\n"
    "\n"
    "  -dolby dvb       Use stream type 0x06 (the default)\n"
    "  -dolby atsc      Use stream type 0x81\n"
    "\n"
    "Transport Stream Switches:\n"
    "\n"
    "  The following switches are only applicable if the input data is TS.\n"
    "\n"
    "  -tsdirect         In normal play, copy all TS packets to the client,\n"
    "                    instead of just sending the PES packets for the video\n"
    "                    and audio streams'\n"
    "\n"
    "  Note that when -tsdirect is specified, PES packets are still inspected\n"
    "  to allow building up the fast forward/reverse indices.\n"
    "  Also, -prepeat, -pes_padding and -drop will have no effect with this switch.\n"
    "\n"
    "Other stuff:\n"
    "\n"
    "  -prepeat <n>      Output the program data (PAT/PMT) after every <n>\n"
    "                    PES packets, to allow a TS reader to resynchronise\n"
    "                    if it starts reading part way through the stream.\n"
    "                    PAT/PMT pairs are also output before 'significant'\n"
    "                    events (changing speed/direction/etc.).\n"
    "                    Defaults to 100.\n"
    "\n"
    "  -ffreq <n>        Frequency for faster forward ('F'). Default is 8.\n"
    "  -rfreq <n>        Frequency for reverse (fast reverse is twice\n"
    "                    the speed). Default is 8.\n"
    "\n"
    "  -pes_padding <n>  When outputting in 'normal play' mode, input PES packets\n" 
    "                    are copied to the output. If '-pes_padding' is used, then <n>\n"
    "                    dummy PES packets will be added to the output for each input\n"
    "                    packet, causing the amount of data output to be roughly <n>+1\n"
    "                    times as great. This can be useful for benchmarking the recipient.\n"
    "\n"
    "  -drop <k> <d>     As TS packets are output, for every <k>+<d> packets,\n"
    "                    keep <k> and then drop (throw away) <d>.\n"
    "                    Applies to all TS packets output, regardless of selected file.\n"
    "                    This can be useful when testing other applications.\n"
    "\n"
    "Alternate modes\n"
    "---------------\n"
    "  Command input and testing modes connect directly to a host, and thus\n"
    "  the host to use must be specified.\n"
    "\n"
    "  -host <host>[:<port>\n"
    "                    The host to which to write TS packets, over\n"
    "                    TCP/IP. If <port> is not specified, it defaults\n"
    "                    to 88.\n"
    "\n"
    "Command input:\n"
    "  -cmd              Enables command input, from the host.\n"
    "  -cmdstdin         Enables command input, from standard input.\n"
    "                    This is not supported on Windows.\n"
    "\n"
    "  In command input mode, the program connects to the host specified\n"
    "  with -host, and takes commands either from the host, or from\n"
    "  standard input.\n"
    "\n"
    "  Command characters are:\n"
    "      q        quit.\n"
    "      n        normal play.\n"
    "      p        pause (the startup state).\n"
    "      f        fast forward (uses 'strip').\n"
    "      F        fast fast forward (uses 'filter').\n"
    "      r        reverse.\n"
    "      R        fast reverse.\n"
    "      >  <     skip forwards/back by 10 seconds.\n"
    "      ]  [     skip forwards/back by 3 minutes.\n"
    "      0..9     select file 0 through 9 (as defined by switches -0 to\n"
    "               -9, see above), rewind it and play at normal speed.\n"
    "  Any other character is ignored.\n"
    "  Note that if command input is from standard input, a newline must\n"
    "  be typed before command characters are 'seen', and if there are\n"
    "  multiple characters on a line, they will be obeyed in sequence.\n"
    "\n"
    "Testing:\n"
    "  -test             Test by running a sequence of pictures at the\n"
    "                    specified host. The exact sequence used can be\n"
    "                    determined with the -f, etc., switches:\n"
    "\n"
    "  -f <nf>           Loop outputting <nn> pictures at normal speed,\n"
    "  -n <nn>           then fast forward past <nf> pictures, then <nn> at\n"
    "  -ff <nF>          normal speed, then <nF> at the higher fast forward\n"
    "  -r <nr>           speed, then <nn> at normal speed again, then\n"
    "                    reverse past <nr>. Repeat until stopped.\n"
    "\n"
    "  If '-f 0 -ff 0 -r 0' is specified, then the data will just play at\n"
    "  normal speed, ignoring -n.\n"
    "\n"
    "  -skiptest         Test forwards and backwards skipping.\n"
    "\n"
    "  -output <name>, -o <name>\n"
    "                    If -test is being used then output may be\n"
    "                    redirected to a file, instead of a host.\n"
    );
}

int main(int argc, char **argv)
{
  char        *output_name = NULL;
  int          had_input_name = FALSE;
  int          had_output_name = FALSE;
  int          output_port = 88; // Useful default port number
  int          quiet = FALSE;
  int          verbose = FALSE;
  int          use_stdin = FALSE;  // for command input...
  int          listen_port = 88;

  enum ACTION  action = ACTION_SERVER;

  int  err = 0;
  int  ii;
  int  argno = 1;

  // Testing specific options
  int  num_normal = 100;
  int  num_fast = 100;
  int  num_faster = 100;
  int  num_reverse = 100;
  int  output_to_file = FALSE;
  int  skiptest = FALSE;

  struct tsserve_context context;

  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
    context.input_names[ii] = NULL;
  
  context.video_only = FALSE;
  context.pad_start = 8;
  context.ffrequency = DEFAULT_FORWARD_FREQUENCY;
  context.rfrequency = DEFAULT_REVERSE_FREQUENCY;
  context.with_seq_hdrs = TRUE;
  context.pes_padding = 0;
  context.drop_packets = 0;
  context.drop_number = 0;
  
  // Program Stream specific options
  context.pmt_pid    = 0x66;
  context.audio_pid  = 0x67;
  context.video_pid  = 0x68;
  context.pcr_pid    = context.video_pid; // Use PCRs from the video stream
  context.repeat_program_every = 100;

  // Transport Stream specific options
  context.tsdirect = FALSE;     // Write to server as a side effect of PES reading

  context.force_stream_type = FALSE;
  context.want_h262 = TRUE; // shouldn't matter
  context.dolby_is_dvb = TRUE;

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (argno < argc)
  {
    if (argv[argno][0] == '-')
    {
      if (!strcmp("--help",argv[argno]) || !strcmp("-h",argv[argno]) ||
          !strcmp("-help",argv[argno]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("-err",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        if (!strcmp(argv[argno+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[argno+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### tsserve: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[argno+1]);
          return 1;
        }
        argno++;
      }
      else if (!strcmp("-details",argv[argno]))
      {
        print_detailed_usage();
        return 0;
      }
      else if (!strcmp("-noseqhdr",argv[argno]) ||
               !strcmp("-noseqhdrs",argv[argno]))
      {
        context.with_seq_hdrs = FALSE;
      }
      else if (!strcmp("-skiptest",argv[argno]))
      {
        action = ACTION_TEST;
        skiptest = TRUE;
      }
      else if (!strcmp("-test",argv[argno]))
      {
        action = ACTION_TEST;
        skiptest = FALSE;
      }
      else if (!strcmp("-tsdirect",argv[argno]))
      {
        context.tsdirect = TRUE; // Write to server as a side effect of TS reading
      }
      else if (!strcmp("-n",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,
                        &num_normal);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-f",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,&num_fast);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-ff",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,
                        &num_faster);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-r",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,
                        &num_reverse);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-ffreq",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,0,
                        &context.ffrequency);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-rfreq",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,0,
                        &context.rfrequency);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-pes_padding",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,
                        &context.pes_padding);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-drop",argv[argno]))
      {
        if (ii+2 >= argc)
        {
          print_err("### tsserve: -drop requires two arguments\n");
          return 1;
        }
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,0,
                        &context.drop_packets);
        if (err) return 1;
        err = int_value("tsserve",argv[argno],argv[argno+2],TRUE,0,
                        &context.drop_number);
        if (err) return 1;
        argno += 2;
      }
      else if (!strcmp("-quiet",argv[argno]) || !strcmp("-q",argv[argno]))
      {
        quiet = TRUE;
        verbose = FALSE;
      }
      else if (!strcmp("-verbose",argv[argno]) || !strcmp("-v",argv[argno]))
      {
        quiet = FALSE;
        verbose = TRUE;
      }
      else if (!strcmp("-x",argv[argno]))
      {
        extra_info = TRUE;
      }
      else if (!strcmp("-noaudio",argv[argno]))
      {
        context.video_only = TRUE;
      }
      else if (!strcmp("-avc",argv[argno]) || !strcmp("-h264",argv[argno]))
      {
        context.force_stream_type = TRUE;
        context.want_h262 = FALSE;
      }
      else if (!strcmp("-h262",argv[argno]))
      {
        context.force_stream_type = TRUE;
        context.want_h262 = TRUE;
      }
      else if (!strcmp("-dolby",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        if (!strcmp("dvb",argv[argno+1]))
          context.dolby_is_dvb = TRUE;
        else if (!strcmp("atsc",argv[argno+1]))
          context.dolby_is_dvb = FALSE;
        else
        {
          print_err("### tsserve: -dolby must be followed by dvb or atsc\n");
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-prepeat",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,
                        &context.repeat_program_every);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-pad",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,10,
                        &context.pad_start);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-port",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = int_value("tsserve",argv[argno],argv[argno+1],TRUE,0,
                        &listen_port);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-cmd",argv[argno]))
      {
        action = ACTION_CMD;
      }
      else if (!strcmp("-cmdstdin",argv[argno]))
      {
        use_stdin = TRUE;
        action = ACTION_CMD;
      }
      else if (!strcmp("-host",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        err = host_value("tsserve",argv[argno],argv[argno+1],
                         &output_name,&output_port);
        if (err) return 1;
        had_output_name = TRUE; // more or less
        argno++;
      }
      else if (!strcmp("-0",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[0] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-1",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[1] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-2",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[2] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-3",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[3] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-4",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[4] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-5",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[5] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-6",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[6] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-7",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[7] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-8",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[8] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-9",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        had_input_name = TRUE;
        context.input_names[9] = argv[argno+1];
        argno++;
      }
      else if (!strcmp("-output",argv[argno]) || !strcmp("-o",argv[argno]))
      {
        CHECKARG("tsserve",argno);
        output_to_file = TRUE;
        had_output_name = TRUE;
        output_name = argv[argno+1];
        argno++;
      }
      else
      {
        fprint_err("### tsserve: "
                   "Unrecognised command line switch '%s'\n",argv[argno]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### tsserve: Unexpected '%s'\n",argv[argno]);
        return 1;
      }
      else
      {
        context.input_names[0] = argv[argno];
        had_input_name = TRUE;
      }
    }
    argno++;
  }

  if (!had_input_name)
  {
    print_err("### tsserve: No input file specified\n");
    return 1;
  }
  if (!had_output_name && action != ACTION_SERVER)
  {
    print_err("### tsserve: No output specified\n");
    return 1;
  }
  if (output_to_file && action != ACTION_TEST)
  {
    print_err("### tsserve: Output to a file (-output) is only allowed"
              " with -test\n");
    return 1;
  }

  if (!quiet)
  {
    print_msg("Input files:\n");
    for (ii = 0; ii < MAX_INPUT_FILES; ii++)
    {
      if (context.input_names[ii] != NULL)
        fprint_msg("   %2d: %s\n",ii,context.input_names[ii]);
    }
  }

  for (ii = 0; ii < MAX_INPUT_FILES; ii++)
  {
    if (context.input_names[ii] != NULL)
    {
      context.default_file_index = ii;
      if (!quiet) fprint_msg("File %d (%s) selected as default\n",
                             ii,context.input_names[ii]);
      break;
    }
  }

  if (context.tsdirect && !quiet)
    print_msg("Serving all TS packets, not just video/audio streams\n");

  if (context.drop_packets && !quiet)
    fprint_msg("DROPPING: Keeping %d TS packet%s, then dropping (throwing away) %d\n",
               context.drop_packets,(context.drop_packets==1?"":"s"),
           context.drop_number);
  
  switch (action)
  {
  case ACTION_SERVER:
    err = run_server(&context,listen_port,verbose,quiet);
    if (err)
    {
      print_err("### tsserve: Error in server\n");
      return 1;
    }
    break;

  case ACTION_TEST:
    err = test_reader(&context,output_to_file,output_name,output_port,
                      num_normal,num_fast,num_faster,num_reverse,skiptest,
                      verbose,quiet,context.tsdirect);
    if (err)
    {
      fprint_err("### tsserve: Error playing to %s\n",output_name);
      return 1;
    }
    break;

  case ACTION_CMD:
    err = command_reader(&context,output_name,output_port,
                         use_stdin,verbose,quiet);
    if (err)
    {
      fprint_err("### tsserve: Error playing to %s\n",output_name);
      return 1;
    }
    break;

  default:
    print_err("### No action specified\n");
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

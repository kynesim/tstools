/*
 * Report on the contents of an H.264 (MPEG-4/AVC) or H.262 (MPEG-2)
 * elementary stream, as a sequence of single characters, representing
 * appropriate entities.
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
#include <fcntl.h>
#include <string.h>

#ifdef _WIN32
#include <stddef.h>
#else  // _WIN32
#include <unistd.h>
#endif // _WIN32


#include "compat.h"
#include "es_fns.h"
#include "pes_fns.h"
#include "accessunit_fns.h"
#include "h262_fns.h"
#include "avs_fns.h"
#include "printing_fns.h"
#include "misc_fns.h"
#include "version.h"

double   frame_rate = 25.0; // default frame rate. this can be modified using the switch "-fr"

static inline
int umod(unsigned int a, unsigned int b)
{
	int r = a % b;
	return r < 0 ? r + b : r;
}


/*
 * Print out a single character representative of our item.
 */
static int h262_item_dot(h262_item_p  item, 
				          double *delta_gop, 
				          int    show_gop_time)
{
  char *str = NULL;

  static int frames = 0;
  static int temp_frames = 0;
  int pic_coding_type = 0;

  // print the time every time we find a random access point (time between two GOPs)
  if (item->unit.start_code == 0xB3)
  {
    *delta_gop = (frames - temp_frames)/frame_rate; // time between two GOPs [in seconds]
    temp_frames = frames;
    if (show_gop_time && temp_frames)
      fprint_msg(": %2.4fs\n", *delta_gop);
  }

  if (item->unit.start_code == 0x00)
  {
    if (frames % ((int)frame_rate*60) == 0) 
      fprint_msg("\n %d minute%s\n",frames/(int)(frame_rate*60),
                 (frames/(int)(frame_rate*60)==1?"":"s")); 
    frames++;
  }

  switch (item->unit.start_code)
  {
  case 0x00:
    str = (item->picture_coding_type==1?"i":
           item->picture_coding_type==2?"p":
           item->picture_coding_type==3?"b":
           item->picture_coding_type==4?"d":"x");
    pic_coding_type = item->picture_coding_type;
    break;
  case 0xB0: str = "R"; break; // Reserved
  case 0xB1: str = "R"; break; // Reserved
  case 0xB2: str = "U"; break; // User data
  case 0xB3: str = "["; break; // SEQUENCE HEADER
  case 0xB4: str = "X"; break; // Sequence error
  case 0xB5: str = "E"; break; // Extension start
  case 0xB6: str = "R"; break; // Reserved
  case 0xB7: str = "]"; break; // SEQUENCE END
  case 0xB8: str = ">"; break; // Group start

  default:
    if (str == NULL)
    {
      if (item->unit.start_code >= 0x01 && item->unit.start_code <= 0xAF)
        return 0; //str = "."; // Don't report slice data explicitly
      else
        str = "?";
    }
    break;
  }
  print_msg(str);
  fflush(stdout);
  return pic_coding_type;
}

/*
 * Simply report on the content of an MPEG2 file as single characters
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int report_h262_file_as_dots(ES_p    es,
                                    int     max,
                                    int     verbose,
				    int	    show_gop_time)
{
  int  err;
  int  count = 0;
  double time_gop = 0.0;
  int gops = 0;
  double time_gop_max = 0.0;
  double time_gop_min = 1000.0;
  double time_gop_tot = 0.0;
  int pic_coding_type;
  unsigned long num_i = 0; // number of I frames
  unsigned long num_p = 0; // number of P frames
  unsigned long num_b = 0; // number of B frames

  if (verbose)
    print_msg("\n"
              "Each character represents a single H.262 item\n"
              "Pictures are represented according to their picture coding\n"
              "type, and the slices within a picture are not shown.\n"
              "    i means an I picture\n"
              "    p means a  P picture\n"
              "    b means a  B picture\n"
              "    d means a  D picture (these should not occur in MPEG-2)\n"
              "    x means some other picture (such should not occur)\n"
              "Other items are represented as follows:\n"
              "    [ means a  Sequence header\n"
              "    > means a  Group Start header\n"
              "    E means an Extension start header\n"
              "    U means a  User data header\n"
              "    X means a  Sequence Error\n"
              "    ] means a  Sequence End\n"
              "    R means a  Reserved item\n"
              "    ? means something else. This may indicate that the stream\n"
              "      is not an ES representing H.262 (it might, for instance\n"
              "      be PES)\n"
              "\n");
  
  for (;;)
  {
    h262_item_p  item; 
    err = find_next_h262_item(es,&item);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error copying NAL units\n");
      return err;
    }
    count++;
    pic_coding_type = h262_item_dot(item, &time_gop, show_gop_time);
    switch (pic_coding_type) {
      case 1: num_i++; break;
      case 2: num_p++; break;
      case 3: num_b++; break;
      default: break;
    }
    

    if(item->unit.start_code == 0xB3)
    {
      time_gop_max = max(time_gop_max, time_gop);
      if (gops) 
        time_gop_min = min(time_gop_min, time_gop);
      gops++;
      time_gop_tot += time_gop;
    }

    free_h262_item(&item);

    if (max > 0 && count >= max)
      break;
  }
  fprint_msg("\nFound %d MPEG2 item%s\n",count,(count==1?"":"s"));
  fprint_msg("%lu I, %lu P, %lu B\n",num_i,num_p,num_b);
  fprint_msg("GOP times (s): max=%2.4f, min=%2.4f, mean=%2.6f (frame rate = %2.2f)\n",time_gop_max,
         time_gop_min,time_gop_tot/(gops-1), frame_rate);
  return 0;
}

/*
 * Simply report on the content of an AVS file as single characters
 *
 * - `es` is the input elementary stream
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int report_avs_file_as_dots(ES_p    es,
                                   int     max,
                                   int     verbose)
{
  int            err = 0;
  int            count = 0;
  int            frames = 0;
  //double         frame_rate = 25.0;      // as a guess
  avs_context_p  context;

  if (verbose)
    print_msg("\n"
              "Each character represents a single AVS item\n"
              "Frames are represented according to their picture coding\n"
              "type, and the slices within a frame are not shown.\n"
              "    i means an I frame\n"
              "    p means a  P frame\n"
              "    b means a  B frame\n"
              "    _ means a (stray) slice, normally only at the start of a stream\n"
              "    ! means something else (this should not be possible)\n"
              "Other items are represented as follows:\n"
              "    [ means a  Sequence header\n"
              "    E means an Extension start header\n"
              "    U means a  User data header\n"
              "    ] means a  Sequence End\n"
              "    V means a  Video edit item\n"
              "    ? means something else. This may indicate that the stream\n"
              "      is not an ES representing AVS (it might, for instance\n"
              "      be PES)\n"
              "\n");
  
  err = build_avs_context(es,&context);
  if (err) return err;
    
  for (;;)
  {
    avs_frame_p      avs_frame;

    err = get_next_avs_frame(context,TRUE,FALSE,&avs_frame);
    if (err == EOF)
      break;
    else if (err)
    {
      free_avs_context(&context);
      return 1;
    }

    if (avs_frame->is_frame)
    {
      frames ++;
      if (avs_frame->picture_coding_type == AVS_I_PICTURE_CODING)
        print_msg("i");
      else if (avs_frame->picture_coding_type == AVS_P_PICTURE_CODING)
        print_msg("p");
      else if (avs_frame->picture_coding_type == AVS_B_PICTURE_CODING)
        print_msg("b");
      else
        print_msg("!");
      // Give a *rough* guide as to timing -- assume a constant frame rate
      if (frames % (int)(frame_rate*60) == 0)
        fprint_msg("\n%d minute%s\n",frames/(25*60),(frames/(25*60)==1?"":"s"));
    }
    else if (avs_frame->start_code < 0xB0)
      print_msg("_");                      // slice -- shouldn't happen
    else
    {
      switch (avs_frame->start_code)
      {
      case 0xB0:        // sequence header
        frame_rate = avs_frame_rate(avs_frame->frame_rate_code);
        print_msg("[");
        break;
      case 0xB1: print_msg("]"); break;
      case 0xB2: print_msg("U"); break;
      case 0xB5: print_msg("E"); break;
      case 0xB7: print_msg("V"); break;
      default:   /*print_msg("?");*/ fprint_msg("<%x>",avs_frame->start_code); break;
      }
    }

    fflush(stdout);
    count ++;
    free_avs_frame(&avs_frame);

    if (max > 0 && frames >= max)
    {
      fprint_msg("\nStopping because %d frames have been read\n",frames);
      break;
    }
  }
  
  fprint_msg("\nFound %d frame%s in %d AVS item%s\n",
             frames,(frames==1?"":"s"),count,(count==1?"":"s"));
  free_avs_context(&context);
  return 0;
}

/*
 * Returns a single character which specifies the type of the access unit
 * The value of gop_start_found says whether that unit is a recovery point 
 */
static char choose_nal_type(access_unit_p access_unit, int *gop_start_found)
{
  char character_nal_type = '?';
  int ii;
  int gop_start = FALSE;
  nal_unit_p temp_nal_unit;
  int rec_point_required = FALSE;
  // FALSE: a random access point is identified as an I frame,
  // TRUE:  a random access point is identified as an I frame + recovery_point SEI.
  //        The value recovery_frame_cnt is never considered (as if it was 0).

  if (access_unit->primary_start == NULL)
    print_msg("_");
  else if (access_unit->primary_start->nal_ref_idc == 0)
  {
    if (all_slices_I(access_unit))
      character_nal_type = 'i';
    else if (all_slices_P(access_unit))
      character_nal_type = 'p';
    else if (all_slices_B(access_unit))
      character_nal_type = 'b';
    else
      character_nal_type = 'x';
  }
  else if (access_unit->primary_start->nal_unit_type == NAL_IDR)
  {
    gop_start = TRUE;
    if (all_slices_I(access_unit))
      character_nal_type = 'D';
    else
      character_nal_type = 'd';
  }
  else if (access_unit->primary_start->nal_unit_type == NAL_NON_IDR)
  {
    if (all_slices_I(access_unit))
    {
      character_nal_type = 'I';
      if (!rec_point_required) 
        gop_start = TRUE;
      else
        for (ii=0; ii<access_unit->nal_units->length; ii++)
        {
          temp_nal_unit = access_unit->nal_units->array[ii];
          if (temp_nal_unit->nal_unit_type == NAL_SEI)
          {
            if (temp_nal_unit->u.sei_recovery.payloadType == 6)
            {
              gop_start = TRUE;
              // Print a warning if more than one frame are needed for a 
              // recovery point. This is technically legal but not supported
              // in our research of random access point. 
              if (temp_nal_unit->u.sei_recovery.recovery_frame_cnt != 0)  
                fprint_msg("!!! recovery_frame_cnt = %d\n",
                           temp_nal_unit->u.sei_recovery.recovery_frame_cnt); 
            }
          }
        }
    }
    else if (all_slices_P(access_unit))
      character_nal_type = 'P';
    else if (all_slices_B(access_unit))
      character_nal_type = 'B';
    else
      character_nal_type = 'X';
  }

  *gop_start_found = gop_start;
  return character_nal_type;
}

/*
 * Report on data by access unit, as single characters
 * (access unit here means frame or coupled fields)
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int dots_by_access_unit(ES_p  es,
                               int   max,
                               int   verbose,
                               int   hash_eos,
                               int   show_gop_time)
{

  int err = 0;
  int access_unit_count = 0;
  access_unit_context_p  context;

  int gop_start_found = FALSE;
  int k_frame = 0;
  int size_gop;
  int size_gop_max = 0;
  int size_gop_min = 100000;
  int gops = 0;
  int size_gop_tot = 0;
  int is_first_k_frame = TRUE;
  char char_nal_type = 'a';
  unsigned long num_idr = 0;
  unsigned long num_i = 0;
  unsigned long num_p = 0;
  unsigned long num_b = 0;

  if (verbose)
    print_msg("\n"
              "Each character represents a single access unit\n"
              "\n"
              "    D       means an IDR.\n"
              "    d       means an IDR that is not all I slices.\n"
              "    I, P, B means all slices of the primary picture are I, P or B,\n"
              "            and this is a reference picture.\n"
              "    i, p, b means all slices of the primary picture are I, P or B,\n"
              "            and this is NOT a reference picture.\n"
              "    X or x  means that not all slices are of the same type.\n"
              "    ?       means some other type of access unit.\n"
              "    _       means that the access unit doesn't contain a primary picture.\n"
              "\n"
              "If -hasheos was specified:\n"
              "    # means an EOS (end-of-stream) NAL unit.\n"
              "\n");

  err = build_access_unit_context(es,&context);
  if (err) return err;
    
  for (;;)
  {
    access_unit_p      access_unit;

    err = get_next_h264_frame(context,TRUE,FALSE,&access_unit);
      
    if (err == EOF)
      break;
    else if (err)
    {
      free_access_unit_context(&context);
      return 1;
    }

    char_nal_type = choose_nal_type(access_unit, &gop_start_found);

    // No real gop exists in h.264 but we try to find the distance between two
    // random access points. These can be: IDR frame or I frame with a
    // recovery_point in the SEI 
    if (gop_start_found)
    {
      if (!is_first_k_frame)
      {
        size_gop = access_unit_count - k_frame;
        size_gop_max = max(size_gop_max, size_gop);
        size_gop_min = min(size_gop_min, size_gop);
        size_gop_tot += size_gop;
        gops++;
        if (show_gop_time)
          fprint_msg(": %2.4f\n",
                     (double)size_gop/frame_rate ); // that's the time duration of a "GOP"
        // (if the frame rate is 25fps)
      }
      is_first_k_frame = FALSE;
      k_frame = access_unit_count;
    }

    switch (char_nal_type) {
      case 'I':
      case 'i': num_i++; break;
      case 'D':
      case 'd': num_idr++; break;
      case 'P':
      case 'p': num_p++; break;
      case 'B':
      case 'b': num_b++; break;
      default: break;
    }

    fprint_msg("%c", char_nal_type);
    access_unit_count++;

    fflush(stdout);
    free_access_unit(&access_unit);

    // Did the logical stream end after the last access unit?
    if (context->end_of_stream)
    {
      if (hash_eos)
      {
        print_msg("#");
        // This should be enough to allow us to keep on after the EOS
        context->end_of_stream = FALSE;
        context->no_more_data = FALSE;
      }
      else
      {
        print_msg("\nStopping because found end-of-stream NAL unit\n");
        break;
      }
    }

    if (max > 0 && context->nac->count >= max)
    {
      fprint_msg("\nStopping because %d NAL units have been read\n",
                 context->nac->count);
      break;
    }
  }
  
  fprint_msg("\nFound %d NAL unit%s in %d access unit%s\n",
             context->nac->count,(context->nac->count==1?"":"s"),
             access_unit_count,(access_unit_count==1?"":"s"));
  fprint_msg("%lu IDR, %lu I, %lu P, %lu B access units\n",num_idr, num_i, num_p, num_b);

  if (gops) //only if there is more than 1 gop
    fprint_msg("GOP size (s): max=%2.4f, min=%2.4f, mean=%2.5f (frame rate = %2.2f)\n",
               (double)size_gop_max/frame_rate, (double)size_gop_min/frame_rate,
               (double)size_gop_tot/(frame_rate*gops), frame_rate);
  free_access_unit_context(&context);
  return 0;
}

/*
 * Simply report on the content of an ES file as single characters for each ES
 * unit
 *
 * - `es` is the input elementary stream
 * - `what_data` should be one of VIDEO_H262, VIDEO_H264 or VIDEO_AVS.
 * - if `max` is non-zero, then reporting will stop after `max` MPEG items
 * - if `verbose` is true, then extra information will be output
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int report_file_as_ES_dots(ES_p    es,
                                  int     what_data,
                                  int     max,
                                  int     verbose)
{
  int err = 0;
  int count = 0;
  struct ES_unit unit;

  (void) setup_ES_unit(&unit);

  if (verbose)
  {
    print_msg("\n"
              "Each character represents a single ES unit\n");
    switch (what_data)
    {
    case VIDEO_H262:
      print_msg("Pictures are represented according to their picture coding\n"
                "type, and the slices within a picture are not shown.\n"
                "    i means an I picture\n"
                "    p means a  P picture\n"
                "    b means a  B picture\n"
                "    d means a  D picture (these should not occur in MPEG-2)\n"
                "    ! means some other picture (such should not occur)\n"
                "Other items are represented as follows:\n"
                "    [ means a  Sequence header\n"
                "    > means a  Group Start header\n"
                "    E means an Extension start header\n"
                "    U means a  User data header\n"
                "    X means a  Sequence Error\n"
                "    ] means a  Sequence End\n"
                "    R means a  Reserved item\n");
      break;
    case VIDEO_H264:
      print_msg("### esdots: -es is not yet supported for H.264\n");
      return 1;
      //break;
    case VIDEO_AVS:
      print_msg("Frames are represented according to their picture coding\n"
                "type, and the slices within a frame are not shown.\n"
                "    i means an I frame\n"
                "    p means a  P frame\n"
                "    b means a  B frame\n"
                "    _ means a slice\n"
                "    ! means something else (this should not be possible)\n"
                "Other items are represented as follows:\n"
                "    [ means a  Sequence header\n"
                "    E means an Extension start header\n"
                "    U means a  User data header\n"
                "    ] means a  Sequence End\n"
                "    V means a  Video edit item\n");
    default:
      print_msg("### esdots: Unexpected type of data\n");
      return 1;
    }
    print_msg("    ? means something else. This may indicate that the stream\n"
              "      is not an ES representing AVS (it might, for instance\n"
              "      be PES)\n"
              "\n");
  }
    
  for (;;)
  {
    err = find_next_ES_unit(es,&unit);
    if (err == EOF)
      break;
    else if (err)
      return 1;

    switch (what_data)
    {
    case VIDEO_H262:
      switch (unit.start_code)
      {
        int picture_coding_type;
      case 0x00:
        picture_coding_type = (unit.data[5] & 0x38) >> 3;
        switch (picture_coding_type)
        {
        case 1: print_msg("i"); break;
        case 2: print_msg("p"); break;
        case 3: print_msg("b"); break;
        case 4: print_msg("d"); break;
        default: print_msg("!"); break;
        }
        break;
      case 0xB0: print_msg("R"); break; // Reserved
      case 0xB1: print_msg("R"); break; // Reserved
      case 0xB2: print_msg("U"); break; // User data
      case 0xB3: print_msg("["); break; // SEQUENCE HEADER
      case 0xB4: print_msg("X"); break; // Sequence error
      case 0xB5: print_msg("E"); break; // Extension start
      case 0xB6: print_msg("R"); break; // Reserved
      case 0xB7: print_msg("]"); break; // SEQUENCE END
      case 0xB8: print_msg(">"); break; // Group start
      default:
        if (unit.start_code >= 0x01 && unit.start_code <= 0xAF)
          print_msg("_");
        else
          print_msg("?");
        break;
      }
      break;
    case VIDEO_H264:
      break;
    case VIDEO_AVS:
      switch (unit.start_code)
      {
      case 0xB3:
        print_msg("i"); break;
      case 0xB6:
        switch (avs_picture_coding_type(&unit))
        {
        case AVS_P_PICTURE_CODING: print_msg("p"); break;
        case AVS_B_PICTURE_CODING: print_msg("b"); break;
        default: print_msg("!"); break;
        }
        break;
      case 0xB0: print_msg("["); break;
      case 0xB1: print_msg("]"); break;
      case 0xB2: print_msg("U"); break;
      case 0xB5: print_msg("E"); break;
      case 0xB7: print_msg("V"); break;
      default:
        if (unit.start_code < 0xB0)
          print_msg("_");
        else
          print_msg("?");
        break;
      }
    default: /* shouldn't happen */ break;
    }

    fflush(stdout);
    count ++;

    if (max > 0 && count >= max)
    {
      fprint_msg("\nStopping because %d ES units have been read\n",count);
      break;
    }
  }
  clear_ES_unit(&unit);
  
  fprint_msg("\nFound %d ES units%s\n",count,(count==1?"":"s"));
  return 0;
}

static void print_usage()
{
  print_msg(
    "Usage: esdots [switches] [<infile>]\n"
    "\n"
    );
  REPORT_VERSION("esdots");
  print_msg(
    "\n"
    "  Present the content of an H.264 (MPEG-4/AVC), H.262 (MPEG-2) or AVS\n"
    "  elementary stream as a sequence of characters, representing access\n"
    "  units/MPEG-2 items/AVS items.\n"
    "\n"
    "  (Note that for H.264 it is access units and not frames that are\n"
    "  represented, and for H.262 it is items and not pictures.)\n"
    "\n"
    "Files:\n"
    "  <infile>  is the Elementary Stream file (but see -stdin below)\n"
    "\n"
    "Switches:\n"
    "  -verbose, -v      Preface the output with an explanation of the\n"
    "                    characters being used.\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -stdin            Take input from <stdin>, instead of a named file\n"
    "  -max <n>, -m <n>  Maximum number of entities to read\n"
    "  -pes, -ts         The input file is TS or PS, to be read via the\n"
    "                    PES->ES reading mechanisms\n"
    "  -hasheos          Print a # on finding an EOS (end-of-stream) NAL unit\n"
    "                    rather than stopping (only applies to H.264)\n"
    "  -es               Report ES units, rather than any 'higher' unit\n"
    "                    (not necessarily suppported for all file types)\n"
    "  -gop              Show the duration of each GOP (for MPEG-2 steams)\n"
    "                    OR the distance between random access points (H.264)\n"
    "  -fr               Set the video frame rate (default = 25 fps)\n"
    "\n"
    "Stream type:\n"
    "  If input is from a file, then the program will look at the start of\n"
    "  the file to determine if the stream is H.264 or H.262 data. This\n"
    "  process may occasionally come to the wrong conclusion, in which case\n"
    "  the user can override the choice using the following switches.\n"
    "\n"
    "  For AVS data, the program will never guess correctly, so the user must\n"
    "  specify the file type, using -avs.\n"
    "\n"
    "  If input is from standard input (via -stdin), then it is not possible\n"
    "  for the program to make its own decision on the input stream type.\n"
    "  Instead, it defaults to H.262, and relies on the user indicating if\n"
    "  this is wrong.\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    "  -avs              Force the program to treat the input as AVS.\n"
    );
}

int main(int argc, char **argv)
{
  char  *input_name = NULL;
  int   had_input_name = FALSE;
  int   use_stdin = FALSE;
  int   err = 0;
  ES_p  es = NULL;
  int   max = 0;
  int   verbose = FALSE;
  int   ii = 1;

  int	use_pes = FALSE;
  int	hash_eos = FALSE;

  int	want_data = VIDEO_H262;
  int	is_data = want_data;
  int	force_stream_type = FALSE;

  int	want_ES = FALSE;
  int	show_gop_time = FALSE;

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-help",argv[ii]) ||
          !strcmp("-h",argv[ii]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("esdots",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### esdots: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        had_input_name = TRUE; // more or less
        use_stdin = TRUE;
      }
      else if (!strcmp("-avc",argv[ii]) || !strcmp("-h264",argv[ii]))
      {
        force_stream_type = TRUE;
        want_data = VIDEO_H264;
      }
      else if (!strcmp("-h262",argv[ii]))
      {
        force_stream_type = TRUE;
        want_data = VIDEO_H262;
      }
      else if (!strcmp("-avs",argv[ii]))
      {
        force_stream_type = TRUE;
        want_data = VIDEO_AVS;
      }
      else if (!strcmp("-es",argv[ii]))
        want_ES = TRUE;
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
        verbose = TRUE;
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("esdots",ii);
        err = int_value("esdots",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-hasheos",argv[ii]))
        hash_eos = TRUE;
      else if (!strcmp("-pes",argv[ii]) || !strcmp("-ts",argv[ii]))
        use_pes = TRUE;
      else if (!strcmp("-gop",argv[ii]))
        show_gop_time = TRUE;
      else if (!strcmp("-fr",argv[ii]))
      {
        CHECKARG("esdots",ii);
        err = double_value("esdots",argv[ii],argv[ii+1],TRUE,&frame_rate);
        if (err) return 1;
        ii++;
      }
      else
      {
        fprint_err("### esdots: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### esdots: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
      else
      {
        input_name = argv[ii];
        had_input_name = TRUE;
      }
    }
    ii++;
  }
  
  if (!had_input_name)
  {
    print_err("### esdots: No input file specified\n");
    return 1;
  }

  err = open_input_as_ES((use_stdin?NULL:input_name),use_pes,FALSE,
                         force_stream_type,want_data,&is_data,&es);
  if (err)
  {
    print_err("### esdots: Error opening input file\n");
    return 1;
  }

  if (want_ES)
    err = report_file_as_ES_dots(es,is_data,max,verbose);
  else if (is_data == VIDEO_H262)
    err = report_h262_file_as_dots(es,max,verbose,show_gop_time);
  else if (is_data == VIDEO_H264)
    err = dots_by_access_unit(es,max,verbose,hash_eos,show_gop_time);
  else if (is_data == VIDEO_AVS)
    err = report_avs_file_as_dots(es,max,verbose);
  else
  {
    print_err("### esdots: Unexpected type of video data\n");
  }

  if (err)
  {
    print_err("### esdots: Error producing 'dots'\n");
    (void) close_input_as_ES(input_name,&es);
    return 1;
  }

  err = close_input_as_ES(input_name,&es);
  if (err)
  {
    print_err("### esdots: Error closing input file\n");
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

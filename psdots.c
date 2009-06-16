/*
 * Report on the content of an H.222 program stream (PS) file as a sequence
 * of single characters, representing appropriate entities.
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
#include <limits.h>

#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "ps_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"


/*
 * Report on the given file with characters representing packets
 *
 * - `ps` is the PS file we're reading
 * - if `max` is more than zero, then it is the maximum number of PS packs
 *   we want to read
 * - `verbose` is true if we want an explanation of the characters
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int report_ps_dots(PS_reader_p  ps,
                          int          max,
                          int          verbose)
{
  int   err;
  int   count = 0;
  int   num_packs = 0;
  offset_t posn;  // The location in the input file of the current packet
  byte  stream_id; // The packet's stream id
  int   end_of_file = FALSE;

  struct PS_packet      packet = {0};
  struct PS_pack_header header = {0};

  if (verbose)
    print_msg("Characters represent the following:\n"
           "    [    Pack header\n"
           "    H    System header\n"
           "    ]    MPEG_program_end_code\n"
           "    p<n> Private stream <n> (1 or 2)\n"
           "    v    Video stream 0\n"
           "    v<n> Video stream <n> (>0)\n"
           "    a    Audio stream 0\n"
           "    a<n> Audio stream <n> (>0)\n"
           "    M    Program stream map\n"
           "    D    Program stream directory\n"
           "    .    Padding\n"
           "    ?    Something else\n"
      );
  
  // Read the start of the first packet (we confidently expect this
  // to be a pack header)
  err = read_PS_packet_start(ps,FALSE,&posn,&stream_id);
  if (err == EOF)
  {
    print_err("### Error reading first pack header\n");
    print_err("    Unexpected end of PS at start of stream\n");
    return 1;
  }
  else if (err)
  {
    print_err("### Error reading first pack header\n");
    return 1;
  }

  if (stream_id != 0xba)
  {
    print_err("### Program stream does not start with pack header\n");
    fprint_err("    First packet has stream id %02X (",stream_id);
    print_stream_id(FALSE,stream_id);
    print_err(")\n");
    return 1;
  }

  // But given that, we can now happily loop reading in packs
  for (;;)
  {
    int  num_system_headers = 0;

    if (max > 0 && num_packs >= max)
    {
      fprint_msg("\nStopping after %d packs\n",num_packs);
      return 0;
    }

    num_packs ++;
    print_msg("[");
    fflush(stdout);
    
    err = read_PS_pack_header_body(ps,&header);
    if (err)
    {
      fprint_err("### Error reading data for pack header starting at "
                 OFFSET_T_FORMAT "\n",posn);
      return 1;
    }

    // Read (and, for the moment, at least, ignore) any system headers
    for (;;)
    {
      err = read_PS_packet_start(ps,FALSE,&posn,&stream_id);
      if (err == EOF)
      {
        end_of_file = TRUE;
        if (stream_id == 0xB9)
        {
          print_msg("]");
          fflush(stdout);
        }
        break;
      }
      else if (err)
        return 1;

      if (stream_id == 0xbb) // System header
      {
        print_msg("H");
        fflush(stdout);
        err = read_PS_packet_body(ps,stream_id,&packet);
        if (err)
        {
          fprint_err("### Error reading system header starting at "
                     OFFSET_T_FORMAT "\n",posn);
          return 1;
        }
        // For the moment, just ignore the system header content
        num_system_headers ++;
      }
      else
        break;
    }
    if (end_of_file)
      break;

    // We've finished with system headers - onto data (one fondly hopes)
    for (;;)
    {
      if (stream_id == 0xba)  // Start of the next pack
        break;

      if (stream_id == 0xBC)
        print_msg("M");
      else if (stream_id == 0xFF)
        print_msg("D");
      else if (stream_id == 0xBD)
        print_msg("p1");
      else if (stream_id == 0xBE)
        print_msg(".");
      else if (stream_id == 0xBF)
        print_msg("p2");
      else if (stream_id >= 0xC0 && stream_id <=0xDF)
      {
        int number = stream_id & 0x1F;
        if (number == 0)
          print_msg("a");
        else
          fprint_msg("a%x",number);
      }
      else if (stream_id >= 0xE0 && stream_id <= 0xEF)
      {
        int number = stream_id & 0x0F;
        if (number == 0)
          print_msg("v");
        else
          fprint_msg("v%x",number);
      }
      else
        print_msg("?");
      fflush(stdout);

      err = read_PS_packet_body(ps,stream_id,&packet);
      if (err)
      {
        fprint_err("### Error reading PS packet starting at "
                   OFFSET_T_FORMAT "\n",posn);
        return 1;
      }

      err = read_PS_packet_start(ps,FALSE,&posn,&stream_id);
      if (err == EOF)
      {
        if (stream_id == 0xB9)
        {
          print_msg("]");
          fflush(stdout);
        }
        end_of_file = TRUE;
        break;
      }
      else if (err)
        return 1;
          
    }
    if (end_of_file)
      break;
  }

  clear_PS_packet(&packet);
  fprint_msg("\nRead %d PS packet%s in %d pack%s\n",
             count,(count==1?"":"s"),
             num_packs,(num_packs==1?"":"s"));
  return 0;
}

static void print_usage()
{
  print_msg(
    "Usage: psdots [switches] [<infile>]\n"
    "\n"
    );
  REPORT_VERSION("psdots");
  print_msg(
    "\n"
    "  Present the content of a Program Stream file as a sequence of\n"
    "  characters, representing the packets.\n"
    "\n"
    "Files:\n"
    "  <infile>  is an H.222 Program Stream file (but see -stdin)\n"
    "\n"
    "Switches:\n"
    "  -err stdout        Write error messages to standard output (the default)\n"
    "  -err stderr        Write error messages to standard error (Unix traditional)\n"
    "  -stdin             Input from standard input, instead of a file\n"
    "  -verbose, -v       Output a description of the characters used\n"
    "  -max <n>, -m <n>   Maximum number of PS packets to read\n"
    );
}

int main(int argc, char **argv)
{
  int    use_stdin = FALSE;
  char  *input_name = NULL;
  int    had_input_name = FALSE;

  PS_reader_p  ps;        // The PS file we're reading
  int    max     = 0;     // The maximum number of PS packets to read (or 0)
  int    verbose = FALSE; // True => output diagnostic/progress messages

  int    err = 0;
  int    ii = 1;

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (ii < argc)
  {
    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-h",argv[ii]) ||
          !strcmp("-help",argv[ii]))
      {
        print_usage();
        return 0;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("psdots",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### psdots: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        verbose = TRUE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("psdots",ii);
        err = int_value("psdots",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        use_stdin = TRUE;
        had_input_name = TRUE;  // so to speak
      }
      else
      {
        fprint_err("### psdots: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### psdots: Unexpected '%s'\n",argv[ii]);
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
    print_err("### psdots: No input file specified\n");
    return 1;
  }


  err = open_PS_file(input_name,FALSE,&ps);
  if (err)
  {
    fprint_err("### psdots: Unable to open input file %s\n",
               (use_stdin?"<stdin>":input_name));
    return 1;
  }
  fprint_msg("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  if (max)
    fprint_msg("Stopping after %d PS packets\n",max);

  err = report_ps_dots(ps,max,verbose);
  if (err)
    print_err("### psdots: Error reporting on input stream\n");


  err = close_PS_file(&ps);
  if (err)
  {
    fprint_err("### psdots: Error closing input file %s\n",
               (use_stdin?"<stdin>":input_name));
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

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
    printf("Characters represent the following:\n"
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
    fprintf(stderr,"### Error reading first pack header\n");
    fprintf(stderr,"    Unexpected end of PS at start of stream\n");
    return 1;
  }
  else if (err)
  {
    fprintf(stderr,"### Error reading first pack header\n");
    return 1;
  }

  if (stream_id != 0xba)
  {
    fprintf(stderr,"### Program stream does not start with pack header\n");
    fprintf(stderr,"    First packet has stream id %02X (",stream_id);
    print_stream_id(stdout,stream_id);
    printf(")\n");
    return 1;
  }

  // But given that, we can now happily loop reading in packs
  for (;;)
  {
    int  num_system_headers = 0;

    if (max > 0 && num_packs >= max)
    {
      printf("\nStopping after %d packs\n",num_packs);
      return 0;
    }

    num_packs ++;
    printf("[");
    fflush(stdout);
    
    err = read_PS_pack_header_body(ps,&header);
    if (err)
    {
      fprintf(stderr,
              "### Error reading data for pack header starting at "
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
          printf("]");
          fflush(stdout);
        }
        break;
      }
      else if (err)
        return 1;

      if (stream_id == 0xbb) // System header
      {
        printf("H");
        fflush(stdout);
        err = read_PS_packet_body(ps,stream_id,&packet);
        if (err)
        {
          fprintf(stderr,
                  "### Error reading system header starting at "
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
        printf("M");
      else if (stream_id == 0xFF)
        printf("D");
      else if (stream_id == 0xBD)
        printf("p1");
      else if (stream_id == 0xBE)
        printf(".");
      else if (stream_id == 0xBF)
        printf("p2");
      else if (stream_id >= 0xC0 && stream_id <=0xDF)
      {
        int number = stream_id & 0x1F;
        if (number == 0)
          printf("a");
        else
          printf("a%x",number);
      }
      else if (stream_id >= 0xE0 && stream_id <= 0xEF)
      {
        int number = stream_id & 0x0F;
        if (number == 0)
          printf("v");
        else
          printf("v%x",number);
      }
      else
        printf("?");
      fflush(stdout);

      err = read_PS_packet_body(ps,stream_id,&packet);
      if (err)
      {
        fprintf(stderr,"### Error reading PS packet starting at "
                OFFSET_T_FORMAT "\n",posn);
        return 1;
      }

      err = read_PS_packet_start(ps,FALSE,&posn,&stream_id);
      if (err == EOF)
      {
        if (stream_id == 0xB9)
        {
          printf("]");
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
  printf("\nRead %d PS packet%s in %d pack%s\n",
         count,(count==1?"":"s"),
         num_packs,(num_packs==1?"":"s"));
  return 0;
}

static void print_usage()
{
  printf(
    "Usage: psdots [switches] [<infile>]\n"
    "\n"
    );
  REPORT_VERSION("psdots");
  printf(
    "\n"
    "  Present the content of a Program Stream file as a sequence of\n"
    "  characters, representing the packets.\n"
    "\n"
    "Files:\n"
    "  <infile>  is an H.222 Program Stream file (but see -stdin)\n"
    "\n"
    "Switches:\n"
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
        fprintf(stderr,"### psdots: "
                "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprintf(stderr,"### psdots: Unexpected '%s'\n",argv[ii]);
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
    fprintf(stderr,"### psdots: No input file specified\n");
    return 1;
  }


  err = open_PS_file(input_name,FALSE,&ps);
  if (err)
  {
    fprintf(stderr,"### psdots: Unable to open input file %s\n",
            (use_stdin?"<stdin>":input_name));
    return 1;
  }
  printf("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  if (max)
    printf("Stopping after %d PS packets\n",max);

  err = report_ps_dots(ps,max,verbose);
  if (err)
    fprintf(stderr,"### psdots: Error reporting on input stream\n");


  err = close_PS_file(&ps);
  if (err)
  {
    fprintf(stderr,"### psdots: Error closing input file %s\n",
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

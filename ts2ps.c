/*
 * Given an H.222 transport stream (TS) file, extract PES data therefrom (i.e.,
 * extract the PES packets within the TS) and construct PS.
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
#ifdef _WIN32
#include <stddef.h>
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "ps_fns.h"
#include "ts_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "pidint_fns.h"
#include "pes_fns.h"
#include "version.h"


/*
 * Write a PS program end code
 *
 * Returns 0 if all went well, 1 if something went wrong
 */
static int write_program_end_code(FILE  *output)
{
  static byte program_end_code[] = {0x00, 0x00, 0x01, 0xB9};
  size_t count = fwrite(program_end_code,4,1,output);
  if (count != 1)
  {
    print_err("### Error writing PS program end code\n");
    return 1;
  }
  return 0;
}

/*
 * Write a PS pack header.
 *
 * Returns 0 if all went well, 1 if something went wrong
 */
static int write_pack_header(FILE  *output)
{
  static byte pack_header[] = {0x00, 0x00, 0x01, 0xBA,
                               0x44, 0x00, 0x04, 0x00,
                               0x04, 0x01, 0x00, 0x00,
                               0x03, 0xF8};
  size_t count;

  // For the moment, just write out an "unset" pack header
  // This is illegal because the mux rate is zero (the standard
  // specifically forbids that)
  count = fwrite(pack_header,sizeof(pack_header),1,output);
  if (count != 1)
  {
    print_err("### Error writing PS pack header out to file\n");
    return 1;
  }
  return 0;
}

/*
 * Write a PES packet from the given data
 *
 * `data_len` must be at most 0xFFFF - 3, allowing for the 3 bytes of the
 * PES header flags/header data length which we must output.
 */
static int write_PES_packet(FILE    *output,
                            byte    *data,
                            uint16_t data_len,
                            byte     stream_id)
{
  static   byte header[] = {0x00, 0x00, 0x01,
                            0xFF, 0xFF, 0xFF,  // replace 3 bytes
                            0x80, 0x00, 0x00}; // flags and header data len
  size_t   count;
  uint16_t PES_packet_length = data_len + 3;   // + 3 for the flags, etc.

  header[3] = stream_id;
  header[4] = (PES_packet_length & 0xFF00) >> 8;
  header[5] = (PES_packet_length & 0x00FF);

  count = fwrite(header,sizeof(header),1,output);
  if (count != 1)
  {
    print_err("### Error writing PS PES packet header out to file\n");
    return 1;
  }

  count = fwrite(data,data_len,1,output);
  if (count != 1)
  {
    print_err("### Error writing PS PES packet data out to file\n");
    return 1;
  }
  return 0;
}

/*
 * Extract data and output it as PS
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_data(int      input,
                        FILE    *output,
                        uint16_t program_number,
                        int      max,
                        int      verbose,
                        int      quiet)
{
  int           err;
  PES_reader_p  reader;

  err = build_PES_reader(input,TRUE,!quiet,!quiet,program_number,&reader);
  if (err)
  {
    print_err("### Error building PES reader over input file\n");
    return 1;
  }

  // Temporarily, just writes out PES packets, not a PS stream...
  for (;;)
  {
    size_t count;
    err = read_next_PES_packet(reader);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error reading next PES packet\n");
      (void) free_PES_reader(&reader);
      return 1;
    }
    err = write_pack_header(output);
    if (err)
    {
      print_err("### Error writing PS pack header\n");
      (void) free_PES_reader(&reader);
      return 1;
    }
    // It is possible that the TS data for video might have specified a zero
    // length in the PES. Our TS reader will have read all of the packet for
    // us, but will not have "adjusted" said length at the start of the packet.
    // It is thus up to us to catch this case and amend it before we output
    // the data...
    if (reader->packet->data[4] == 0 &&
        reader->packet->data[5] == 0)
    {
      int32_t PES_packet_length = reader->packet->data_len - 6;
      byte   *start  = reader->packet->data;
      // Our maximum length is determined by the maximum length we can
      // indicate in the two bytes of the PES_packet_length. When we're
      // *writing* data, we also have to allow for writing the two flag
      // bytes and PES_header_data_length that come thereafter. 
#define MAX_LENGTH 0xFFFF
      if (PES_packet_length > MAX_LENGTH)
      {
        fprint_err("PES packet of 'zero' length is really %6d - too long for one packet\n",
                   PES_packet_length);
        // Output what we can of the original packet
        reader->packet->data[4] = (MAX_LENGTH & 0xFF00) >> 8;
        reader->packet->data[5] = (MAX_LENGTH & 0x00FF);
        // Remember that we also write out the 6 bytes preceding those
        // MAX_LENGTH bytes...
        fprint_err(".. writing out %5d (%5d total)\n",MAX_LENGTH,MAX_LENGTH+6);
        count = fwrite(reader->packet->data,MAX_LENGTH+6,1,output);
        if (count != 1)
        {
          print_err("### Error writing (start of) PES packet out to file\n");
          (void) free_PES_reader(&reader);
          return 1;
        }
        PES_packet_length -= MAX_LENGTH;
        start             += MAX_LENGTH+6;
        while (PES_packet_length > 0)
        {
          // Now, when writing out chunks of data as PES packets,
          // we have 6 bytes of header (00 00 01 stream_id length/length)
          // followed by two bytes of flags (81 00) and a zero
          // PES_header_data_length (00). Those last three bytes have
          // to be included in the PES_packet_length of the PES packet
          // we write out, which means that the longest "chunk" of data
          // we can write is three less than the (otherwise) maximum.
          int this_length = min(MAX_LENGTH-3,PES_packet_length);
          int err;
          fprint_err(".. writing out %5d\n",this_length);
          err = write_PES_packet(output,start,this_length,
                                 reader->packet->data[3]);
          if (err)
          {
            print_err("### Error writing (part of) PES packet out to file\n");
            (void) free_PES_reader(&reader);
            return 1;
          }
          PES_packet_length -= this_length;
          start             += this_length;
        }
      }
      else
      {
        fprint_err("PES packet of 'zero' length, adjusting to %6d-6=%6d"
                   " (stream id %02x, 'length' %d)\n",
                   reader->packet->data_len,PES_packet_length,
                   reader->packet->data[3],reader->packet->length);
        reader->packet->data[4] = (PES_packet_length & 0xFF00) >> 8;
        reader->packet->data[5] = (PES_packet_length & 0x00FF);
        count = fwrite(reader->packet->data,reader->packet->data_len,1,output);
        if (count != 1)
        {
          print_err("### Error writing PES packet out to file\n");
          (void) free_PES_reader(&reader);
          return 1;
        }
      }
    }
    else
    {
      count = fwrite(reader->packet->data,reader->packet->data_len,1,output);
      if (count != 1)
      {
        print_err("### Error writing PES packet out to file\n");
        (void) free_PES_reader(&reader);
        return 1;
      }
    }
  }

  err = write_program_end_code(output);
  if (err)
  {
    (void) free_PES_reader(&reader);
    return 1;
  }

  (void) free_PES_reader(&reader);   // naughtily ignore the return code
  return 0;
}

static void print_usage()
{
  print_msg(
    "Usage: ts2ps [switches] [<infile>] [<outfile>]\n"
    "\n"
    );
  REPORT_VERSION("ts2ps");
  print_msg(
    "\n"
    "  Extract a single program stream from a Transport Stream.\n"
    "\n"
    "  WARNING: This software does not yet generate legitimate PS data.\n"
    "           In particular, the PS pack header records are illegal.\n"
    "\n"
    "Files:\n"
    "  <infile>  is an H.222 Transport Stream file (but see -stdin)\n"
    "  <outfile> is an H.222 Program Stream file (but see -stdout)\n"
    "\n"
    "General switches:\n"
    "  -err stdout        Write error messages to standard output (the default)\n"
    "  -err stderr        Write error messages to standard error (Unix traditional)\n"
    "  -stdin             Input from standard input, instead of a file\n"
    "  -stdout            Output to standard output, instead of a file\n"
    "                     Forces -quiet and -err stderr.\n"
    "  -verbose, -v       Output informational/diagnostic messages\n"
    "  -quiet, -q         Only output error messages\n"
    "  -max <n>, -m <n>   Maximum number of TS packets to read\n"
    "                     (not currently used)\n"
    "  -prog <n>          Choose program number <n> (default 0, which means\n"
    "                     the first one found).\n"
    );
}

int main(int argc, char **argv)
{
  int    use_stdout = FALSE;
  int    use_stdin = FALSE;
  char  *input_name = NULL;
  char  *output_name = NULL;
  int    had_input_name = FALSE;
  int    had_output_name = FALSE;

  int       input   = -1;    // Our input file descriptor
  FILE     *output  = NULL;  // The stream we're writing to (if any)
  int       max     = 0;     // The maximum number of TS packets to read (or 0)
  int       quiet   = FALSE; // True => be as quiet as possible
  int       verbose = FALSE; // True => output diagnostic/progress messages
  uint16_t  program_number = 0;

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
        quiet = FALSE;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        verbose = FALSE;
        quiet = TRUE;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("ts2ps",ii);
        err = int_value("ts2ps",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-prog",argv[ii]))
      {
        int temp;
        CHECKARG("ts2ps",ii);
        err = int_value("ts2ps",argv[ii],argv[ii+1],TRUE,10,&temp);
        if (err) return 1;
        program_number = temp;
        ii++;
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        use_stdin = TRUE;
        had_input_name = TRUE;  // so to speak
      }
      else if (!strcmp("-stdout",argv[ii]))
      {
        use_stdout = TRUE;
        had_output_name = TRUE;  // so to speak
        redirect_output_stderr();
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("ts2ps",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### ts2ps: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else
      {
        fprint_err("### ts2ps: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name && had_output_name)
      {
        fprint_err("### ts2ps: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
      else if (had_input_name)  // shouldn't do this if had -stdout
      {
        output_name = argv[ii];
        had_output_name = TRUE;
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
    print_err("### ts2ps: No input file specified\n");
    return 1;
  }

  if (!had_output_name)
  {
    print_err("### ts2ps: No output file specified\n");
    return 1;
  }
  
  // Try to stop extraneous data ending up in our output stream
  if (use_stdout)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  if (use_stdin)
    input = STDIN_FILENO;
  else
  {
    input = open_binary_file(input_name,FALSE);
    if (input == -1)
    {
      fprint_err("### ts2ps: Unable to open input file %s\n",input_name);
      return 1;
    }
  }
  if (!quiet)
    fprint_msg("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  if (had_output_name)
  {
    if (use_stdout)
      output = stdout;
    else
    {
      output = fopen(output_name,"wb");
      if (output == NULL)
      {
        if (!use_stdin) (void) close_file(input);
        fprint_err("### ts2ps: "
                   "Unable to open output file %s: %s\n",output_name,
                   strerror(errno));
        return 1;
      }
    }
    if (!quiet)
      fprint_msg("Writing to   %s\n",(use_stdout?"<stdout>":output_name));
  }
  
  if (max && !quiet)
    fprint_msg("Stopping after %d TS packets\n",max);

  err = extract_data(input,output,program_number,max,verbose,quiet);
  if (err)
  {
    print_err("### ts2ps: Error extracting data\n");
    if (!use_stdin)  (void) close_file(input);
    if (!use_stdout) (void) fclose(output);
    return 1;
  }

  // And tidy up when we're finished
  if (!use_stdout)
  {
    errno = 0;
    err = fclose(output);
    if (err)
    {
      fprint_err("### ts2ps: Error closing output file %s: %s\n",
                 output_name,strerror(errno));
      (void) close_file(input);
      return 1;
    }
  }
  if (!use_stdin)
  {
    err = close_file(input);
    if (err)
      fprint_err("### ts2ps: Error closing input file %s\n",input_name);
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

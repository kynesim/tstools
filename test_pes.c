/*
 * Test the PES reading facilities
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
#include <string.h>
#include <errno.h>

#include "compat.h"
#include "pes_fns.h"
#include "pidint_fns.h"
#include "misc_fns.h"
#include "ps_fns.h"
#include "ts_fns.h"
#include "es_fns.h"
#include "h262_fns.h"
#include "tswrite_fns.h"
#include "version.h"

/*
 * Write out TS program data based on the information we have
 */
static int write_program_data_A(PES_reader_p  reader,
                              TS_writer_p     output)
{
  // We know we support at most two program streams for output
  int      num_progs = 0;
  uint32_t prog_pids[2];
  byte     prog_type[2];
  int      err;
  uint32_t pcr_pid, pmt_pid;

  if (reader->is_TS)
  {
    // For TS, we can use the stream types from the PMT itself
    int number;

    if (reader->video_pid != 0)
    {
      pmt_stream_p stream = pid_stream_in_pmt(reader->program_map,
                                              reader->video_pid);
      if (stream == NULL)
      {
        fprintf(stderr,"### Cannot find video PID %04x in program map\n",
                reader->video_pid);
        return 1;
      }
      prog_pids[0] = reader->output_video_pid; // may not be the same
      prog_type[0] = stream->stream_type;
      num_progs = 1;
      pcr_pid = reader->video_pid;
    }
    if (reader->audio_pid != 0)
    {
      pmt_stream_p stream = pid_stream_in_pmt(reader->program_map,
                                              reader->audio_pid);
      if (stream == NULL)
      {
        fprintf(stderr,"### Cannot find audio PID %04x in program map\n",
                reader->audio_pid);
        return 1;
      }
      prog_pids[num_progs] = reader->output_audio_pid; // may not be the same
      prog_type[num_progs] = stream->stream_type;
      num_progs++;
    }
    pmt_pid = reader->pmt_pid;
  }
  else
  {
    // For PS, we have to be given appropriate PIDs, and we need to
    // deduce stream types from the stream ids. Which, unfortunately,
    // we can't do.

    // For now, avoid the whole issue and just force some values...
    num_progs = 1;
    prog_pids[0] = 0x68;                    // hard-wired for video
    prog_type[0] = MPEG2_VIDEO_STREAM_TYPE; // hard-wired for now
    pcr_pid = 0x68;

    if (reader->audio_stream_id != 0)
    {
      prog_pids[1] = 0x67;                    // hard-wired again
      prog_type[1] = MPEG2_AUDIO_STREAM_TYPE; // a random guess
      num_progs = 2;
    }
    pmt_pid = 0x66;
  }
  
  err = write_TS_program_data2(output,
                               1, // transport stream id
                               reader->program_number,
                               pmt_pid,pcr_pid,
                               num_progs,prog_pids,prog_type);
  if (err)
  {
    fprintf(stderr,"### Error writing out TS program data\n");
    return 1;
  }
  return 0;

}

/*
 * Read PES packets and write them out to the target
 *
 * Returns 0 if all went well, 1 if an error occurred.
 */
static int play_pes_packets(PES_reader_p   reader,
                            TS_writer_p    output)
{
  int  err;
  int  ii;
  int  pad_start = 8;
  int  index = 0;

  ES_p es;  // A view of our PES packets as ES units
  
  // Start off our output with some null packets - this is in case the
  // reader needs some time to work out its byte alignment before it starts
  // looking for 0x47 bytes
  for (ii=0; ii<pad_start; ii++)
  {
    err = write_TS_null_packet(output);
    if (err) return 1;
  }

  // Wrap our PES stream up as an ES stream
  err = build_elementary_stream_PES(reader,&es);
  if (err)
  {
    fprintf(stderr,"### Error trying to build ES reader from PES reader\n");
    return 1;
  }

  for (;;)
  {
    h262_item_p  item;

    if (index % 500 == 0)
    {
      // Write out program data as we come to know it
      err = write_program_data_A(reader,output);
      if (err) return 1;
    }

    // Iterate our count here so that the first item is numbered 1
    index++;

    err = find_next_h262_item(es,&item);
    if (err == EOF)
      break;
    else if (err)
    {
      fprintf(stderr,"### Error copying NAL units\n");
      return err;
    }

    err = write_ES_as_TS_PES_packet(output,item->unit.data,
                                    item->unit.data_len,DEFAULT_VIDEO_PID,
                                    DEFAULT_VIDEO_STREAM_ID);
    if (err)
    {
      fprintf(stderr,"### Error writing MPEG2 item\n");
      return err;
    }

    free_h262_item(&item);
  }

  close_elementary_stream(&es);
  return 0;
}

static int test1(PES_reader_p  reader,
                 int           verbose)
{
  PES_packet_data_p  packet;
  int      ii;
  int      err;
  byte    *old_data;
  uint32_t old_data_len;

  if (verbose)
    printf("-------------------------- Test 1 --------------------------\n");
  for (ii = 0; ii < 10; ii++)
  {
    err = read_next_PES_packet(reader);
    if (err == EOF)
    {
      if (reader->give_info) printf("EOF\n");
      break;
    }
    else if (err)
    {
      fprintf(stderr,"### test_pes: Error reading next PES packet\n");
      return 1;
    }
    packet = reader->packet;
    if (verbose)
    {
      printf("\n>> PS packet at " OFFSET_T_FORMAT " is %02x (",
             packet->posn,packet->data[3]);
      print_stream_id(TRUE,packet->data[3]);
      printf(")\n");
      print_data(TRUE,"   Data",packet->data,packet->data_len,20);

      err = report_PES_data_array("",packet->data,packet->data_len,FALSE);
      if (err) return 1;
    }
  }

  err = read_next_PES_packet(reader);
  if (err)
  {
    fprintf(stderr,"### test_pes: Error reading next PES packet\n");
    return 1;
  }
  packet = reader->packet;
  if (verbose)
  {
    printf("\n>> PS packet at " OFFSET_T_FORMAT " is %02x (",
           packet->posn,packet->data[3]);
    print_stream_id(TRUE,packet->data[3]);
    printf(")\n");
    print_data(TRUE,"   Data",packet->data,packet->data_len,20);
  }

  old_data = malloc(packet->data_len);
  if (old_data == NULL)
  {
    fprintf(stderr,"### Error allocating data array\n");
    return 1;
  }
  memcpy(old_data,packet->data,packet->data_len);
  old_data_len = packet->data_len;
  
  if (verbose)
    printf("\n** Rewinding to the start of said packet again\n");
  err = set_PES_reader_position(reader,packet->posn);
  if (err)
  {
    fprintf(stderr,"### test_pes: Error seeking to previous PES packet\n");
    free(old_data);
    return 1;
  }

  if (verbose)
    printf("** Reading packet the second time\n");
  err = read_next_PES_packet(reader);
  if (err)
  {
    fprintf(stderr,"### test_pes: Error reading next PES packet\n");
    free(old_data);
    return 1;
  }
  packet = reader->packet;
  if (verbose)
  {
    printf("\n>> PS packet at " OFFSET_T_FORMAT " is %02x (",
           packet->posn,packet->data[3]);
    print_stream_id(TRUE,packet->data[3]);
    printf(")\n");
    print_data(TRUE,"   Data",packet->data,packet->data_len,20);
  }
  if (packet->data_len != old_data_len)
  {
    fprintf(stderr,
            "### Test1: first packet length %d, second packet length %d\n",
            old_data_len,packet->data_len);
    free(old_data);
    return 1;
  }
  else if (memcmp(packet->data,old_data,packet->data_len))
  {
    fprintf(stderr,"### Test1: packet data differs\n");
    print_data(FALSE,"    Packet 1",old_data,old_data_len,50);
    print_data(FALSE,"    Packet 2",packet->data,packet->data_len,50);
    free(old_data);
    return 1;
  }
  if (verbose)
    printf("------------------------------------------------------------\n");
  
  // Even in a test it's a good idea to tidy up
  free(old_data);

  return 0;
}

static void print_usage()
{
  printf(
    "Usage: test_pes <input-file> <host>[:<port>]\n"
    "\n"
    );
  REPORT_VERSION("test_pes");
  printf(
    "\n"
    "  Test the PES reading facilities. <input-file> should be a TS\n"
    "  (Transport Stream) or PS (Program Stream) file.\n"
    "\n"
    "Input:\n"
    "  <input-file>       An H.222.0 TS or PS file.\n"
    "  <host>             The host to which to write TS packets, over\n"
    "                     TCP/IP. If <port> is not specified, it defaults\n"
    "                     to 88.\n"
    "\n"
    "Switches:\n"
    "  -quiet, -q        Suppress informational and warning messages.\n"
    "  -verbose, -v      Output additional diagnostic messages\n"
    "  -noaudio          Ignore any audio data\n"
    "  -nohost           Don't try to connect to the host\n"
    );
}

int main(int argc, char **argv)
{
  char        *input_name = NULL;
  char        *output_name = NULL;
  int          had_input_name = FALSE;
  int          had_output_name = FALSE;
  int          port = 88; // Useful default port number
  PES_reader_p reader = NULL;
  TS_writer_p  output = NULL;
  int          quiet = FALSE;
  int          verbose = FALSE;
  int          video_only = FALSE;
  int          want_output = TRUE;

  int  err;
  int  ii = 1;

  if (argc < 2)
  {
    print_usage();
    return 1;
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
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        quiet = TRUE;
        verbose = FALSE;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        verbose = TRUE;
        quiet = FALSE;
      }
      else if (!strcmp("-noaudio",argv[ii]))
      {
        video_only = TRUE;
      }
      else if (!strcmp("-nohost",argv[ii]))
      {
        want_output = FALSE;
      }
      else
      {
        fprintf(stderr,"### test_pes: "
                "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name && (want_output && had_output_name))
      {
        fprintf(stderr,"### test_pes: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
      else if (had_input_name && want_output)
      {
        err = host_value("test_pes",NULL,argv[ii],&output_name,&port);
        if (err) return 1;
        had_output_name = TRUE; // more or less
        ii++;
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
    fprintf(stderr,"### test_pes: No input file specified\n");
    return 1;
  }
  if (want_output && !had_output_name)
  {
    fprintf(stderr,"### test_pes: No target host specified\n");
    return 1;
  }

  err = open_PES_reader(input_name,!quiet,!quiet,&reader);
  if (err)
  {
    fprintf(stderr,"### test_pes: Error opening file %s\n",input_name);
    return 1;
  }

  if (!quiet)
    printf("Opened file %s (as %s)\n",input_name,(reader->is_TS?"TS":"PS"));

  set_PES_reader_video_only(reader,video_only);

  if (want_output)
  {
    err = tswrite_open(TS_W_TCP,output_name,NULL,port,quiet,&output);
    if (err)
    {
      (void) close_PES_reader(&reader);
      fprintf(stderr,"### test_pes: Unable to connect to %s\n",output_name);
      return 1;
    }
  }

  err = test1(reader,verbose);
  if (err)
  {
    fprintf(stderr,"### test_pes: Test 1 failed\n");
    (void) close_PES_reader(&reader);
    (void) tswrite_close(output,TRUE);
    return 1;
  }
  if (!quiet)
    printf("** Test 1 passed\n"
           "** Rewinding\n");
  err = set_PES_reader_position(reader,0);
  if (err)
  {
    fprintf(stderr,"### test_pes: Error seeking to previous PES packet\n");
    return 1;
  }

  if (want_output)
  {
    err = play_pes_packets(reader,output);
    if (err)
    {
      fprintf(stderr,"### test_pes: Error playing PES packets\n");
      (void) close_PES_reader(&reader);
      (void) tswrite_close(output,TRUE);
      return 1;
    }
  }

  if (want_output)
  {
    err = tswrite_close(output,quiet);
    if (err)
      fprintf(stderr,"### test_pes: Error closing output %s: %s\n",output_name,
              strerror(errno));
  }
  err = close_PES_reader(&reader);
  if (err)
  {
    fprintf(stderr,"### test_pes: Error closing file %s\n",input_name);
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

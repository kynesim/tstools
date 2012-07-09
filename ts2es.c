/*
 * Given an H.222 transport stream (TS) file, extract elementary stream
 * data therefrom (i.e., extract the ES from the PES packets within the
 * TS).
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
#include "ts_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "pidint_fns.h"
#include "es_fns.h"
#include "pes_fns.h"
#include "version.h"

// A three-way choice for what to output by PID
enum pid_extract
{
  EXTRACT_UNDEFINED,
  EXTRACT_VIDEO,  // Output the first "named" video stream
  EXTRACT_AUDIO,  // Ditto for audio
  EXTRACT_PID,    // Output an explicit PID
};
typedef enum pid_extract EXTRACT;


/*
 * Extract all the TS packets for either a video or audio stream.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_av_via_pes(char  *input_name,
                              char  *output_name,
                              int    want_video,
                              int    quiet)
{
  int          err;
  PES_reader_p reader = NULL;
  ES_p         es = NULL;
  FILE        *output;

  if (!want_video)
  {
    print_err("### Audio output is not supported via PES in this utility\n");
    return 1;
  }

  output = fopen(output_name,"wb");
  if (output == NULL)
  {
    fprint_err("### Unable to open output file %s: %s\n",output_name,
               strerror(errno));
    return 1;
  }
  
  err = open_PES_reader(input_name,!quiet,!quiet,&reader);
  if (err)
  {
    fprint_err("### Error opening file %s\n",input_name);
    fclose(output);
    return 1;
  }

  set_PES_reader_video_only(reader,TRUE);

  // Wrap our PES stream up as an ES stream
  err = build_elementary_stream_PES(reader,&es);
  if (err)
  {
    print_err("### Error trying to build ES reader from PES reader\n");
    (void) close_PES_reader(&reader);
    (void) fclose(output);
    return 1;
  }

  for (;;)
  {
    ES_unit_p  unit;
    err = find_and_build_next_ES_unit(es,&unit);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error reading next ES unit\n");
      (void) fclose(output);
      (void) close_PES_reader(&reader);
      close_elementary_stream(&es);
      return 1;
    }
    err = write_ES_unit(output,unit);
    if (err)
    {
      print_err("### Error writing ES unit out to file\n");
      free_ES_unit(&unit);
      (void) fclose(output);
      (void) close_PES_reader(&reader);
      close_elementary_stream(&es);
      return 1;
    }
    free_ES_unit(&unit);
  }

  (void) fclose(output);              // naughtily ignore the return code
  (void) close_PES_reader(&reader);   // naughtily ignore the return code
  close_elementary_stream(&es);
  return 0;
}

/*
 * Extract all the TS packets for a nominated PID to another file.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_pid_packets(TS_reader_p  tsreader,
                               FILE        *output,
                               uint32_t     pid_wanted,
                               int          max,
                               int          verbose,
                               int          quiet)
{
  int    err;
  int    count = 0;
  int    extracted = 0;
  int    pes_packet_len = 0;
  int    got_pes_packet_len = FALSE;
  // It doesn't make sense to start outputting data for our PID until we
  // get the start of a packet
  int    need_packet_start = TRUE;
  
  for (;;)
  {
    uint32_t pid;
    int      payload_unit_start_indicator;
    byte    *adapt, *payload;
    int      adapt_len, payload_len;
    
    if (max > 0 && count >= max)
    {
      if (!quiet) fprint_msg("Stopping after %d packets\n",max);
      break;
    }

    err = get_next_TS_packet(tsreader,&pid,
                             &payload_unit_start_indicator,
                             &adapt,&adapt_len,&payload,&payload_len);
    if (err == EOF)
      break;
    else if (err)
    {
      print_err("### Error reading TS packet\n");
      return 1;
    }
    
    count++;

    // If the packet is empty, all we can do is ignore it
    if (payload_len == 0)
      continue;

    if (pid == pid_wanted)
    {
      byte  *data;
      int    data_len;
      size_t written;

      if (verbose)
      {
        fprint_msg("%4d: TS Packet PID %04x",count,pid);
        if (payload_unit_start_indicator)
          print_msg(" (start)");
        else if (need_packet_start)
          print_msg(" <ignored>");
        print_msg("\n");
      }


      if (payload_unit_start_indicator)
      {
        // It's the start of a PES packet, so we need to drop the header
        int offset;

        if (need_packet_start)
          need_packet_start = FALSE;

        pes_packet_len = (payload[4] << 8) | payload[5];
        if (verbose) fprint_msg("PES packet length %d\n",pes_packet_len);
        got_pes_packet_len = (pes_packet_len > 0);

        if (IS_H222_PES(payload))
        {
          // It's H.222.0 - payload[8] is the PES_header_data_length,
          // so our ES data starts that many bytes after that field
          offset = payload[8] + 9;
        }
        else
        {
          // We assume it's MPEG-1
          offset = calc_mpeg1_pes_offset(payload,payload_len);
        }
        data = &payload[offset];
        data_len = payload_len-offset;
        if (verbose) print_data(TRUE,"data",data,data_len,1000);
      }
      else
      {
        // If we haven't *started* a packet, we can't use this,
        // since it will just look like random bytes when written out.
        if (need_packet_start)
          {
            continue;
          }

        data = payload;
        data_len = payload_len;
        if (verbose) print_data(TRUE,"Data",payload,payload_len,1000);

        if (got_pes_packet_len)
        {
          // Try not to write more data than the PES packet declares
          if (data_len > pes_packet_len)
          {
            data_len = pes_packet_len;
            if (verbose) print_data(TRUE,"Reduced data",data,data_len,1000);
            pes_packet_len = 0;
          }
          else
            pes_packet_len -= data_len;
        }
      }
      if (data_len > 0)
      {
        // Windows doesn't seem to like writing 0 bytes, so be careful...
        written = fwrite(data,data_len,1,output);
        if (written != 1)
        {
          fprint_err("### Error writing TS packet - units written = %d\n",
                     (int)written);
          return 1;
        }
      }
      extracted ++;
    }
  }

  if (!quiet)
    fprint_msg("Extracted %d of %d TS packet%s\n",
               extracted,count,(count==1?"":"s"));

  // If the user has forgotten to say -pid XX, or -video/-audio,
  // and are piping the output to another program, it can be surprising
  // if there is no data!
  if (quiet && extracted == 0)
    fprint_err("### No data extracted for PID %#04x (%d)\n",
               pid_wanted,pid_wanted);
  return 0;
}

/*
 * Extract all the TS packets for either a video or audio stream.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_av(int   input,
                      FILE *output,
                      int   want_video,
                      int   max,
                      int   verbose,
                      int   quiet)
{
  int      err, ii;
  int      max_to_read = max;
  int      total_num_read = 0;
  uint32_t pid = 0;
  TS_reader_p tsreader = NULL;
  pmt_p       pmt = NULL;

  // Turn our file into a TS reader
  err = build_TS_reader(input,&tsreader);
  if (err) return 1;

  // First, find out what program streams we actually have
  for (;;)
  {
    int  num_read;

    // Give up if we've read more than our limit
    if (max > 0 && max_to_read <= 0)
      break;

    err = find_pmt(tsreader, 1, max_to_read,verbose,quiet,&num_read,&pmt);
    if (err == EOF)
    {
      if (!quiet)
        print_msg("No program stream information in the input file\n");
      free_TS_reader(&tsreader);
      free_pmt(&pmt);
      return 0;
    }
    else if (err)
    {
      print_err("### Error finding program stream information\n");
      free_TS_reader(&tsreader);
      free_pmt(&pmt);
      return 1;
    }
    max_to_read -= num_read;
    total_num_read += num_read;

    // From that, find a stream of the type we want...
    // Note that the audio detection will accept either DVB or ADTS Dolby (AC-3)
    // stream types
    for (ii=0; ii < pmt->num_streams; ii++)
    {
      if (( want_video && IS_VIDEO_STREAM_TYPE(pmt->streams[ii].stream_type)) ||
          (!want_video && (IS_AUDIO_STREAM_TYPE(pmt->streams[ii].stream_type))))
      {
        pid = pmt->streams[ii].elementary_PID;
        break;
      }
    }
    free_pmt(&pmt);

    // Did we find what we want? If not, go round again and look for the
    // next PMT (subject to the number of records we're willing to search)
    if (pid != 0)
      break;
  }

  if (pid == 0)
  {
    fprint_err("### No %s stream specified in first %d TS packets in input file\n",
               (want_video?"video":"audio"),max);
    free_TS_reader(&tsreader);
    return 1;
  }

  if (!quiet)
    fprint_msg("Extracting %s PID %04x (%d)\n",(want_video?"video":"audio"),
               pid,pid);

  // Amend max to take account of the packets we've already read
  max -= total_num_read;

  // And do the extraction.
  err = extract_pid_packets(tsreader,output,pid,max,verbose,quiet);
  free_TS_reader(&tsreader);
  return err;
}

/*
 * Extract all the TS packets for a nominated PID to another file.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_pid(int          input,
                       FILE        *output,
                       uint32_t     pid_wanted,
                       int          max,
                       int          verbose,
                       int          quiet)
{
  int    err;
  TS_reader_p tsreader = NULL;

  // Turn our file into a TS reader
  err = build_TS_reader(input,&tsreader);
  if (err) return 1;

  err = extract_pid_packets(tsreader,output,pid_wanted,max,verbose,quiet);

  free_TS_reader(&tsreader);
  return err;
}

static void print_usage()
{
  print_msg(
    "Usage: ts2es [switches] [<infile>] [<outfile>]\n"
    "\n"
    );
  REPORT_VERSION("ts2es");
  print_msg(
    "\n"
    "  Extract a single (elementary) program stream from a Transport Stream\n"
    "  (or Program Stream).\n"
    "\n"
    "Files:\n"
    "  <infile>  is an H.222 Transport Stream file (but see -stdin and -pes)\n"
    "  <outfile> is a single elementary stream file (but see -stdout)\n"
    "\n"
    "Which stream to extract:\n"
    "  -pid <pid>         Output data for the stream with the given\n"
    "                     <pid>. Use -pid 0x<pid> to specify a hex value\n"
    "  -video             Output data for the (first) video stream\n"
    "                     named in the (first) PMT. This is the default.\n"
    "  -audio             Output data for the (first) audio stream\n"
    "                     named in the (first) PMT\n"
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
    "\n"
    "  -pes, -ps          Use the PES interface to read ES units from\n"
    "                     the input file. This allows PS data to be read\n"
    "                     (there is no point in using this for TS data).\n"
    "                     Does not support -pid, -stdin or -stdout.\n"
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
  char  *action_switch = "None";

  EXTRACT   extract = EXTRACT_VIDEO; // What we're meant to extract
  int       input   = -1;    // Our input file descriptor
  FILE     *output  = NULL;  // The stream we're writing to (if any)
  int       max     = 0;     // The maximum number of TS packets to read (or 0)
  uint32_t  pid     = 0;     // The PID of the (single) stream to extract
  int       quiet   = FALSE; // True => be as quiet as possible
  int       verbose = FALSE; // True => output diagnostic/progress messages
  int       use_pes = FALSE;

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
        CHECKARG("ts2es",ii);
        err = int_value("ts2es",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pes",argv[ii]) || !strcmp("-ps",argv[ii]))
      {
        use_pes = TRUE;
      }
      else if (!strcmp("-pid",argv[ii]))
      {
        CHECKARG("ts2es",ii);
        err = unsigned_value("ts2es",argv[ii],argv[ii+1],0,&pid);
        if (err) return 1;
        ii++;
        extract = EXTRACT_PID;
      }
      else if (!strcmp("-video",argv[ii]))
      {
        extract = EXTRACT_VIDEO;
      }
      else if (!strcmp("-audio",argv[ii]))
      {
        extract = EXTRACT_AUDIO;
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
        CHECKARG("ts2es",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### ts2es: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else
      {
        fprint_err("### ts2es: "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name && had_output_name)
      {
        fprint_err("### ts2es: Unexpected '%s'\n",argv[ii]);
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
    print_err("### ts2es: No input file specified\n");
    return 1;
  }

  if (!had_output_name)
  {
    fprint_err("### ts2es: "
               "No output file specified for %s\n",action_switch);
    return 1;
  }

  // ============================================================
  // Testing PES output
  if (use_pes && extract == EXTRACT_PID)
  {
    print_err("### ts2es: -pid is not supported with -pes\n");
    return 1;
  }
  if (use_pes && use_stdout)
  {
    print_err("### ts2es: -stdout is not supported with -pes\n");
    return 1;
  }
  if (use_pes && use_stdin)
  {
    print_err("### ts2es: -stdin is not supported with -pes\n");
    return 1;
  }
  if (use_pes)
  {
    err = extract_av_via_pes(input_name,output_name,(extract==EXTRACT_VIDEO),
                             quiet);
    if (err)
    {
      print_err("### ts2es: Error writing via PES\n");
      return 1;
    }
    return 0;
  }
  // ============================================================
  
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
      fprint_err("### ts2es: Unable to open input file %s\n",input_name);
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
        fprint_err("### ts2es: "
                   "Unable to open output file %s: %s\n",output_name,
                   strerror(errno));
        return 1;
      }
    }
    if (!quiet)
      fprint_msg("Writing to   %s\n",(use_stdout?"<stdout>":output_name));
  }

  if (!quiet)
  {
    if (extract == EXTRACT_PID)
      fprint_msg("Extracting packets for PID %04x (%d)\n",pid,pid);
    else
      fprint_msg("Extracting %s\n",(extract==EXTRACT_VIDEO?"video":"audio"));
  }
  
  if (max && !quiet)
    fprint_msg("Stopping after %d TS packets\n",max);

  if (extract == EXTRACT_PID)
    err = extract_pid(input,output,pid,max,verbose,quiet);
  else
    err = extract_av(input,output,(extract==EXTRACT_VIDEO),
                     max,verbose,quiet);
  if (err)
  {
    print_err("### ts2es: Error extracting data\n");
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
      fprint_err("### ts2es: Error closing output file %s: %s\n",
                 output_name,strerror(errno));
      (void) close_file(input);
      return 1;
    }
  }
  if (!use_stdin)
  {
    err = close_file(input);
    if (err)
      fprint_err("### ts2es: Error closing input file %s\n",input_name);
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

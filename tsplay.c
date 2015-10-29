/*
 * Play (stream) TS packets.
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
#else // _WIN32
#include <unistd.h>
#endif // _WIN32

#include <time.h>       // Sleeping and timing

#include "compat.h"
#include "printing_fns.h"
#include "tsplay_fns.h"
#include "tswrite_fns.h"
#include "printing_fns.h"
#include "misc_fns.h"
#include "version.h"

#include "ps_fns.h"
#include "pes_fns.h"
#include "pidint_fns.h"

static void print_usage(int summary)
{
  print_msg(
    "Basic usage: tsplay  <infile>  <host>[:<port>]\n"
    "\n"
    );
  REPORT_VERSION("tsplay");
  if (summary)
    print_msg(
      "\n"
      "  Play the given file (containing Transport Stream or Program Stream\n"
      "  data) 'at' the nominated host, or to an output file. The output\n"
      "  is always Transport Stream.\n"
      );
  else
    print_msg(
      "\n"
      "  Reads from a file containing H.222.0 (ISO/IEC 13818-1) Transport\n"
      "  Stream or Program Stream data (converting PS to TS as it goes),\n"
      "  and 'plays' the Transport Stream 'at' the nominated host, or to an\n"
      "  output file.\n"
      "\n"
      "  Assumes a single program in the file, and for PS assumes that the\n"
      "  program stream is well formed - i.e., that it starts with a pack\n"
      "  header. A PS stream that ends after a PES packet, but without an\n"
      "  MPEG_program_end_code will cause a warning message, but will not\n"
      "  be treated as an error.\n"
      "\n"
      "  Output to a file is only provided for testing purposes, and does\n"
      "  not use the buffering/child process mechanisms (although this may\n"
      "  not be clear from the informative messages output).\n"
      "\n"
      "  Note that most switches can be placed anywhere on the command line.\n"
      );
  print_msg(
    "\n"
    "Input:\n"
    "  <infile>          Input is from the named H.222 TS file.\n"
    "  -stdin            Input is from standard input.\n"
    "\n"
    "Output:\n"
    "  <host>\n"
    "  <host>:<port>     Normally, output is to a named host.\n"
    "                    If <port> is not specified, it defaults to 88.\n"
    "                    Output defaults to UDP.\n"
    "  -output <name>\n"
    "  -o <name>         Output is to file <name>.\n"
    "\n"
    "  -tcp              Output to the host is via TCP.\n"
    "  -udp              Output to the host is via UDP (the default).\n"
    );
  if (summary)
    print_msg(
      "  -stdout           Output is to standard output. Forces -quiet and -err stderr.\n"
      );
  else
    print_msg(
      "  -err stdout       Write error messages to standard output (the default)\n"
      "  -err stderr       Write error messages to standard error (Unix traditional)\n"
      "  -stdout           Output is to standard output. This does not make sense\n"
      "                    with -tcp or -udp. This forces -quiet and -err stderr.\n"
      );
  print_msg(
    "\n"
    "  -mcastif <ipaddr>\n"
    "  -i <ipaddr>       If output is via UDP, and <host> is a multicast\n"
    "                    address, then <ipaddr> is the IP address of the\n"
    "                    network interface to use. This may not be supported\n"
    "                    on some versions of Windows.\n"
    "\n"
    "General Switches:\n"
    "  -quiet, -q        Only output error messages\n"
    "  -verbose, -v      Output progress messages\n"
    "  -help <subject>   Show help on a particular subject\n"
    "  -help             Summarise the <subject>s that can be specified\n"
    "\n"
    );
  if (summary)
    print_msg(
      "  -max <n>, -m <n>  Maximum number of TS/PS packets to read.\n"
      "                    See -details for more information.\n"
      "  -loop             Play the input file repeatedly. Can be combined\n"
      "                    with -max.\n"
      );
  else
    fprint_msg(
      "Normal operation outputs some messages summarising the command line\n"
      "choices, information about the circular buffer filling, and\n"
      "confirmation when the program is ending.\n"
      "Quiet operation endeavours only to output error messages.\n"
      "Verbose operation outputs a progress message every %d TS packets,\n"
      "and a note whenever the input file is rewound for -loop.\n"
      "\n"
      "  -loop             Play the input file repeatedly.\n"
      "\n"
      "This assumes that the file ends neatly - i.e., not with a partial\n"
      " TS packet. On the other hand, it *can* be combined with -max to\n"
      "allow for short video sequences to be repeated.\n"
      "\n"
      "If PS data is being read, and looping has been selected, then the\n"
      "verbosity flags only apply to the first time through the data, and\n"
      "thereafter it is as if -quiet had been specified.\n"
      "\n"
      "  -max <n>, -m <n>  Maximum number of TS packets to read\n"
      "                    (or PS packets if the input data is PS)\n"
      "\n"
      "Note that the -max value must be at least 7 * <buffer size>, since\n"
      "the parent process adds TS packets to the circular buffer 7 at a\n"
      "time, and the child process waits until the buffer has filled up\n"
      "before doing anything. In recognition of this, if the user specifies\n"
      "a smaller value, the software will exit with a complaint.\n"
      "\n"
      "(Strictly, if the input is PS, this restriction is not correct, as\n"
      "one PS packet generates multiple TS packets, and thus lower values\n"
      "for -max could be allowed. For simplicity, this is not considered.)\n"
      "",
      TSPLAY_REPORT_EVERY);
}

static void print_help_help()
{
  print_msg(
    "With no switches, tsplay will give a brief summary of its basic usage.\n"
    "Otherwise:\n"
    "\n"
    "  -help detail[s]   Show an expanded version of the help you get if you\n"
    "                    run tsplay with no arguments\n"
    "  -help ts          Show help specific to playing TS data\n"
    "  -help ps          Show help specific to playing PS data\n"
    "  -help tuning      Show help about how to tune the program's operation\n"
    "  -help test[ing]   Show help on switches that can be useful in testing\n"
    "                    the target application (the video player)\n"
    "  -help debug[ging] Show help on debugging this application.\n"
    "  -help             Show this message\n"
    "  -help all         Show all available help (equivalent to each of the\n"
    "                    above specific helps, in order)\n"
    );
}

static void print_help_ts()
{
  print_msg(
    "Transport Stream Switches:\n"
    "The following switches are only applicable if the input data is TS.\n"
    "\n"
    "  -ignore <pid>     Any TS packets with this PID will not be output\n"
    "                    (more accurately, any TS packets with this PID and with\n"
    "                    PCR information will be transmitted with PID 0x1FFF, and\n"
    "                    any other packets with this PID will be ignored).\n"
    "                    The <pid> may not be 0, and should not be the PAT or PMT.\n"
    "\n"
    "Normally, the TS reading process remembers the last PCR, and also scans ahead\n"
    "for the next PCR. This allows it to calculate an accurate timestamp for each TS\n"
    "packet, at the cost of ignoring any packets before the first PCR, and also\n"
    "(at least in the currrent implementation), ignoring any TS packets after the\n"
    "last PCR, and also at the cost of some extra copying of data.\n"
    "\n"
    "However, sometimes it is more appropriate to revert to the mechanism used by\n"
    "earlier versions of tsplay:\n"
    "\n"
    "  -oldpace          Switch off scanning ahead for the next PCR.\n"
    "  -pace-pcr1        v1 of the PCR scan code\n"
    "  -pace-pcr2-ts     v2 of the PCR scan code - use 1st PCR PID found [default]\n"
    "  -pace-pcr2-pmt    v2 of the PCR scan code - get PCR PID from PMT\n"
    "\n"
    "which attempts to predict an approximate PCR for each TS packet, based on an\n"
    "initial speed (see '-bitrate'/'-byterate' in '-help tuning') and the PCRs found\n"
    "earlier in the data stream. This works reasonably well for streams with a\n"
    "constant bitrate, but does not cope well if the bitrate varies greatly.\n"
    "\n"
    "Note that '-nopcrs' (see '-help tuning') also implies '-oldpace'.\n"
    "\n"
    "In order to buffer PCRs, the first PCR must be found. Normally this is done\n"
    "by finding the first PAT/PMT, and reading the PCR PID from there. However,\n"
    "sometimes it is useful to *tell* the program where to look for its first PCR:\n"
    "\n"
    "  -forcepcr <pid>   Specifies which PID to look for to find the first PCR.\n"
    "\n"
    "Note that after the first PCR is read, *all* TS packets are inspected for\n"
    "PCRs, irrespective of PID.\n"
    );
}

static void print_help_ps()
{
  print_msg(
    "Program Stream Switches:\n"
    "The following switches are only applicable if the input data is PS.\n"
    "\n"
    "If input is from a file, then the program will look at the start of\n"
    "the file to determine if the stream is H.264 or H.262 data. This\n"
    "process may occasionally come to the wrong conclusion, in which case\n"
    "the user can override the choice:\n"
    "\n"
    "  -h264, -avc       Force the program to treat the input as MPEG-4/AVC.\n"
    "  -h262             Force the program to treat the input as MPEG-2.\n"
    "\n"
    "Program stream data from a DVD stores its audio differently\n"
    "(specifically, the DVD specification states that AC-3 (Dolby), DTS and\n"
    "other audio are stored in a specialised manner in private_stream_1):\n"
    "\n"
    "  -dvd              The PS data is from a DVD. This is the default.\n"
    "                    This switch has no effect on MPEG-1 PS data.\n"
    "  -notdvd, -nodvd   The PS data is not from a DVD.\n"
    "\n"
    "Specifying which audio/video streams to read:\n"
    "\n"
    "  -vstream <n>      Take video from video stream <n> (0..7).\n"
    "                    The default is the first video stream found.\n"
    "  -astream <n>      Take audio from audio stream <n> (0..31).\n"
    "                    The default is the first audio stream found\n"
    "                    (this includes private_stream_1 on non-DVD streams).\n"
    "  -ac3stream <n>    Take audio from AC3 substream <n> (0..7), from\n"
    "                    private_stream_1. This implies -dvd.\n"
    "                    (If audio is being taken from a substream, the user\n"
    "                    is assumed to have determined which one is wanted,\n"
    "                    e.g., using psreport)\n"
    "\n"
    "The input PS data does not have PAT/PMT or PID values, and thus tsplay\n"
    "must invent them. The following switches may be used to choose particular\n"
    "PID values:\n"
    "\n"
    "  -vpid <pid>       <pid> is the PID to output video data with.\n"
    "                    Use '-vpid 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x68.\n"
    "  -apid <pid>       <pid> is the PID to output audio data with.\n"
    "                    Use '-apid 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x67.\n"
    "  -pmt <pid>        <pid> is the PID to output PMT data with.\n"
    "                    Use '-pmt 0x<pid>' to specify a hex value.\n"
    "                    Defaults to 0x66.\n"
    "\n"
    "The default values for the various PIDs should suffice in most cases\n"
    "\n"
    "If the audio stream being output is Dolby (AC-3), then the stream type\n"
    "used to output it differs for DVB (European) and ATSC (USA) data. It\n"
    "may be specified as follows:\n"
    "\n"
    "  -dolby dvb       Use stream type 0x06 (the default)\n"
    "  -dolby atsc      Use stream type 0x81\n"
    "\n"
    "Finally, it is occasionally useful to tweak how often PAT/PMT are written,\n"
    "and how much padding the stream starts with:\n"
    "\n"
    "  -prepeat <n>      Output the program data (PAT/PMT) after every <n>\n"
    "                    PS packs, to allow a TS reader to resynchronise\n"
    "                    if it starts reading part way through the stream.\n"
    "                    Defaults to 100.\n"
    "  -pad <n>          Pad the start of the output stream with <n> filler\n"
    "                    TS packets, to allow a TS reader time to byte align\n"
    "                    with the datastream before any significant data\n"
    "                    occurs. Defaults to 8.\n"
    "\n"
    );
}

static void print_help_tuning()
{
  tswrite_help_tuning();
}

static void print_help_testing()
{
  tswrite_help_testing();
  print_msg(
    "\n"
    "  -drop <k> <d>     As TS packets are output, for every <k>+<d> packets,\n"
    "                    keep <k> and then drop (throw away) <d>.\n"
    "                    This can be useful when testing other applications.\n"
    );
}

static void print_help_debugging()
{
  tswrite_help_debug();
}

int main(int argc, char **argv)
{
  TS_writer_p       tswriter;
  struct TS_context context;
  char  *input_name = NULL;
  int    had_input_name = FALSE;
  int    had_output_name = FALSE;
  int    input   = -1;
  int    max     = 0;     // The maximum number of TS packets to read (or 0)
  int    quiet   = FALSE;
  int    verbose = FALSE;
  int    err = 0;
  int    ii = 1;
  int    loop = FALSE;
  time_t start,end;
  int    is_TS;   // Does it appear to be TS or PS?

  // Values relevent to "opening" the output file/socket
  enum  TS_writer_type  how = TS_W_UNDEFINED;  // how to output our TS data
  char                 *output_name = NULL;    // the output filename/host
  int                   port = 88;             // the port to connect to
  char *multicast_if = NULL;                   // IP address of multicast i/f

  tsplay_output_pace_mode pace_mode = TSPLAY_OUTPUT_PACE_PCR2_TS;

  uint32_t  pid_to_ignore = 0;
  uint32_t  override_pcr_pid = 0;  // 0 means "use the PCR found in the PMT"

  // Program Stream specific options
  uint32_t pmt_pid = 0x66;
  uint32_t video_pid = 0x68;
  uint32_t pcr_pid = video_pid;  // Use PCRs from the video stream
  uint32_t audio_pid = 0x67;
  int     repeat_program_every = 100;
  int     pad_start = 8;
  int     input_is_dvd = TRUE;

  int     video_stream = -1;
  int     audio_stream = -1;
  int     want_ac3_audio = FALSE;

  int     want_h262 = TRUE;
  int     force_stream_type = FALSE;
  int     want_dolby_as_dvb = TRUE;
  int     drop_packets = 0;
  int     drop_number  = 0;

  if (argc < 2)
  {
    print_usage(TRUE);
    return 0;
  }

  // Process the standard tswrite switches/arguments
  err = tswrite_process_args("tsplay",argc,argv,&context);
  if (err) return 1;

  // And process any remaining arguments...
  while (ii < argc)
  {
    // Ignore any arguments that have already been "eaten"
    if (!strcmp(argv[ii],TSWRITE_PROCESSED))
    {
      ii++;
      continue;
    }

    if (argv[ii][0] == '-')
    {
      if (!strcmp("--help",argv[ii]) || !strcmp("-h",argv[ii]) ||
          !strcmp("-help",argv[ii]))
      {
        if (argc == (ii+1))
          print_help_help();
        else
        {
          if (!strcmp(argv[ii+1],"ps"))
            print_help_ps();
          else if (!strcmp(argv[ii+1],"ts"))
            print_help_ts();
          else if (!strcmp(argv[ii+1],"tuning"))
            print_help_tuning();
          else if (!strcmp(argv[ii+1],"test") || !strcmp(argv[ii+1],"testing"))
            print_help_testing();
          else if (!strcmp(argv[ii+1],"debug") || !strcmp(argv[ii+1],"debugging"))
            print_help_debugging();
          else if (!strcmp(argv[ii+1],"detail") || !strcmp(argv[ii+1],"details"))
            print_usage(FALSE);
          else if (!strcmp(argv[ii+1],"all"))
          {
            print_usage(FALSE);
            print_msg("\n");
            print_help_ts();
            print_msg("\n");
            print_help_ps();
            print_msg("\n");
            print_help_tuning();
            print_msg("\n");
            print_help_testing();
            print_msg("\n");
            print_help_debugging();
            print_msg("\n");
            print_help_help();
          }
          else
          {
            fprint_err("### tsplay: "
                       "Unrecognised command line switch '%s %s' -- try '-help'\n",
                       argv[ii],argv[ii+1]);
            return 1;
          }
        }
        return 0;
      }
      else if (!strcmp("-quiet",argv[ii]) || !strcmp("-q",argv[ii]))
      {
        quiet = TRUE;
        verbose = FALSE;
      }
      else if (!strcmp("-verbose",argv[ii]) || !strcmp("-v",argv[ii]))
      {
        quiet = FALSE;
        verbose = TRUE;
      }
      else if (!strcmp("-output",argv[ii]) || !strcmp("-o",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        had_output_name = TRUE;
        how = TS_W_FILE;
        output_name = argv[ii+1];
        ii++;
      }
      else if (!strcmp("-mcastif",argv[ii]) || !strcmp("-i",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        multicast_if = argv[ii+1];
        ii++;
      }
      else if (!strcmp("-stdout",argv[ii]))
      {
        had_output_name = TRUE;  // more or less
        how = TS_W_STDOUT;
        output_name = NULL;
        redirect_output_stderr();
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        had_input_name = TRUE;  // more or less
        input_name = NULL;
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### tsplay: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-tcp",argv[ii]))
      {
        if (how == TS_W_STDOUT || how == TS_W_FILE)
        {
          print_err("### tsplay: -tcp does not make sense with file output\n");
          return 1;
        }
        how = TS_W_TCP;
      }
      else if (!strcmp("-udp",argv[ii]))
      {
        if (how == TS_W_STDOUT || how == TS_W_FILE)
        {
          print_err("### tsplay: -udp does not make sense with file output\n");
          return 1;
        }
        how = TS_W_UDP;
      }
      else if (!strcmp("-max",argv[ii]) || !strcmp("-m",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,10,&max);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-oldpace",argv[ii]))
      {
        pace_mode = TSPLAY_OUTPUT_PACE_FIXED;
      }
      else if (!strcmp("-pace-pcr1",argv[ii]))
      {
        pace_mode = TSPLAY_OUTPUT_PACE_PCR1;
      }
      else if (!strcmp("-pace-pcr2-ts",argv[ii]))
      {
        pace_mode = TSPLAY_OUTPUT_PACE_PCR2_TS;
      }
      else if (!strcmp("-pace-pcr2-pmt",argv[ii]))
      {
        pace_mode = TSPLAY_OUTPUT_PACE_PCR2_PMT;
      }
      else if (!strcmp("-forcepcr",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&override_pcr_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-loop",argv[ii]))
      {
        loop = TRUE;
      }
      else if (!strcmp("-avc",argv[ii]) || !strcmp("-h264",argv[ii]))
      {
        force_stream_type = TRUE;
        want_h262 = FALSE;
      }
      else if (!strcmp("-h262",argv[ii]))
      {
        force_stream_type = TRUE;
        want_h262 = TRUE;
      }
      else if (!strcmp("-dvd",argv[ii]))
      {
        input_is_dvd = TRUE;
      }
      else if (!strcmp("-notdvd",argv[ii]) || !strcmp("-nodvd",argv[ii]))
      {
        input_is_dvd = FALSE;
      }
      else if (!strcmp("-vstream",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value_in_range("ps2ts",argv[ii],argv[ii+1],0,0xF,0,
                                 &video_stream);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-astream",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value_in_range("ps2ts",argv[ii],argv[ii+1],0,0x1F,0,
                                 &audio_stream);
        if (err) return 1;
        want_ac3_audio = FALSE;
        ii++;
      }
      else if (!strcmp("-ac3stream",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value_in_range("ps2ts",argv[ii],argv[ii+1],0,0x7,0,
                                 &audio_stream);
        if (err) return 1;
        want_ac3_audio = TRUE;
        input_is_dvd = TRUE;
        ii++;
      }
      else if (!strcmp("-dolby",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        if (!strcmp("dvb",argv[ii+1]))
          want_dolby_as_dvb = TRUE;
        else if (!strcmp("atsc",argv[ii+1]))
          want_dolby_as_dvb = FALSE;
        else
        {
          print_err("### tsplay: -dolby must be followed by dvb or atsc\n");
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-prepeat",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,10,
                        &repeat_program_every);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pad",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,10,&pad_start);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-ignore",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&pid_to_ignore);
        if (err) return 1;
        if (pid_to_ignore == 0)
        {
          print_err("### tsplay: -ignore 0 is not allowed\n");
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-vpid",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&video_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-apid",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&audio_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pmt",argv[ii]))
      {
        CHECKARG("tsplay",ii);
        err = unsigned_value("tsplay",argv[ii],argv[ii+1],0,&pmt_pid);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-drop",argv[ii]))
      {
        if (ii+2 >= argc)
        {
          print_err("### tsplay: -drop requires two arguments\n");
          return 1;
        }
        err = int_value("tsplay",argv[ii],argv[ii+1],TRUE,0,&drop_packets);
        if (err) return 1;
        err = int_value("tsplay",argv[ii],argv[ii+2],TRUE,0,&drop_number);
        if (err) return 1;
        ii += 2;
      }
      else
      {
        fprint_err("### tsplay: "
                   "Unrecognised command line switch '%s' -- try '-help'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (!had_input_name)
      {
        input_name = argv[ii];
        had_input_name = TRUE;
      }
      else if (!had_output_name)
      {
        // This is presumably the host to write to
        err = host_value("tsplay",NULL,argv[ii],&output_name,&port);
        if (err) return 1;
        had_output_name = TRUE;
        if (how == TS_W_UNDEFINED)
          how = TS_W_UDP;
      }
      else
      {
        fprint_err("### tsplay: Unexpected '%s'\n",argv[ii]);
        return 1;
      }
    }
    ii++;
  }

  if (!had_input_name)
  {
    print_err("### tsplay: No input file specified\n");
    return 1;
  }

  // We *need* some output...
  if (!had_output_name)
  {
    print_err("### tsplay: No output file or host specified\n");
    return 1;
  }

  // On the other hand, it can be nice to have a *string* for <stdout>
  if (how == TS_W_STDOUT)
    output_name = "<stdout>";

  // Try to stop extraneous data ending up in our output stream
  if (how == TS_W_STDOUT)
  {
    verbose = FALSE;
    quiet = TRUE;
  }

  // This is an important check
  if (max > 0 && how == TS_W_UDP && (max / 7) < context.circ_buf_size)
  {
    fprint_err("### tsplay: -max %d cannot work with -buffer %d"
               " - max must be at least %d",max,
               context.circ_buf_size,context.circ_buf_size*7);
    if (max/7 > 0)
      fprint_err(",\n            or buffer size reduced to %d",max/7);
    print_err("\n");
    return 1;
  }

  // If tswrite found '-nopcrs' in the switches, make sure that we've
  // switched PCR lookahead off.
  if (context.pcr_mode == TSWRITE_PCR_MODE_NONE)
    pace_mode = TSPLAY_OUTPUT_PACE_FIXED;
  else if (pace_mode == TSPLAY_OUTPUT_PACE_PCR1)
    context.pcr_mode = TSWRITE_PCR_MODE_PCR1;

  if (input_name)
  {
    input = open_binary_file(input_name,FALSE);
    if (input == -1)
    {
      fprint_err("### tsplay: Unable to open input file %s\n",input_name);
      return 1;
    }

    err = determine_if_TS_file(input,&is_TS);
    if (err)
    {
      fprint_err("### tsplay: Cannot play file %s\n",output_name);
      (void) close_file(input);
      return 1;
    }
  }
  else
  {
    input_name = "<stdin>";
    input = STDIN_FILENO;
    is_TS = TRUE;                       // an assertion
  }
  if (!quiet)
    fprint_msg("Reading from  %s%s\n",input_name,(loop?" (and looping)":""));

  err = tswrite_open(how,output_name,multicast_if,port,quiet,
                     &tswriter);
  if (err)
  {
    fprint_err("### tsplay: Cannot open/connect to %s\n",output_name);
    (void) close_file(input);
    return 1;
  }

  if (!quiet)
  {
    if (is_TS)
    {
      print_msg("Input appears to be Transport Stream\n");
      if (pace_mode != TSPLAY_OUTPUT_PACE_FIXED)
        print_msg("Using 'exact' TS packet timing (by looking-ahead to the next PCR)\n");
      else
        print_msg("Approximating/predicting intermediate PCRs\n");
      if (pid_to_ignore)
        fprint_msg("Ignoring PID %04x (%d)\n",pid_to_ignore,pid_to_ignore);
    }
    else
    {
      print_msg("Input appears to be Program Stream\n");
      if (input_is_dvd)
        print_msg("Treating input as from DVD\n");
      else
        print_msg("Treating input as NOT from DVD\n");
    }
    if (max)
      fprint_msg("Stopping after at most %d packets\n",max);

    if (how == TS_W_UDP)
      tswrite_report_args(&context);
  }

  if (drop_packets)
  {
    if (!quiet)
      fprint_msg("DROPPING: Keeping %d TS packet%s, then dropping (throwing away) %d\n",
                 drop_packets,(drop_packets==1?"":"s"),drop_number);
    tswriter->drop_packets = drop_packets;
    tswriter->drop_number = drop_number;
  }

  if (!quiet)
    start = time(NULL);

  // We can only use buffered output for TCP/IP and UDP
  // (it doesn't make much sense for output to a file)
  if (how == TS_W_UDP)
  {
    err = tswrite_start_buffering_from_context(tswriter,&context);
    if (err)
    {
      print_err("### tsplay: Error setting up buffering\n");
      (void) close_file(input);
      (void) tswrite_close(tswriter,TRUE);
      return 1;
    }
  }

  if (is_TS)
  {
    err = play_TS_stream(input,tswriter,pace_mode,pid_to_ignore,
                         override_pcr_pid,max,loop,quiet,verbose);
  }
  else
    err = play_PS_stream(input,tswriter,pad_start,
                         repeat_program_every,force_stream_type,
                         want_h262,input_is_dvd,
                         video_stream,audio_stream,want_ac3_audio,
                         want_dolby_as_dvb,pmt_pid,pcr_pid,
                         video_pid,TRUE,audio_pid,max,loop,
                         verbose,quiet);
  if (err)
  {
    print_err("### tsplay: Error playing stream\n");
    (void) close_file(input);
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }

  if (!quiet)
  {
    end = time(NULL);
    fprint_msg("Started  output at %s",ctime(&start));
    fprint_msg("Finished output at %s",ctime(&end));
    fprint_msg("Elapsed time %.1fs\n",difftime(end,start));
  }
  
  err = close_file(input);
  if (err)
  {
    fprint_err("### tsplay: Error closing input file %s\n",input_name);
    (void) tswrite_close(tswriter,TRUE);
    return 1;
  }
  err = tswrite_close(tswriter,quiet);
  if (err)
  {
    fprint_err("### tsplay: Error closing output to %s\n",output_name);
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

/*
 * Extract and dump the contents of a DVB subtitle stream within a TS
 * Reference standard: ETSI EN 300 743 v1.3.1 (2006-11)
 * 
 * This is still a work in progress, the dump isn't comprehensive and error
 * detection is minimal
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
#include "fmtx.h"

// A three-way choice for what to output by PID
enum pid_extract
{
  EXTRACT_UNDEFINED,
  EXTRACT_TS,  // Output the first "named" video stream
  EXTRACT_PID,    // Output an explicit PID
};
typedef enum pid_extract EXTRACT;

typedef struct dvbdata_s
{
  int found;
  int pts_valid;
  int dts_valid;
  unsigned int data_len;
  uint64_t pts;
  uint64_t last_pts;
  uint64_t dts;
  uint8_t data[0x10000];
} dvbdata_t;

static dvbdata_t dvbd = {0};

static int tfmt = FMTX_TS_DISPLAY_90kHz_RAW;

#define PROGNAME "tsdvbsub"

static inline unsigned int mem16be(const uint8_t * p)
{
  return (p[0] << 8) | p[1];
}

static const uint8_t * page_composition_segment(dvbdata_t * const dvbd, const uint8_t * p, int segment_length)
{
  const uint8_t * const eos = p + segment_length;
  const char * state_text[] = {
    "normal", "acquisition point", "mode change", "reserved"
  };
  int page_state;
  fprint_msg("\npage_composition_segment\n");
  fprint_msg("page_time_out: %d\n", *p++);
  fprint_msg("page_version_number: %d\n", p[0] >> 4);
  page_state = (p[0] >> 2) & 3;
  fprint_msg("page_state: %d (%s)\n", page_state, state_text[page_state]);
  fprint_msg("reserved: %#x\n", p[0] & 3);
  ++p;
  while (p < eos)
  {
    fprint_msg("region_id: %d\n", *p++);
    fprint_msg("reserved: %#x\n", *p++);
    fprint_msg("region_horizontal_address: %d\n", mem16be(p));
    p += 2;
    fprint_msg("region_vertical_address: %d\n", mem16be(p));
    p += 2;
  }
  return p;
}

static const uint8_t * region_composition_segment(dvbdata_t * const dvbd, const uint8_t * p, int segment_length)
{
  const uint8_t * const eos = p + segment_length;
  int region_fill_flag;

  fprint_msg("\nregion_composition_segment\n");
  fprint_msg("region_id: %d\n", *p++);
  fprint_msg("region_version_number: %d\n", p[0] >> 4);
  region_fill_flag = (p[0] >> 3) & 1;
  fprint_msg("region_fill_flag: %d\n", region_fill_flag);
  fprint_msg("reserved: %#x\n", p[0] & 7);
  ++p;
  fprint_msg("region_width: %d\n", mem16be(p));
  p += 2;
  fprint_msg("region_height: %d\n", mem16be(p));
  p += 2;
  fprint_msg("region_level_of_complexity: %d\n", p[0] >> 5);
  fprint_msg("region_depth: %d\n", (p[0] >> 2) & 7);
  fprint_msg("reserved: %#x\n", p[0] & 3);
  ++p;
  fprint_msg("CLUT_id: %d\n", *p++);
  fprint_msg("region_8-bit_pixel_code: %d\n", *p++);
  fprint_msg("region_4-bit_pixel_code: %d\n", p[0] >> 4);
  fprint_msg("region_2-bit_pixel_code: %d\n", (p[0] >> 2) & 3);
  fprint_msg("reserved: %#x\n", p[0] & 3);
  ++p;

  while (p < eos)
  {
    int object_type;
    fprint_msg("object_id: %d\n", mem16be(p));
    p += 2;
    fprint_msg("object_type: %d\n", object_type = (p[0] >> 6));
    fprint_msg("object_provider_flag: %d\n", (p[0] >> 4) & 3);
    fprint_msg("object_horizontal_position: %d\n", mem16be(p) & 0xfff);
    p += 2;
    fprint_msg("reserved: %#x\n", p[0] >> 4);
    fprint_msg("object_vertical_position: %d\n", mem16be(p) & 0xfff);
    p += 2;
    if (object_type == 1 || object_type == 2)
    {
      fprint_msg("foreground_pixel_code: %d\n", *p++);
      fprint_msg("background_pixel_code: %d\n", *p++);
    }
  }
  return p;
}

static const uint8_t * CLUT_definition_segment(dvbdata_t * const dvbd, const uint8_t * p, int segment_length)
{
  const uint8_t * const eos = p + segment_length;

  fprint_msg("\nCLUT definition_segment\n");
  fprint_msg("CLUT_id: %d\n", *p++);
  fprint_msg("CLUT_version_number: %d\n", p[0] >> 4);
  fprint_msg("reserved: %#x\n", p[0] & 0xf);
  ++p;

  while (p < eos)
  {
    int full_range_flag;
    fprint_msg("CLUT_entry_id: %d\n", *p++);

    fprint_msg("2-bit/entry_CLUT_flag: %d\n", p[0] >> 7);
    fprint_msg("4-bit/entry_CLUT_flag: %d\n", (p[0] >> 6) & 1);
    fprint_msg("8-bit/entry_CLUT_flag: %d\n", (p[0] >> 5) & 1);
    fprint_msg("reserved: %#x\n", (p[0] >> 1) & 0xf);
    fprint_msg("full_range_flag: %#x\n", full_range_flag = (p[0] & 1));
    ++p;

    if (full_range_flag == 1)
    {
      fprint_msg("Y-value: %d\n", *p++);
      fprint_msg("Cr-value: %d\n", *p++);
      fprint_msg("Cb-value: %d\n", *p++);
      fprint_msg("T-value: %d\n", *p++);
    }
    else
    {
      fprint_msg("Y-value: %d\n", p[0] >> 2);
      fprint_msg("Cr-value: %d\n", ((p[0] & 3) << 2) | ((p[1] >> 6) & 3));
      fprint_msg("Cb-value: %d\n", (p[1] >> 2) & 0xf);
      fprint_msg("T-value: %d\n", p[1] & 3);
      p += 2;
    }
  }
  return p;
}

static const uint8_t * object_data_segment(dvbdata_t * const dvbd, const uint8_t * p, int segment_length)
{
  const uint8_t * const sos = p;
  const uint8_t * const eos = p + segment_length;
  int object_coding_method;

  fprint_msg("\nobject_data_segment\n");
  fprint_msg("object_id: %d\n", mem16be(p));
  p += 2;
  fprint_msg("object_version_number: %d\n", p[0] >> 4);
  fprint_msg("object_coding_method: %d\n", object_coding_method = ((p[0] >> 2) & 3));
  fprint_msg("non_modifying_colour_flag: %d\n", (p[0] >> 1) & 1);
  fprint_msg("reserved: %#x\n", p[0] & 0x1);
  ++p;

  switch (object_coding_method)
  {
    case 0:
    {
      unsigned int top_field_data_block_length;
      unsigned int bottom_field_data_block_length;
      fprint_msg("top_field_data_block_length: %d\n", top_field_data_block_length = mem16be(p));
      p += 2;
      fprint_msg("bottom_field_data_block_length: %d\n", bottom_field_data_block_length = mem16be(p));
      p += 2;
      print_data(TRUE, "top pixel-data:", p, top_field_data_block_length, 0x10000);
      p += top_field_data_block_length;
      print_data(TRUE, "bottom pixel-data:", p, bottom_field_data_block_length, 0x10000);
      p += bottom_field_data_block_length;
      if (((p - sos) & 1) != 0)
      {
        fprint_msg("8_stuff_bits: %d\n", *p++);
      }
      break;
    }

    case 1:
    {
      unsigned int number_of_codes;
      unsigned int i;
      fprint_msg("number_of_codes: %d\n", number_of_codes = *p++);
      p += 2;
      for (i = 0; i != number_of_codes; ++i)
      {
        fprint_msg("character_code: %d\n", mem16be(p));
        p += 2;
      }
      break;
    }

    default:
      print_data(TRUE, "reserved:", p, eos - p, 0x10000);
      break;
  }

  return p;
}


static const uint8_t * subtitling_segment(dvbdata_t * const dvbd, const uint8_t * p)
{
  unsigned int segment_type;
  unsigned int segment_length;
  const uint8_t * p2;

  fprint_msg("\nsubtitling_segment\n");
  fprint_msg("sync_byte: %#x\n", *p++);
  fprint_msg("segment_type: %#x\n", segment_type = *p++);
  fprint_msg("page_id: %d\n", (p[0] << 8) | p[1]);
  p += 2;
  fprint_msg("segment_length: %d\n", segment_length = ((p[0] << 8) | p[1]));
  p += 2;
  switch (segment_type)
  {
    case 0x10:
      p2 = page_composition_segment(dvbd, p, segment_length);
      break;

    case 0x11:
      p2 = region_composition_segment(dvbd, p, segment_length);
      break;

    case 0x12:
      p2 = CLUT_definition_segment(dvbd, p, segment_length);
      break;

    case 0x13:
      p2 = object_data_segment(dvbd, p, segment_length);
      break;

    default:
      print_data(TRUE, "data", p, segment_length, segment_length);
      p2 = p + segment_length;
      break;
  }

  // Always believe seg length in case of early return
  p += segment_length;

  if (p != p2)
  {
    fprint_msg("### parse length mismatch\n");
  }

  return p;
}

static void flush_dvbd(dvbdata_t * const dvbd)
{
  const uint8_t * p = dvbd->data;

  if (!dvbd->found)
    return;

  fprint_msg("\nPTS: %s, DTS: %s, PTS - last_PTS: %s\n",
    !dvbd->pts_valid ? "none" : fmtx_timestamp(dvbd->pts, tfmt),
    !dvbd->dts_valid ? "none" : fmtx_timestamp(dvbd->dts, tfmt),
    !dvbd->pts_valid ? "????" : fmtx_timestamp(dvbd->pts - dvbd->last_pts, tfmt));
  if (dvbd->pts_valid)
    dvbd->last_pts = dvbd->pts;

  fprint_msg("data length: %d\n\n", dvbd->data_len);

  fprint_msg("data_identifier: %#x\n", *p++);
  fprint_msg("subtitle_stream_id: %d\n", *p++);

  while (*p == 0xf)
  {
    p = subtitling_segment(dvbd, p);
  }

  fprint_msg("end_of_PES_data_field_marker: %#x\n", *p++);

  if (dvbd->data_len > (unsigned int)(p - dvbd->data))
  {
    print_data(TRUE, "excess bytes", p, dvbd->data_len - (p - dvbd->data), 0x10000);
  }
  else if (dvbd->data_len < (unsigned int)(p - dvbd->data))
  {
    fprint_msg("### overrun\n");
  }

  dvbd->data_len = 0;
  dvbd->pts_valid = FALSE;
  dvbd->dts_valid = FALSE;
  dvbd->found = FALSE;
  memset(dvbd->data, 0, sizeof(dvbd->data));
}

static void add_data_dvbd(dvbdata_t * const dvbd, const uint8_t * const data, unsigned int len)
{
  unsigned int gap = sizeof(dvbd->data) - dvbd->data_len;

  if (len == 0)
    return;

  if (gap < len)
  {
    fprint_err("### Data buffer overflow\n");
    len = gap;
  }
  memcpy(dvbd->data + dvbd->data_len, data, len);
  dvbd->data_len += len;
}


/*
 * Extract all the TS packets for a nominated PID to another file.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_pid_packets(TS_reader_p  tsreader,
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
      int pes_overflow = 0;

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

        flush_dvbd(&dvbd);

        err = find_PTS_DTS_in_PES(payload,payload_len,
                                  &dvbd.pts_valid, &dvbd.pts, &dvbd.dts_valid, &dvbd.dts);
        dvbd.found = TRUE;

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
      }

      if (got_pes_packet_len)
      {
        // Try not to write more data than the PES packet declares
        if (data_len > pes_packet_len)
        {
          pes_overflow = data_len - pes_packet_len;
          data_len = pes_packet_len;
          pes_packet_len = 0;
        }
        else
          pes_packet_len -= data_len;
      }

      add_data_dvbd(&dvbd, data, data_len);
      if (got_pes_packet_len && pes_packet_len == 0)
      {
        flush_dvbd(&dvbd);
      }

      if (pes_overflow)
      {
        print_data(TRUE, "Data after PES", data + data_len, pes_overflow, 1000);
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
                      const int prog_no,
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

    err = find_pmt(tsreader,prog_no,max_to_read,verbose,quiet,&num_read,&pmt);
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
      if (pmt->streams[ii].stream_type == 6 &&
          pmt->streams[ii].ES_info_length > 0 &&
          pmt->streams[ii].ES_info[0] == 0x59)
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
    fprint_err("### No DVB subtitle stream specified in first %d TS packets in input file\n",
               max);
    free_TS_reader(&tsreader);
    return 1;
  }

  if (!quiet)
    fprint_msg("Extracting DVB Subtitles PID %04x (%d)\n", pid,pid);

  // Amend max to take account of the packets we've already read
  max -= total_num_read;

  // And do the extraction.
  err = extract_pid_packets(tsreader,pid,max,verbose,quiet);
  free_TS_reader(&tsreader);
  return err;
}

/*
 * Extract all the TS packets for a nominated PID to another file.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int extract_pid(int          input,
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

  err = extract_pid_packets(tsreader,pid_wanted,max,verbose,quiet);

  free_TS_reader(&tsreader);
  return err;
}

static void print_usage()
{
  print_msg(
    "Usage: " PROGNAME " [switches] <infile>\n"
    "\n"
    );
  REPORT_VERSION(PROGNAME);
  print_msg(
    "\n"
    "  Parse & dump the contents of a single DVB subtitling stream from a\n"
    "  Transport Stream\n"
    "  (or Program Stream).\n"
    "\n"
    "Files:\n"
    "  <infile>  is an H.222 Transport Stream file (but see -stdin and -pes)\n"
    "\n"
    "Which stream to extract:\n"
    "  -pid <pid>         Output data for the stream with the given\n"
    "                     <pid>. Use -pid 0x<pid> to specify a hex value\n"
    "  [default]          The stream will be located from the PMT info\n"
    "  -prog <n>          Program number [default=1]\n"
    "\n"
    "General switches:\n"
    "  -err stdout        Write error messages to standard output (the default)\n"
    "  -err stderr        Write error messages to standard error (Unix traditional)\n"
    "  -stdin             Input from standard input, instead of a file\n"
    "  -verbose, -v       Output informational/diagnostic messages\n"
    "  -quiet, -q         Only output error messages\n"
    "  -max <n>, -m <n>   Maximum number of TS packets to read\n"
    );
}

int main(int argc, char **argv)
{
  int    use_stdin = FALSE;
  char  *input_name = NULL;
  int    had_input_name = FALSE;
  EXTRACT extract = EXTRACT_TS;

  int       input   = -1;    // Our input file descriptor
  int       maxts   = 0;     // The maximum number of TS packets to read (or 0)
  uint32_t  pid     = 0;     // The PID of the (single) stream to extract
  int       quiet   = FALSE; // True => be as quiet as possible
  int       verbose = FALSE; // True => output diagnostic/progress messages
  int       prog_no = 1;

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
        CHECKARG(PROGNAME, ii);
        err = int_value(PROGNAME, argv[ii],argv[ii+1],TRUE,10,&maxts);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-pid",argv[ii]))
      {
        CHECKARG(PROGNAME,ii);
        err = unsigned_value(PROGNAME,argv[ii],argv[ii+1],0,&pid);
        if (err) return 1;
        ii++;
        extract = EXTRACT_PID;
      }
      else if (!strcmp("-prog",argv[ii]))
      {
        CHECKARG(PROGNAME,ii);
        err = int_value(PROGNAME, argv[ii],argv[ii+1],TRUE,10,&prog_no);
        if (err) return 1;
        ii++;
      }
      else if (!strcmp("-stdin",argv[ii]))
      {
        use_stdin = TRUE;
        had_input_name = TRUE;  // so to speak
      }
      else if (!strcmp("-err",argv[ii]))
      {
        CHECKARG(PROGNAME,ii);
        if (!strcmp(argv[ii+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[ii+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### " PROGNAME ": "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else if (!strcmp("-tfmt",argv[ii]))
      {
        CHECKARG(PROGNAME,ii);
        if ((tfmt = fmtx_str_to_timestamp_flags(argv[ii + 1])) < 0)
        {
          fprint_msg("### tsreport: Bad timestamp format '%s'\n",argv[ii+1]);
          return 1;
        }
        ii++;
      }
      else
      {
        fprint_err("### " PROGNAME ": "
                   "Unrecognised command line switch '%s'\n",argv[ii]);
        return 1;
      }
    }
    else
    {
      if (had_input_name)
      {
        fprint_err("### " PROGNAME ": Unexpected '%s'\n",argv[ii]);
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
    print_err("### " PROGNAME ": No input file specified\n");
    return 1;
  }

  // ============================================================
  
  if (use_stdin)
    input = STDIN_FILENO;
  else
  {
    input = open_binary_file(input_name,FALSE);
    if (input == -1)
    {
      fprint_err("### " PROGNAME ": Unable to open input file %s\n",input_name);
      return 1;
    }
  }
  if (!quiet)
    fprint_msg("Reading from %s\n",(use_stdin?"<stdin>":input_name));

  if (!quiet)
  {
    if (extract == EXTRACT_PID)
      fprint_msg("Extracting packets for PID %04x (%d)\n",pid,pid);
  }
  
  if (maxts != 0 && !quiet)
    fprint_msg("Stopping after %d TS packets\n",maxts);

  if (extract == EXTRACT_PID)
    err = extract_pid(input,pid,maxts,verbose,quiet);
  else
    err = extract_av(input,prog_no,maxts,verbose,quiet);
  if (err)
  {
    print_err("### " PROGNAME ": Error extracting data\n");
    if (!use_stdin)  (void) close_file(input);
    return 1;
  }

  if (!use_stdin)
  {
    err = close_file(input);
    if (err)
      fprint_err("### " PROGNAME ": Error closing input file %s\n",input_name);
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

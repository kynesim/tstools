/*
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
 *   Gareth Bailey (gb@kynesim.co.uk), Kynesim, Cambridge, UK
 *
 * ***** END LICENSE BLOCK *****
 */

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>

#include "misc_fns.h"
#include "printing_fns.h"
#include "version.h"

#define TS_PACKET_SIZE 188

#define TO_BE16(from,to) *to = (0xFF & (from>>8)); *(to+1) = (0xFF & from); 

static uint8_t *create_out_packet(char *in_data, int in_len, uint16_t pid)
{
  uint8_t *out_packet = malloc(TS_PACKET_SIZE);
  uint8_t *ptr = out_packet;
  uint16_t flags;
  uint16_t flags_pid;

  if (!ptr)
    return NULL;
  if (in_len > (TS_PACKET_SIZE - 4))
    return NULL;

  *ptr = 0x47;
  ptr++;

  /* Transport Error Indicator */
  flags = 0<<15;

  /* Payload Unit Start Indicator */
  flags = flags | 0<<14;

  /* Transport Priority */
  flags = flags | 0<<13;

  flags_pid = flags | pid;
  TO_BE16(flags_pid,ptr);
  ptr+=2;

  *ptr = 0x11;
  ptr++;

  memcpy(ptr,in_data,in_len);
  ptr+=in_len;

  memset(ptr,0xFF,TS_PACKET_SIZE-(ptr-out_packet));
  ptr+=TS_PACKET_SIZE-(ptr-out_packet);

  assert((ptr-TS_PACKET_SIZE) == out_packet);

  {
    int i;
    ptr = out_packet;
    print_msg("Packet to be written is:\n");
    for (i=0;i<TS_PACKET_SIZE;i++)
    {
      if (!(i%16)) print_msg("\n");

      fprint_msg("%02x ",ptr[i]);
    }
    print_msg("\n\n");
  }

  return out_packet;
}


static int insert_packets(int      file,
                   int      out_file,
                   uint8_t *out_packet,
                   int     *packet_numbers,int n_pack)
{
  uint8_t buf[TS_PACKET_SIZE];
  int rv;
  int packets_read=0;
  int bytes_read=0;
  int packnum_i=0;

  while (1)
  {
    rv = read(file, buf, TS_PACKET_SIZE);

    if (rv == 0) break;
    if (rv != TS_PACKET_SIZE) return rv;

    bytes_read+=rv;

    if (packet_numbers[packnum_i]==packets_read && packnum_i<n_pack)
    {
      fprint_msg("Writing new packet before packet %d...\n",packets_read);

      rv = write(out_file,out_packet,TS_PACKET_SIZE);
      assert(rv == TS_PACKET_SIZE);
      packnum_i++;
    }

    rv = write(out_file,buf,rv);

    assert(rv=TS_PACKET_SIZE);

    packets_read++;
  }

  fprint_msg("\nRead a total of %d packets (%d bytes)\n",packets_read,bytes_read);
  return 0;
}

static off_t get_file_size(int file)
{
  struct stat stat_ret;
  fstat(file, &stat_ret);

  return stat_ret.st_size;
}


static int num_char_in_string(char *string,char c)
{
  unsigned int i;
  int a=0;

  for (i=0; i<strlen(string);i++)
  {
    if (c == *(string+i))
      a++;
  }
  return a;
}

static void print_usage()
{
  print_msg(
    "Usage: ts_packet_insert [switches] <infile>\n"
    "\n"
    );
  REPORT_VERSION("ts_packet_insert");
  print_msg(
    "\n"
    "  Insert TS packets into a Transport Stream at positions\n"
    "  specified by the user.\n"
    "\n"
    "Input:\n"
    "  <infile>          An H.222 Transport Stream file.\n"
    "\n"
    "Switches:\n"
    "  -err stdout       Write error messages to standard output (the default)\n"
    "  -err stderr       Write error messages to standard error (Unix traditional)\n"
    "  -p <positions>    This a a colon (':') delimited string of numbers\n"
    "                    between 0 and 1, representing how far through to put \n"
    "                    each TS packet.  E.g., -p 0.1:0.4:0.7:0.9 will insert\n"
    "                    4 packets at 10%%, 40%%, 70%% and 90%% through the file.\n"
    "  -pid <pid>        The inserted packets will have the PID specfied.\n"
    "                    If no PID is specified, then 0x68 will be used.\n"
    "  -s <string>       The inserted packets will contain <string> as their\n"
    "                    payload. This defaults to 'Inserted packet'.\n"
    "  -o <output file>  The new TS file will be written out with the given name\n"
    "                    (which defaults to out.ts)\n"
    "For example:\n"
    "\n"
    "    ts_packet_insert -p 0.3:0.6 -o out.ts -pid 89 -s \"AD=start\" in.ts\n"
    );
}

/* bubble sort */
static void sort_positions(double *in_array,int size)
{
  int sorted=0;

  while (!sorted)
  {
    int i;
    /* asume sorted */
    sorted++;

    for (i=0;i<size-1;i++)
    {
      if (in_array[i]>in_array[i+1])
      {
        double tmp;

        tmp = in_array[i];
        in_array[i] = in_array[i+1];
        in_array[i+1] = tmp;

        /* damn, go round again */
        sorted=0;
      }
    }
  }
}

int main(int argc, char **argv)
{
  char *output_file_path = "out.ts";
  char *in_file_path = NULL;
  long in_file_size=0;
  /*an array of floats for the positions of packets to insert,values of 0-1*/
  double *positions=NULL; 
  int *packet_numbers=NULL;
  int n_pos = 0;

  int argno = 1;
  int arg_counter = 0;

  uint32_t pid=0x68;

  char *out_string="Inserted packet";

  if (argc < 2)
  {
    print_usage();
    return 0;
  }

  while (argno < argc)
  {
    if (argv[argno][0] == '-')
    {
      if (!strcmp("-p",argv[argno]))
      {
        char *endptr;
        char *position_string;
        int pos_index;

        ++argno;

        free(positions);

        n_pos = (num_char_in_string(argv[argno],':')+1);
        positions = malloc(n_pos * sizeof(double));
        if (!positions)
        {
          print_err("malloc failed");
          exit(1);
        }

        position_string = strtok(argv[argno],":");
        pos_index=0;
        print_msg("Adding new packets at:");
        while (1)
        {
          if (!position_string)
            break;

          positions[pos_index] = strtod(position_string,&endptr);

          if (endptr == position_string || positions[pos_index]>1 || positions[pos_index]<0)
          {
            fprint_err("\nNot a valid floating point number for position (argument %d)\n",argno); 
            exit(1);
          }

          fprint_msg("  %d%%",(int)(positions[pos_index]*100));

          position_string = strtok(NULL,":");
          pos_index++;
        }
        print_msg("\n");
        sort_positions(positions,n_pos);
        assert(pos_index == n_pos);

      }
      else if (!strcmp("-err",argv[argno]))
      {
        CHECKARG("ts_packet_insert",argno);
        if (!strcmp(argv[argno+1],"stderr"))
          redirect_output_stderr();
        else if (!strcmp(argv[argno+1],"stdout"))
          redirect_output_stdout();
        else
        {
          fprint_err("### ts_packet_insert: "
                     "Unrecognised option '%s' to -err (not 'stdout' or"
                     " 'stderr')\n",argv[argno+1]);
          return 1;
        }
        argno++;
      }
      else if (!strcmp("-pid",argv[argno]))
      {
        int err;
        CHECKARG("ts_packet_insert",argno);
        err = unsigned_value("ts_packet_insert",argv[argno],argv[argno+1],0,&pid);
        if (err) return 1;
        argno++;
      }
      else if (!strcmp("-o",argv[argno]))
      {
        CHECKARG("ts_packet_insert",argno);
        output_file_path = argv[++argno];
      }
      else if (!strcmp("-s",argv[argno]))
      {
        CHECKARG("ts_packet_insert",argno);
        out_string = argv[++argno];
      }
      else if (!strcmp("-h",argv[argno]) || !strcmp("--help",argv[argno]))
      {
        print_usage();
        return 0;
      }
      else
      {
        fprint_msg("\n *** Unknown option %s, ignoring.\n\n",argv[argno]);
      }
    }
    else
    {
      if (arg_counter == 0)
      {
        in_file_path = argv[argno];
        arg_counter++;
      }
      else
      {
        fprint_err( "### ts_packet_insert: Unexpected '%s'\n", argv[argno]);
        return 1;
      }
    }
    argno++;
  }

  if (!in_file_path)
  {
    print_err("Error: No input file specified.\n");
    exit(1);
  }

  fprint_msg("Reading from file:   %s\n",in_file_path);
  fprint_msg("Writing to file:     %s\n",output_file_path);
  fprint_msg("Inserting packets with PID %#x (%u)\n",pid,pid);
  fprint_msg("Using output string: %s\n",out_string);

  {
    int out_file;
    int in_file = open(in_file_path,O_RDONLY);

    if (in_file<0)
    {
      fprint_err("Error: could not open %s for reading: %s\n",
                 in_file_path,strerror(errno));
      exit(1);
    }

    out_file = open(output_file_path,O_WRONLY | O_TRUNC | O_CREAT,0644);
    if (out_file<0)
    {
      fprint_err("Error: could not open %s for reading: %s\n",
                 output_file_path,strerror(errno));
      exit(1);
    }

    in_file_size = get_file_size(in_file);

    if (in_file_size % TS_PACKET_SIZE)
    {
      print_err("Error: TS file length is not a multiple of 188 bytes\n");
      exit(1);
    }

    {
      int num_pack = in_file_size / TS_PACKET_SIZE;
      int i;
      fprint_msg("\nInput file is %ld bytes long with ",in_file_size);
      fprint_msg("%d TS packets\n",num_pack);

      packet_numbers = malloc(n_pos * sizeof(int));

      /* Find out which packets we insert before */
      for (i=0;i<n_pos;i++)
      {
        packet_numbers[i] = (int)((double)num_pack * positions[i]);
      }


      {
        /* create the packet to spit out */
        uint8_t *out_packet = create_out_packet(out_string,
                                                strlen(out_string)+1,
                                                pid);

        insert_packets(in_file,out_file,out_packet,packet_numbers,n_pos);
      }
    }
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

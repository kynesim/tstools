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

#define TS_PACKET_SIZE 188

#define DBG_INFO(x) printf x

#define TO_BE16(from,to) *to = (0xFF & (from>>8)); *(to+1) = (0xFF & from); 

static uint8_t *create_out_packet(char *in_data,int in_len,uint16_t pid)
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
    DBG_INFO(("Packet to be written is:\n"));
    for (i=0;i<TS_PACKET_SIZE;i++)
    {
      if (!(i%16)) DBG_INFO(("\n"));
  
      DBG_INFO(("%02x ",ptr[i]));
    }
    DBG_INFO(("\n\n"));
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
      DBG_INFO(("Writing new packet before packet %d...\n",packets_read));

      rv = write(out_file,out_packet,TS_PACKET_SIZE);
      assert(rv == TS_PACKET_SIZE);
      packnum_i++;
    }

    rv = write(out_file,buf,rv);

    assert(rv=TS_PACKET_SIZE);

    packets_read++;
  }

  printf("\nRead a total of %d packets (%d bytes)\n",packets_read,bytes_read);
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
  printf("Usage:\n"
         "\tts_packet_insert [switches] <infile> [switches]\n"
         "\n"
        );

  printf("\tInsert TS packets into a Transport Stream at a positions "
         "\n\tspecified by the user.\n\n"
        );

  printf("Input:\n"
         "\t<infile>\t A H.222.0 TS stream.\n\n"
        );

  printf("Switches:\n"
         "\t-p [positions]\t This a a colon (':') delimited string of numbers\n"
         "\t\t\t between 0 and 1, representing how far through to put \n"
         "\t\t\t each ts packet.  E.g.  -p 0.1:0.4:0.7:0.9 will insert\n"
         "\t\t\t 4 packets at 10%%, 40%%, 70%% and 90%% through the file.\n"
         "\n"
        );

  printf("\t-pid [pid]\t The inserted packets will have the pid specfied.\n"
         "\n"
        );

  printf("\t-s [string]\t The inserted packets will contain [string] as it's\n"
         "\t\t\t payload.\n"
         "\n"
        );

  printf("\t-o [output file] The new TS file will be written out to the file\n"
         "\t\t\t specified. (defaults to out.ts)\n"
         "\n"
        );

  printf("\t-h (--help) \t This message."
         "\n\n"
        );

  printf("Example:\n"
         "\tts_packet_insert -p 0.3:0.6 -o out.ts -pid 89 -s \"AD=start\" in.ts"
         "\n\n"
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
  char *output_file_path = "./__out.ts";
  char *in_file_path = NULL;
  long in_file_size=0;
  /*an array of floats for the positions of packets to insert,valuse of 0-1*/
  double *positions=NULL; 
  int *packet_numbers=NULL;
  int n_pos = 0;

  int argno = 1;
  int arg_counter = 0;

  int pid=0;

  char * out_string=NULL;

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
        char * position_string;
        int pos_index;

        ++argno;

        free(positions);

        n_pos = (num_char_in_string(argv[argno],':')+1);
        positions = malloc(n_pos * sizeof(double));
        if (!positions)
        {
          fprintf(stderr,"malloc failed");
          exit(1);
        }

        position_string = strtok(argv[argno],":");
        pos_index=0;
        DBG_INFO(("Adding new packets at\n"));
        while(1)
        {
          if(!position_string)
            break;

          positions[pos_index] = strtod(position_string,&endptr);

          if (endptr == position_string || positions[pos_index]>1 || positions[pos_index]<0)
          {
            fprintf(stderr,"\nNot a valid floating point number for position (argument %d)\n",argno); 
            exit(1);
          }

          DBG_INFO(("\t%d%%\n",(int)(positions[pos_index]*100)));

          position_string = strtok(NULL,":");
          pos_index++;
        }
        DBG_INFO(("\n"));
        sort_positions(positions,n_pos);
        assert(pos_index == n_pos);

      }
      else if (!strcmp("-pid",argv[argno]))
      {
        pid = atoi(argv[++argno]);
        DBG_INFO(("Will insert packets with a pid of 0x%x\n",pid));
      }
      else if (!strcmp("-o",argv[argno]))
      {
        output_file_path = argv[++argno];
      }
      else if (!strcmp("-s",argv[argno]))
      {
        out_string = argv[++argno];
        DBG_INFO(("Output string will be:\t%s\n",out_string));
      }
      else if (!strcmp("-h",argv[argno]) || !strcmp("--help",argv[argno]))
      {
        print_usage();
        return 0;
      }
      else
      {
        DBG_INFO(("\n *** Unknown option %s, ignoring.\n\n",argv[argno]));
      }
    }
    else
    {
      if (arg_counter == 0)
      {
        in_file_path = argv[argno];
        arg_counter++;
      }
    }
    argno++;
  }

  if (!in_file_path)
  {
    fprintf(stderr,"Error: No input file specified.\n");
    exit(1);
  }

  DBG_INFO(("Writing to file: \t%s\n",output_file_path));
  DBG_INFO(("Reading from: \t\t%s\n",in_file_path));

  {
    /* open files*/
    int in_file = open(in_file_path,O_RDONLY);
    int out_file = open(output_file_path,O_WRONLY | O_TRUNC | O_CREAT,0644);

    if (in_file<0)
    {
      fprintf(stderr,"Error: couldn't open %s for reading\n",in_file_path);
      exit(1);
    }

    if (out_file<0)
    {
      fprintf(stderr,"Error: couldn't open %s for reading\n",output_file_path);
      exit(1);
    }

    in_file_size = get_file_size(in_file);

    if (in_file_size % TS_PACKET_SIZE)
    {
      fprintf(stderr,"Error: ts file length is not a multiple of 188 bytes\n");
      exit(1);
    }

    {
      int num_pack = in_file_size / TS_PACKET_SIZE;
      int i;
      DBG_INFO(("\nIn file is %ld bytes long with ",in_file_size));
      DBG_INFO(("%d TS packets\n",num_pack));

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

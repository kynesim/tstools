/* 
 * Report on a pcap (.pcap) file.
 *
 * <rrw@kynesim.co.uk> 2008-09-05
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
 *   Kynesim, Cambridge UK
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif // _WIN32

#include "compat.h"
#include "pcap.h"
#include "ethernet.h"
#include "ipv4.h"
#include "version.h"
#include "misc_fns.h"

typedef struct pcapreport_ctx_struct
{
  int use_stdin;
  char *input_name;
  int had_input_name;
  int dump_data;
  int dump_extra;
  PCAP_reader_p pcreader;
  pcap_hdr_t pcap_hdr;
  char *output_name;
  FILE *output_file;
  uint32_t output_dest_addr;
  uint32_t output_dest_port;
} pcapreport_ctx_t;

static void print_usage()
{
  printf("Usage: pcapreport [switches] [<infile>] [switches]\n"
	 "\n"
	 );
  REPORT_VERSION("pcap");
  printf(
	 "\n"
	 " Report on a pcap capture file.\n"
	 "\n"
	 " -o <output file>         Dump selected UDP payloads to the named output file.\n"
	 " -d <dest ip>:<port>      Select data with the given destination IP and port.\n"
	 " --dump-data | -D         Dump any data in the input file to stdout.\n"
	 " --extra-dump | -e    Dump only data which isn't being sent to the -o file.\n"
	 "\n"
	 " Specifying 0.0.0.0 for destination IP or 0 for destination port will capture all\n"
	 " hosts and ports respectively.\n"
	 "\n"
	 );
}


int main(int argc, char **argv)
{
  int err = 0;
  int ii = 1;
  pcapreport_ctx_t ctx;

  memset(&ctx, '\0', sizeof(pcapreport_ctx_t));

  if (argc < 2)
    {
      print_usage();
      return 0;
    }

  while (ii < argc)
    {
      if (argv[ii][0] == '-')
	{
	  if (!strcmp("--help", argv[ii]) || !strcmp("-h", argv[ii]) ||
	      !strcmp("-help", argv[ii]))
	    {
	      print_usage();
	      return 0;
	    }
	  else if (!strcmp("--o", argv[ii]) || !strcmp("-o", argv[ii]))
	    {
	      ++ii;
	      if (ii < argc)
		{
		  ctx.output_name = argv[ii];
		}
	      else
		{
		  fprintf(stderr, "### pcapreport: -o requires an argument.\n");
		  return 1;
		}
	    }
	  else if (!strcmp("--d", argv[ii]) || !strcmp("-d", argv[ii]))
	    {
	      char *hostname;
	      int port;

	      err = host_value("pcapreport", argv[ii], argv[ii+1], &hostname, &port);
	      if (err) return 1;
	      ++ii;
	      
	      ctx.output_dest_port = port;
	      if (ipv4_string_to_addr(&ctx.output_dest_addr, hostname))
		{
		  fprintf(stderr, "### pcapreport: '%s' is not a host IP address (names are not allowed!)\n",
			  hostname);
		  return 1;
		}
	    }
	  else if (!strcmp("--dump-data", argv[ii]) || !strcmp("-D", argv[ii]))
	    {
	      ++ctx.dump_data;
	    }
	  else if (!strcmp("--extra-dump", argv[ii]) || !strcmp("-E", argv[ii]))
	    {
	      ++ctx.dump_extra;
	    }
	  else
	    {
	      fprintf(stderr, "### pcapreport: "
		      "Unrecognised command line switch '%s'\n", argv[ii]);
	      return 1;
	    }
	}
      else
	{
	  if (ctx.had_input_name)
	    {
	      fprintf(stderr, "### pcapreport: Unexpected '%s'\n", 
		      argv[ii]);
	      return 1;
	    }
	  else
	    {
	      ctx.input_name = argv[ii];
	      ctx.had_input_name = TRUE;
	    }
	}
      ++ii;
    }

  err = pcap_open(&ctx.pcreader, &ctx.pcap_hdr, ctx.input_name);
  if (err)
    {
      fprintf(stderr, 
	      "### pcapreport: Unable to open input file %s for reading "
	      "PCAP (code %d)\n", 
	      ctx.had_input_name ? "<stdin>": ctx.input_name, err);
    }

  if (ctx.output_name)
    {
      printf("pcapreport: Dumping all packets for %s:%d to %s .\n",
	     ipv4_addr_to_string(ctx.output_dest_addr),
	     ctx.output_dest_port,
	     ctx.output_name);
    }

  printf("Capture made by version %u.%u local_tz_correction "
	 "%d sigfigs %u snaplen %d network %u\n", 
	 ctx.pcap_hdr.version_major, ctx.pcap_hdr.version_minor,
	 ctx.pcap_hdr.thiszone,
	 ctx.pcap_hdr.sigfigs,
	 ctx.pcap_hdr.snaplen,
	 ctx.pcap_hdr.network);
  
  if (ctx.pcap_hdr.snaplen < 65535)
    {
      fprintf(stderr,"### pcapreport: WARNING snaplen is %d, not >= 65535 - "
	      "not all data may have been captured.\n", 
	      ctx.pcap_hdr.snaplen);
    }
	 
  
  {
    int done = 0;
    int pkt = 0;

    while (!done)
      {
	pcaprec_hdr_t rec_hdr;
	uint8_t *data = NULL;
	uint32_t len = 0;
	int sent_to_output = 0;
	
	err = pcap_read_next(ctx.pcreader, &rec_hdr, &data, &len);
	
	switch (err)
	  {
	  case 0: // EOF.
	    ++done;
	    break;
	  case 1: // Got a packet.
	    {
	      uint8_t *allocated = data;

	      printf("Packet: Time = %d.%d orig_len = %d \n", 
		     rec_hdr.ts_sec, rec_hdr.ts_usec, 
		     rec_hdr.orig_len);
	      
	      if (!(ctx.pcap_hdr.network == PCAP_NETWORK_TYPE_ETHERNET))
		{
		  goto dump_out;
		}

	      {
		ethernet_packet_t epkt;
		uint32_t out_st, out_len;
		int rv;
		ipv4_header_t ipv4_hdr;
		ipv4_udp_header_t udp_hdr;
		
		rv = ethernet_packet_from_pcap(&rec_hdr, 
					       data, len, 
					       &epkt,
					       &out_st,
					       &out_len);

		if (rv)
		  {
		    goto dump_out;
		  }

		printf("Ethernet: src %02x:%02x:%02x:%02x:%02x:%02x "
		       " dst %02x:%02x:%02x:%02x:%02x:%02x "
		       "typeorlen 0x%04x\n",
		       epkt.src_addr[0], epkt.src_addr[1], 
		       epkt.src_addr[2], epkt.src_addr[3], 
		       epkt.src_addr[4], epkt.src_addr[5],
		       epkt.dst_addr[0], epkt.dst_addr[1], 
		       epkt.dst_addr[2], epkt.dst_addr[3],
		       epkt.dst_addr[4], epkt.dst_addr[5], 
		       epkt.typeorlen);
		
		data = &data[out_st];
		len = out_len;
		
		// Is it IP?
		if (epkt.typeorlen != 0x800)
		  {
		    goto dump_out;
		  }


		rv = ipv4_from_payload(data, len, 
				       &ipv4_hdr, 
				       &out_st, 
				       &out_len);
		if (rv)
		  {
		    goto dump_out;
		  }
		
		printf("IPv4: src = %s", 
		       ipv4_addr_to_string(ipv4_hdr.src_addr));
		printf(" dest = %s \n", 
		       ipv4_addr_to_string(ipv4_hdr.dest_addr));
		
		printf(
		       "IPv4: version = 0x%x hdr_length = 0x%x"
		       " serv_type = 0x%08x length = 0x%04x\n"
		       "IPv4: ident = 0x%04x flags = 0x%02x"
		       " frag_offset = 0x%04x ttl = %d\n"
		       "IPv4: proto = %d csum = 0x%04x\n",
		       ipv4_hdr.version,
		       ipv4_hdr.hdr_length,
		       ipv4_hdr.serv_type,
		       ipv4_hdr.length,
		       ipv4_hdr.ident,
		       ipv4_hdr.flags,
		       ipv4_hdr.frag_offset,
		       ipv4_hdr.ttl,
		       ipv4_hdr.proto,
		       ipv4_hdr.csum);
		
		data = &data[out_st];
		len = out_len;
		
		if (!(IPV4_HDR_IS_UDP(&ipv4_hdr)))
		  {
		    goto dump_out;
		  }
		
		
		rv = ipv4_udp_from_payload(data, len,
					   &udp_hdr,
					   &out_st,
					   &out_len);
		if (rv) 
		  {
		    goto dump_out;
		  }
		
		printf("UDP: src port = %d "
		       "dest port = %d len = %d \n",
		       udp_hdr.source_port,
			 udp_hdr.dest_port,
		       udp_hdr.length);
		
		data = &data[out_st];
		len = out_len;
		
		if (ctx.output_name && 
		    (!ctx.output_dest_addr || (ipv4_hdr.dest_addr == ctx.output_dest_addr)) && 
		    (!ctx.output_dest_port || (udp_hdr.dest_port == ctx.output_dest_port)))
		  {
		    ++sent_to_output;
		    if (!ctx.output_file)
		      {
			ctx.output_file = fopen(ctx.output_name, "wb");
			if (!ctx.output_file)
			  {
			    fprintf(stderr,"### pcapreport: Cannot open %s .\n", 
				    ctx.output_name);
			    return 1;
			  }
		      }
		    
		    printf(">> Dumping %d bytes to output file.\n", len);
		    rv = fwrite(data, len, 1, ctx.output_file);
		    if (rv != 1)
		      {
			fprintf(stderr, "### pcapreport: Couldn't write %d bytes"
				" to %s (error = %d).\n", 
				len, ctx.output_name, 
				ferror(ctx.output_file));
			return 1;
		      }
		  }
	      }

	      // Adjust 
	    dump_out:
	      if (ctx.dump_data || (ctx.dump_extra && !sent_to_output))
		{
		  print_data(stdout, "data", 
			     data, len, len);
		}
	      free(allocated); allocated = data = NULL;
	    }
	    break;
	  default:
	    // Some other error.
	    fprintf(stderr, "### pcapreport: Can't read packet %d - code %d\n",
		    pkt, err);
	    ++done;
	    break;
	  }
	++pkt;
      }
  }

  if (ctx.output_file) 
    {
      printf("Closing output file.\n");
      fclose(ctx.output_file); 
    }

  return 0;
}

/* End file */

/* pcap.c */
/*
 * Read pcap files.
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
 *   Richard Watts, Kynesim <rrw@kynesim.co.uk>
 *
 * ***** END LICENSE BLOCK *****
 */


/* Both of these return 1 on success, 0 on EOF,  <0 on error */

#include "pcap.h"
#include "misc_fns.h"

static inline uint32_t uint_32_ctx(const struct _pcap_io_ctx *const ctx, const void *v)
{
  return ctx->is_be ? uint_32_be(v) : uint_32_le(v);
}

static inline uint16_t uint_16_ctx(const struct _pcap_io_ctx *const ctx, const void *v)
{
  return ctx->is_be ? uint_16_be(v) : uint_16_le(v);
}

// Hi-32, Lo-32 but native within!
static inline uint64_t uint_64_be_ctx(const struct _pcap_io_ctx *const ctx, const void *v)
{
  return ((uint64_t)uint_32_ctx(ctx, v) << 32) | (uint64_t)uint_32_ctx(ctx, (const char*)v + 4);
}

static inline uint64_t uint_64_ctx(const struct _pcap_io_ctx *const ctx, const void *v)
{
  return ctx->is_be ?
         ((uint64_t)uint_32_be(v) << 32) | (uint64_t)uint_32_be((const uint8_t *)v + 4) :
         ((uint64_t)uint_32_le((const uint8_t *)v + 4) << 32) | (uint64_t)uint_32_le(v);
}



static int read_block_header(const struct _pcap_io_ctx *const ctx, uint32_t *const pLength)
{
  uint32_t buf[2];
  int rv;

  *pLength = 0;
  rv = fread(buf, 8, 1, ctx->file);
  if (rv != 1)
  {
    if (feof(ctx->file))
    {
      return 0;
    }
    else
    {
      return PCAP_ERR_FILE_READ;
    }
  }

  *pLength = uint_32_ctx(ctx, buf + 1);
  return uint_32_ctx(ctx, buf + 0);
}


static int read_chunk(FILE *const f, const size_t len, uint8_t **const pBuf)
{
  int rv;
  void *buf = malloc(len);

  *pBuf = NULL;
  if (buf == NULL)
  {
    return PCAP_ERR_OUT_OF_MEMORY;
  }

  rv = fread(buf, len, 1, f);
  if (rv != 1)
  {
    free(buf);
    if (feof(f))
    {
      return 0;
    }
    else
    {
      return PCAP_ERR_FILE_READ;
    }
  }

  *pBuf = buf;
  return 1;
}

static int read_options(FILE *const f, const size_t len, uint8_t **const pBuf)
{
  // If all we have is the final total length data - skip it
  if (len <= 4)
  {
    fseek(f, len, SEEK_CUR);
    *pBuf = NULL;
    return 1;
  }

  return read_chunk(f, len, pBuf);
}

typedef enum pcapng_type_e
{
  PCAPNG_TYPE_INVALID_BLOCK = 0,
  PCAPNG_TYPE_INTERFACE_BLOCK = 1,
  PCAPNG_TYPE_PACKET_BLOCK = 2,
  PCAPNG_TYPE_SIMPLE_PACKET_BLOCK = 3,
  PCAPNG_TYPE_NAME_RESOLUTION_BLOCK = 4,
  PCAPNG_TYPE_INTERFACE_STATISTICS_BLOCK = 5,
  PCAPNG_TYPE_ENHANCED_PACKET_BLOCK = 6,
  PCAPNG_TYPE_SECTION_HEADER_BLOCK = 0x0a0d0d0a
} pcapng_type_t;

typedef struct pcapng_hdr_packet_s
{
  uint16_t drops_count;
  uint32_t interface_id;
  uint32_t captured_len;
  uint32_t packet_len;
  uint64_t timestamp;
} pcapng_hdr_packet_t;

typedef struct pcapng_hdr_section_s
{
  uint16_t major_version;
  uint16_t minor_version;
  uint64_t section_length;
} pcapng_hdr_section_t;



typedef struct pcapng_header_s
{
  pcapng_type_t type;
  uint8_t *data;
  uint8_t *options;
  union
  {
    pcapng_hdr_packet_t packet;
    pcapng_hdr_interface_t iface;
    pcapng_hdr_section_t section;
  } hdr;
} pcapng_header_t;


// Kill header contents
static void free_block(pcapng_header_t *const hdr)
{
  hdr->type = PCAPNG_TYPE_INVALID_BLOCK;
  if (hdr->data != NULL)
  {
    free(hdr->data);
    hdr->data = NULL;
  }
  if (hdr->options != NULL)
  {
    free(hdr->options);
    hdr->options = NULL;
  }
}

static int do_section_header(struct _pcap_io_ctx *const ctx, uint32_t length, const uint8_t *const buf,
  pcapng_header_t *const hdr)
{
  uint32_t magic;
  int rv;

  // PCAP-NG
  ctx->is_ng = 1;

  magic = uint_32_ctx(ctx, buf + 0);

  printf("Magic = %08x, Len = %#x\n", magic, length);

  if (magic == 0x1a2b3c4d)
  {
    // Right way up
  }
  else if (magic == 0x4d3c2b1a)
  {
    // Wrong way up
    ctx->is_be = !ctx->is_be;
    // Endian reverse length
    length = (length >> 16) | (length << 16);
    length = ((length >> 8) & 0xff00ff) | ((length << 8) & 0xff00ff00);
  }
  else
  {
    return PCAP_ERR_INVALID_MAGIC;
  }


#if SIZEOF_PCAP_HDR_ON_DISC != 24
#error I am confused
#endif

  // Length here includes headers
  if (length < 28)
  {
    return PCAP_ERR_BAD_LENGTH;
  }

  length -= 24;

  hdr->hdr.section.major_version = uint_16_ctx(ctx, buf + 4);
  hdr->hdr.section.minor_version = uint_16_ctx(ctx, buf + 6);
  hdr->hdr.section.section_length = uint_64_ctx(ctx, buf + 8);

  if ((rv = read_options(ctx->file, length, &hdr->options)) <= 0)
    return rv;

  return 1;
}

static int read_block(struct _pcap_io_ctx *const ctx, pcapng_header_t *const hdr)
{
  int rv = 1;
  int hdr_type;
  uint32_t length;

  hdr->type = PCAPNG_TYPE_INVALID_BLOCK;
  hdr->data = NULL;
  hdr->options = NULL;

  if ((hdr_type = read_block_header(ctx, &length)) <= 0)
  {
    return hdr_type;
  }

  // If section header length may be endian confused so sort in a bit
  if (hdr_type != PCAPNG_TYPE_SECTION_HEADER_BLOCK)
  {
    if (length > 0x100000 || length < 8) 
    {
      return PCAP_ERR_BAD_LENGTH;
    }
    length -= 8;
  }

  switch (hdr_type)
  {
      case PCAPNG_TYPE_INTERFACE_BLOCK:
      {
        uint8_t buf[8];

        if (length < 12)
          return PCAP_ERR_BAD_LENGTH;

        if (fread(buf, 8, 1, ctx->file) != 1)
          return PCAP_ERR_FILE_READ;

        hdr->hdr.iface.link_type = uint_16_ctx(ctx, buf + 0);
        hdr->hdr.iface.snap_len = uint_32_ctx(ctx, buf + 4);

        if ((rv = read_options(ctx->file, length - 8, &hdr->options)) <= 0)
          return rv;

        // Now stash - cos we need it later
        // Alloc a new if (or at least check we have one)
        if (ctx->if_count + 1 > ctx->if_size)
        {
          if (ctx->interfaces == NULL)
          {
            if ((ctx->interfaces = malloc(sizeof(*ctx->interfaces) * 4)) == NULL)
              return PCAP_ERR_OUT_OF_MEMORY;
            ctx->if_size = 4;
          }
          else
          {
            pcapng_hdr_interface_t *resized = realloc(ctx->interfaces,
              sizeof(*ctx->interfaces) * ctx->if_size * 2);
            if (resized == NULL)
              return PCAP_ERR_OUT_OF_MEMORY;
            ctx->if_size *= 2;
            ctx->interfaces = resized;
          }
        }

        ctx->interfaces[ctx->if_count++] = hdr->hdr.iface;
        break;
      }

      case PCAPNG_TYPE_PACKET_BLOCK:
      case PCAPNG_TYPE_ENHANCED_PACKET_BLOCK:
      {
        uint8_t buf[20];
        size_t data_len;

        if (length < 24)
          return PCAP_ERR_BAD_LENGTH;

        if (fread(buf, 20, 1, ctx->file) != 1)
          return PCAP_ERR_FILE_READ;

        if (hdr_type == PCAPNG_TYPE_PACKET_BLOCK)
        {
          hdr->hdr.packet.interface_id = uint_16_ctx(ctx, buf + 0);
          hdr->hdr.packet.drops_count = uint_16_ctx(ctx, buf + 2);
        }
        else
        {
          hdr->hdr.packet.interface_id = uint_32_ctx(ctx, buf + 0);
          hdr->hdr.packet.drops_count = 0;
        }
        hdr->hdr.packet.timestamp = uint_64_be_ctx(ctx, buf + 4);
        hdr->hdr.packet.captured_len = uint_32_ctx(ctx, buf + 12);
        hdr->hdr.packet.packet_len = uint_32_ctx(ctx, buf + 16);

        if (hdr->hdr.packet.interface_id >= ctx->if_count)
          return PCAP_ERR_BAD_INTERFACE_ID;

        length -= 20;
        data_len = (hdr->hdr.packet.captured_len + 3) & ~3;

        if (length - 4 < data_len)
          return PCAP_ERR_BAD_LENGTH;

        if ((rv = read_chunk(ctx->file, data_len, &hdr->data)) <= 0)
          break;

        length -= data_len;

        if ((rv = read_options(ctx->file, length, &hdr->options)) <= 0)
          break;

        break;
      }

      case PCAPNG_TYPE_SECTION_HEADER_BLOCK:
      {
        uint8_t buf[16];

        // Clear out old data even if we error

        // All interfaces are toast
        if (ctx->interfaces != NULL)
        {
          free(ctx->interfaces);
          ctx->interfaces = NULL;
          ctx->if_count = 0;
          ctx->if_size = 0;
        }

        if (fread(buf, 16, 1, ctx->file) != 1)
          return PCAP_ERR_FILE_READ;

        if ((rv = do_section_header(ctx, length, buf, hdr)) < 0)
          return rv;

        break;
      }

      default:
        fseek(ctx->file, length, SEEK_CUR);
        break;
  }

  if (rv <= 0)
  {
    free_block(hdr);
  }
  else
  {
    hdr->type = hdr_type;
  }

  return rv;
}


static int pcap_read_header(PCAP_reader_p ctx, pcap_hdr_t *hdr)
{
  uint8_t hdr_val[SIZEOF_PCAP_HDR_ON_DISC];
  int rv;
  uint32_t magic;

  // This reads an old-style header which is shorter than the shortest new-style one
  rv = fread(&hdr_val[0], SIZEOF_PCAP_HDR_ON_DISC, 1, ctx->file);
  if (rv != 1)
  {
    if (feof(ctx->file))
    {
      return 0;
    }
    else
    {
      return PCAP_ERR_FILE_READ;
    }
  }

  magic = uint_32_be(hdr_val + 0);

  if (magic == PCAPNG_TYPE_SECTION_HEADER_BLOCK)
  {
    pcapng_header_t nghdr = { 0 };

    printf("NG header found\n");

    // PCAP-NG
    ctx->is_ng = 1;

    if ((rv = do_section_header(ctx, uint_32_ctx(ctx, hdr_val + 4), hdr_val + 8, &nghdr)) <= 0)
      return rv;

    hdr->magic_number = 0x1a2b3c4d;

    hdr->version_major = nghdr.hdr.section.major_version;
    hdr->version_minor = nghdr.hdr.section.minor_version;

    printf("Version: %d.%d\n", hdr->version_major, hdr->version_minor);

    // Find the 1st i/f block (there must be one before the data)

    for (;;)
    {
      free_block(&nghdr);

      if ((rv = read_block(ctx, &nghdr)) <= 0)
      {
        return rv;
      }

      if (nghdr.type == PCAPNG_TYPE_INTERFACE_BLOCK)
      {
        hdr->snaplen = nghdr.hdr.iface.snap_len;
        hdr->network = nghdr.hdr.iface.link_type;
        free_block(&nghdr);
        break;
      }
    }
  }
  else
  {
    ctx->is_ng = 0;

    /* The magic number is 0xa1b2c3d4. If the writing
     * machine was BE, the first byte will be a1 else d4
     */
    if (magic == 0xa1b2c3d4)
    {
      // Big endian.
      ctx->is_be = 1;
    }
    else if (magic == 0xd4c3b2a1)
    {
      // Little endian.
      ctx->is_be = 0;
    }
    else
    {
      return PCAP_ERR_INVALID_MAGIC;
    }

    hdr->magic_number = 0xa1b2c3d4;

    hdr->version_major = (ctx->is_be ? uint_16_be(&hdr_val[4]) :
                                       uint_16_le(&hdr_val[4]));
    hdr->version_minor = (ctx->is_be ? uint_16_be(&hdr_val[6]) :
                                       uint_16_le(&hdr_val[6]));
    hdr->thiszone = (int32_t)(ctx->is_be ? uint_32_be(&hdr_val[8]) :
                                           uint_32_le(&hdr_val[8]));
    hdr->sigfigs = (ctx->is_be ? uint_32_be(&hdr_val[12]) :
                                 uint_32_le(&hdr_val[12]));
    hdr->snaplen = (ctx->is_be ? uint_32_be(&hdr_val[16]) :
                                 uint_32_le(&hdr_val[16]));
    hdr->network = (ctx->is_be ? uint_32_be(&hdr_val[20]) :
                                 uint_32_le(&hdr_val[20]));
  }


  return 1;
}

static int pcap_read_pktheader(PCAP_reader_p ctx, pcaprec_hdr_t *hdr)
{
  uint8_t hdr_val[SIZEOF_PCAPREC_HDR_ON_DISC];
  int rv;

  rv = fread(&hdr_val[0], SIZEOF_PCAPREC_HDR_ON_DISC, 1, ctx->file);
  if (rv != 1)
  {
    if (feof(ctx->file))
    {
      return 0;
    }
    else
    {
      return PCAP_ERR_FILE_READ;
    }
  }

  hdr->ts_sec = (ctx->is_be ? uint_32_be(&hdr_val[0]) :
                              uint_32_le(&hdr_val[0]));
  hdr->ts_usec = (ctx->is_be ? uint_32_be(&hdr_val[4]) :
                               uint_32_le(&hdr_val[4]));
  hdr->incl_len = (ctx->is_be ? uint_32_be(&hdr_val[8]) :
                                uint_32_le(&hdr_val[8]));
  hdr->orig_len = (ctx->is_be ? uint_32_be(&hdr_val[12]) :
                                uint_32_le(&hdr_val[12]));

  return 1;
}

extern int pcap_open(PCAP_reader_p *ctx_p, pcap_hdr_t *out_hdr,
  const char *filename)
{
  FILE *fptr = (filename ? fopen(filename, "rb") : stdin);
  PCAP_reader_p ctx;
  int rv;

  (*ctx_p) = NULL;

  if (!fptr)
  {
    // Couldn't open the file.
    return -1;
  }
  ctx = (PCAP_reader_p)calloc(SIZEOF_PCAP_READER, 1);
  if (!ctx)
  {
    fclose(fptr);
    // Out of memory.
    return -2;
  }

  ctx->file = fptr;

  rv = pcap_read_header(ctx, out_hdr);

  if (rv != 1)
  {
    // Header read failed.
    fclose(ctx->file);
    free(ctx);
    return -4;
  }

  (*ctx_p) = ctx;

  return 0;
}

extern int pcap_read_next(PCAP_reader_p ctx, pcaprec_hdr_t *out_hdr,
  uint8_t **out_data,
  uint32_t *out_len)
{
  int rv;

  (*out_data) = NULL; (*out_len) = 0;

  if (ctx->is_ng)
  {
    for (;;)
    {
      pcapng_header_t nghdr;

      if ((rv = read_block(ctx, &nghdr)) <= 0)
      {
        return rv;
      }

      if (nghdr.type == PCAPNG_TYPE_PACKET_BLOCK ||
          nghdr.type == PCAPNG_TYPE_ENHANCED_PACKET_BLOCK)
      {
        *out_data = nghdr.data;
        *out_len = nghdr.hdr.packet.captured_len;

        out_hdr->incl_len = nghdr.hdr.packet.captured_len;
        out_hdr->orig_len = nghdr.hdr.packet.packet_len;
        out_hdr->ts_sec = (uint32_t)(nghdr.hdr.packet.timestamp / 1000000);
        out_hdr->ts_usec = (uint32_t)(nghdr.hdr.packet.timestamp % 1000000);

        // NULL out so we don't free it here!
        nghdr.data = NULL;
        free_block(&nghdr);
        return 1;
      }

      free_block(&nghdr);
    }
  }
  else
  {
    rv = pcap_read_pktheader(ctx, out_hdr);
    if (rv != 1)
    {return rv; }

    // Otherwise we now know how long our packet is ..
    (*out_data) = (uint8_t*)malloc(out_hdr->incl_len);

    if (!(*out_data))
    {
      // Out of memory.
      return -3;
    }

    (*out_len) = out_hdr->incl_len;

    rv = fread((*out_data), (*out_len), 1, ctx->file);
    if (rv != 1)
    {
      free(*out_data); (*out_data) = NULL;
      *out_len = 0;

      if (feof(ctx->file))
      {
        // Ah. EOF.
        return 0;
      }
      else
      {
        // Error. Curses.
        return rv;
      }
    }
    else if (rv == 1)
    {
      // Gotcha.
      return 1;
    }
  }

  return 0;
}

int pcap_close(PCAP_reader_p *const pctx)
{
  PCAP_reader_p ctx = *pctx;

  if (ctx == NULL)
    return 0;

  if (ctx->interfaces != NULL)
  {
    free(ctx->interfaces);
  }
  if (ctx->file != NULL)
  {
    fclose(ctx->file);
  }
  free(ctx);

  return 0;
}


// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:


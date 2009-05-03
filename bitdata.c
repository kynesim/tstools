/*
 * Functions to handle byte data as bit data, and particularly to read
 * Exp-Golomb encoded data.
 *
 * See H.264 clause 10.
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
#include <math.h>
#include <assert.h>

#include "compat.h"
#include "bitdata_fns.h"
#include "printing_fns.h"

static int MASK[] =  { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };

/*
 * Build a new bitdata datastructure.
 *
 * - `data` is the byte array we're extracting bits from.
 * - `data_len` is its length (in bytes).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_bitdata(bitdata_p  *bitdata,
                         byte        data[],
                         int         data_len)
{
  bitdata_p  new = malloc(SIZEOF_BITDATA);
  if (new == NULL)
  {
    print_err("### Unable to allocate bitdata datastructure\n");
    return 1;
  }

  new->data = data;
  new->data_len = data_len;
  new->cur_byte = 0;
  new->cur_bit = -1;

  *bitdata = new;
  return 0;

}


/*
 * Tidy up and free a bitdata datastructure after we've finished with it.
 *
 * Clears the bitdata datastructure, frees it, and sets `bitdata` to NULL.
 *
 * Does nothing if `bitdata` is already NULL.
 */
extern void free_bitdata(bitdata_p  *bitdata)
{
  if (*bitdata == NULL)
    return;
  (*bitdata)->data = NULL;
  (*bitdata)->cur_byte = 0;
  (*bitdata)->cur_bit = -1;
  free(*bitdata);
  *bitdata = NULL;
}

/*
 * Return the next bit from the data.
 *
 * Returns 0 or 1 if it reads the bit correctly, -1 if there are no more
 * bits to be read.
 */
static inline int next_bit(bitdata_p  bitdata)
{
  bitdata->cur_bit += 1;
  if (bitdata->cur_bit == 8)
  {
    bitdata->cur_bit = 0;
    bitdata->cur_byte += 1;
    if (bitdata->cur_byte > (bitdata->data_len - 1))
    {
      print_err("### No more bits to read from input stream\n");
      return -1;
    }
  }
  return (bitdata->data[bitdata->cur_byte] & MASK[bitdata->cur_bit])
    >> (7 - bitdata->cur_bit);
}

/*
 * Return the next bit from the data.
 *
 * Returns 0 if it reads the bit correctly, 1 if there are no more
 * bits to be read.
 */
extern int read_bit(bitdata_p  bitdata,
                    byte      *bit)
{
  int next = next_bit(bitdata);
  if (next < 0)
    return 1;
  else
  {
    *bit = next;
    return 0;
  }
}
 
/*
 * Reads `count` bits from the data.
 *
 * Note it is asserted that `count` must be in the range 0..32.
 *
 * Returns 0 if all went well, 1 if there were not enough bits in the data.
 */
extern int read_bits(bitdata_p  bitdata,
                     int        count,
                     uint32_t  *bits)
{
  int      index = 0;
  uint32_t result = 0;

  assert((count >=0 && count <= 32));

  for (index=0; index<count; index++)
  {
    int  bit = next_bit(bitdata);
    if (bit < 0)
      return 1;
    else
      result = (result << 1) | bit;
  }
  *bits = result;
  return 0;
}
 
/*
 * Reads `count` bits from the data, into a byte.
 *
 * Note it is asserted that `count` must be in the range 0..8.
 *
 * Returns 0 if all went well, 1 if there were not enough bits in the data.
 */
extern int read_bits_into_byte(bitdata_p  bitdata,
                               int        count,
                               byte      *bits)
{
  int   index = 0;
  byte  result = 0;

  assert((count >=0 && count <= 8));

  for (index=0; index<count; index++)
  {
    int  bit = next_bit(bitdata);
    if (bit < 0)
      return 1;
    else
      result = (result << 1) | bit;
  }
  *bits = result;
  return 0;
}

/*
 * Read zero bits, counting them. Stop at the first non-zero bit.
 *
 * Returns the number of zero bits. Note that the non-zero bit is not
 * "unread" in any way, so reading another bit will retrieve the first bit
 * thereafter.
 */
extern int count_zero_bits(bitdata_p  bitdata)
{
  int count = 0;
  while (next_bit(bitdata) == 0)
    count ++;
  return count;
}

/*
 * Read and decode an Exp-Golomb code.
 *
 * Reference H.264 10.1 for an explanation.
 *
 * Returns 0 if all went well, 1 if there were not enough bits in the data.
 */
extern int read_exp_golomb(bitdata_p   bitdata,
                           uint32_t   *result)
{
  uint32_t next = 0;
  int leading_zero_bits = count_zero_bits(bitdata);
  int err = read_bits(bitdata,leading_zero_bits,&next);
  if (err)
  {
    fprint_err("### Unable to read ExpGolomb value - not enough bits (%d)\n",
               leading_zero_bits);
    return err;
  }
  *result = (int) (pow(2,leading_zero_bits) - 1 + next);
  return 0;
}

/*
 * Read and decode a signed Exp-Golomb code.
 *
 * Reference H.264 10.1 sqq for an explanation.
 *
 * Returns 0 if all went well, 1 if there were not enough bits in the data.
 */
extern int read_signed_exp_golomb(bitdata_p   bitdata,
                                  int32_t    *result)
{
  uint32_t val = 0;
  int err = read_exp_golomb(bitdata,&val);
  if (err)
  {
    print_err("### Unable to read signed ExpGolomb value\n");
    return err;
  }
  *result = (int) (pow(-1,(val+1)) * ceil(val / 2.0));
  return 0;
}


// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

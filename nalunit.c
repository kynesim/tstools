/*
 * Utilities for working with NAL units in H.264 elementary streams.
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
#include <math.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#else  // _WIN32
#include <unistd.h>
#endif // _WIN32

#include "compat.h"
#include "printing_fns.h"
#include "es_fns.h"
#include "ts_fns.h"
#include "bitdata_fns.h"
#include "nalunit_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"

#define DEBUG 0

#define REPORT_NAL_SHOWS_ADDRESS 0

/*
 * Request details of the NAL unit contents as they are read
 */
extern void set_show_nal_reading_details(nal_unit_context_p  context,
                                         int                 show)
{
  context->show_nal_details = show;

}

// ------------------------------------------------------------
// NAL unit context
// ------------------------------------------------------------
/*
 * Build a new NAL unit context, for reading NAL units from an ES.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_nal_unit_context(ES_p                es,
                                  nal_unit_context_p *context)
{
  int err;
  nal_unit_context_p  new = malloc(SIZEOF_NAL_UNIT_CONTEXT);
  if (new == NULL)
  {
    print_err("### Unable to allocate NAL unit context datastructure\n");
    return 1;
  }
  new->es = es;
  new->count = 0;
  new->show_nal_details = FALSE;
  err = build_param_dict(&new->seq_param_dict);
  if (err)
  {
    free(new);
    return err;
  }
  err = build_param_dict(&new->pic_param_dict);
  if (err)
  {
    free_param_dict(&new->seq_param_dict);
    free(new);
    return err;
  }
  *context = new;
  return 0;
}

/*
 * Free a NAL unit context datastructure.
 *
 * Clears the datastructure, frees it, and returns `context` as NULL.
 *
 * Does nothing if `context` is already NULL.
 */
extern void free_nal_unit_context(nal_unit_context_p *context)
{
  nal_unit_context_p  cc = *context;

  if (cc == NULL)
    return;

  free_param_dict(&cc->seq_param_dict);
  free_param_dict(&cc->pic_param_dict);
  
  free(*context);
  *context = NULL;
  return;
}

/*
 * Rewind a file being read as NAL units.
 *
 * A thin jacket for `seek_ES`.
 *
 * Doesn't unset the sequence and picture parameter dictionaries
 * that have been built up when reading the file - this may possibly
 * not be the desired behaviour, but should be OK for well behaved files.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int rewind_nal_unit_context(nal_unit_context_p  context)
{
  ES_offset  start_of_file = {0,0};
  return seek_ES(context->es,start_of_file);
}

// ------------------------------------------------------------
// Basic NAL unit datastructure stuff
// ------------------------------------------------------------
/*
 * Build a new NAL unit datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_nal_unit(nal_unit_p  *nal)
{
  int  err;
  nal_unit_p  new = malloc(SIZEOF_NAL_UNIT);
  if (new == NULL)
  {
    print_err("### Unable to allocate NAL unit datastructure\n");
    return 1;
  }

  err = setup_ES_unit(&(new->unit));
  if (err)
  {
    print_err("### Unable to allocate NAL unit data buffer\n");
    free(new);
    return 1;
  }

  // However, we haven't yet got any actual data
  new->data = NULL;  // Only set to unit.data[3] when we *have* a NAL unit
  new->data_len = 0;
  new->rbsp = NULL;
  new->rbsp_len = 0;
  new->bit_data = NULL;

  new->nal_unit_type = NAL_UNSPECIFIED;

  new->starts_picture_decided = FALSE;
  new->starts_picture = FALSE;
  new->start_reason = NULL;
  new->decoded = FALSE;

  *nal = new;
  return 0;
}

/*
 * Tidy up a NAL unit datastructure after we've finished with it.
 */
static inline void clear_nal_unit(nal_unit_p  nal)
{
  clear_ES_unit(&(nal->unit));
  nal->data = NULL;
  nal->data_len = 0;
  if (nal->rbsp != NULL)
  {
    free(nal->rbsp);
    nal->rbsp_len = 0;
  }
  free_bitdata(&nal->bit_data);
}

/*
 * Tidy up and free a NAL unit datastructure after we've finished with it.
 *
 * Empties the NAL unit datastructure, frees it, and sets `nal` to NULL.
 *
 * If `nal` is already NULL, does nothing.
 */
extern void free_nal_unit(nal_unit_p  *nal)
{
  if (*nal == NULL)
    return;
  clear_nal_unit(*nal);
  free(*nal);
  *nal = NULL;
}

// ------------------------------------------------------------
// Interpretive functions
// ------------------------------------------------------------

/*
 * Process `data` to remove any emulation prevention bytes.
 *
 * That is, sort out 0x000003 sequences.
 *
 * Basically, if the data stream was *meant* to contain 0x0000xx,
 * then it will instead contain 0x000003xx, and so we need to remove
 * that extra 0x03 (in fact, there are also rules about what values
 * 0xxx can take, but we shall assume that they have been followed
 * by the encoder).  See H.264 7.3.1
 *
 * - `data` is the NAL unit data array, including the first byte,
 *   which contains the nal_ref_idc and nal_unit_type.
 * - `rbsp` is the processed data, not including said first byte.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int remove_emulation_prevention(byte   data[],
                                       int    data_len,
                                       byte  *rbsp[],
                                       int   *rbsp_len)
{
  int  ii;
  int  posn = 0;
  byte prev1 = 27; // J. Random Number
  byte prev2 = 27;
  byte *tgt = NULL;

  // We know we're going to produce data that is no longer than our input
  tgt = malloc(data_len);
  if (tgt == NULL)
  {
    print_err("### Cannot malloc RBSP target array\n");
    return 1;
  }
  
  for (ii=1; ii<data_len; ii++)  // NB: ignoring that first byte
  {
    if (prev2 == 0x00 && prev1 == 0x00 && data[ii] == 0x03)
      ; // ignore the emulation prevention 03 byte
    else
      tgt[posn++] = data[ii];
    prev2 = prev1;
    prev1 = data[ii];
  }
  *rbsp = tgt;
  *rbsp_len = posn;
  return 0;
}

/*
 * Prepare for reading bit data from the RBSP.
 *
 * (Note that calling this more than once is safe.)
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static inline int prepare_rbsp(nal_unit_p  nal)
{
  int        err;
  bitdata_p  bd=NULL;

  if (nal->bit_data != NULL)
    return 0;

  // Only remove the emulation 03 bytes when we think we need to
  // (of course, we *could* do this as part of the bitdata byte
  // reading code, but unless/until it's clear that the tradeoff
  // in time/complexity is worth it, let's not bother).
  if (nal->rbsp == NULL)
  {
    err = remove_emulation_prevention(nal->data,nal->data_len,
                                      &(nal->rbsp),&(nal->rbsp_len));
    if (err)
    {
      print_err("### Error removing emulation prevention bytes\n");
      return 1;
    }
  }
  
  err = build_bitdata(&bd,nal->rbsp,nal->rbsp_len);
  if (err)
  {
    print_err("### Unable to build bitdata datastructure for NAL RBSP\n");
    return 1;
  }
  nal->bit_data = bd;
  return 0;
}

/*
 * Look at the start of the slice header.
 *
 * Assumes that this *is* a NAL unit representing a slice.
 *
 * Don't call this directly - call read_rbsp_data() instead.
 *
 * If either of `seq_param_dict` or `pic_param_dict` is NULL, then
 * we only read the first few entries in the RBSP (including the
 * slice_type and the pic_parameter_set_id).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int read_slice_data(nal_unit_p   nal,
                           param_dict_p seq_param_dict,
                           param_dict_p pic_param_dict,
                           int          show_nal_details)
{
  int        err;
  bitdata_p  bd = nal->bit_data;
  uint32_t   temp;
  nal_slice_data_p data = &(nal->u.slice);
  nal_seq_param_data_p seq_param_data = NULL;
  nal_pic_param_data_p pic_param_data = NULL;

#define CHECK(name) \
  if (err)                                      \
  {                                             \
    fprint_err("### Error reading %s field from slice data\n",(name)); \
  }

  err = read_exp_golomb(bd,&data->first_mb_in_slice);
  CHECK("first_mb_in_slice");
  err = read_exp_golomb(bd,&data->slice_type);
  CHECK("slice_type");
  err = read_exp_golomb(bd,&temp);
  CHECK("pic_parameter_set_id");
  data->pic_parameter_set_id = temp;

  if (show_nal_details)
  {
    fprint_msg("@@ NAL " OFFSET_T_FORMAT_08 "/%04d: size %d\n"
               "   nal_ref_idc %x nal_unit_type %02x (%s)\n",
               nal->unit.start_posn.infile,nal->unit.start_posn.inpacket,
               nal->data_len,
               nal->nal_ref_idc,nal->nal_unit_type,
               NAL_UNIT_TYPE_STR(nal->nal_unit_type));
    fprint_msg("   first_mb_in_slice %u, slice_type %u (%s),"
               " pic_parameter_set_id %d\n",data->first_mb_in_slice,
               data->slice_type,NAL_SLICE_TYPE_STR(data->slice_type),
               data->pic_parameter_set_id);
  }
  
  // If we don't have sequence/parameter sets, then we can't go any
  // further. Assume the caller knew what they were doing...
  if (seq_param_dict == NULL || pic_param_dict == NULL)
    return 0;
  
  // To read the frame number we need to know how long it is, which is
  // determined by the value of log2_max_frame_num, which is defined in the
  // relevant sequence parameter set, which is determined by the picture
  // parameter set whose id we just read.
  err = get_pic_param_data(pic_param_dict,data->pic_parameter_set_id,
                           &pic_param_data);
  if (err) return 1;
  err = get_seq_param_data(seq_param_dict,pic_param_data->seq_parameter_set_id,
                           &seq_param_data);
  if (err) return 1;

  // Whilst we've got the sequence parameter set to hand, it's convenient
  // to remember the pic_order_cnt_type locally, so that we don't need to
  // look it up again when trying to decide if this is the first VCL NAL
  data->seq_param_set_pic_order_cnt_type = seq_param_data->pic_order_cnt_type;

  if (show_nal_details)
    fprint_msg("   seq_param_set->pic_order_cnt_type %u\n",
               data->seq_param_set_pic_order_cnt_type);
  
  err = read_bits(bd,seq_param_data->log2_max_frame_num,&data->frame_num);
  CHECK("frame_num");

  if (show_nal_details)
    fprint_msg("   frame_num %u (%d bits)\n",data->frame_num,
               seq_param_data->log2_max_frame_num);

  data->field_pic_flag = 0;     // value if not present - i.e., a frame
  data->bottom_field_flag = 0;
  data->bottom_field_flag_present = FALSE;
  if (!seq_param_data->frame_mbs_only_flag)
  {
    err = read_bit(bd,&data->field_pic_flag);
    CHECK("field_pic_flag");
    if (show_nal_details)
      fprint_msg("   field_pic_flag %d\n",data->field_pic_flag);
    if (data->field_pic_flag)
    {
      data->bottom_field_flag_present = TRUE;
      err = read_bit(bd,&data->bottom_field_flag);
      CHECK("bottom_field_flag");
      if (show_nal_details)
        fprint_msg("   bottom_field_flag %d\n",data->bottom_field_flag);
    }
  }
  
  if (nal->nal_unit_type == 5)
  {
    err = read_exp_golomb(bd,&data->idr_pic_id);
    CHECK("idr_pic_id");
    if (show_nal_details)
      fprint_msg("   idr_pic_id %u\n",data->idr_pic_id);
  }

  data->delta_pic_order_cnt_bottom = 0;  // value if not present
  data->delta_pic_order_cnt[0] = 0;
  data->delta_pic_order_cnt[1] = 0;
  if (seq_param_data->pic_order_cnt_type == 0)
  {
    err = read_bits(bd,seq_param_data->log2_max_pic_order_cnt_lsb,
                    &data->pic_order_cnt_lsb);
    CHECK("pic_order_cnt_lsb");
    if (pic_param_data->pic_order_present_flag && !data->field_pic_flag)
    {
      err = read_signed_exp_golomb(bd,&data->delta_pic_order_cnt_bottom);
      CHECK("delta_pic_order_cnt_bottom");
    }
    if (show_nal_details)
      fprint_msg("   pic_order_cnt_lsb %u (%d bits)\n"
                 "   delta_pic_order_cnt_bottom %d\n",
                 data->pic_order_cnt_lsb,
                 seq_param_data->log2_max_pic_order_cnt_lsb,
                 data->delta_pic_order_cnt_bottom);
  }
  else if (seq_param_data->pic_order_cnt_type == 1 &&
           !seq_param_data->delta_pic_order_always_zero_flag)
  {
    err = read_signed_exp_golomb(bd,&data->delta_pic_order_cnt[0]);
    CHECK("delta_pic_order_cnt[0]");
    if (show_nal_details)
      fprint_msg("   delta_pic_order_cnt[0] %d\n",data->delta_pic_order_cnt[0]);

    if (pic_param_data->pic_order_present_flag && !data->field_pic_flag)
    {
      err = read_signed_exp_golomb(bd,&data->delta_pic_order_cnt[1]);
      CHECK("delta_pic_order_cnt[1]");
      if (show_nal_details)
        fprint_msg("   delta_pic_order_cnt[1] %d\n",data->delta_pic_order_cnt[1]);
    }
  }

  // Since we're not claiming to support redundant pictures, we could
  // give up before reading the next value. However, if we *do* read
  // it, we can grumble about/ignore redundant pictures if we get them,
  // which seems a useful thing to be able to do.
  data->redundant_pic_cnt = 0;
  data->redundant_pic_cnt_present = FALSE;
  if (pic_param_data->redundant_pic_cnt_present_flag)
  {
    data->redundant_pic_cnt_present = TRUE;
    err = read_exp_golomb(bd,&data->redundant_pic_cnt);
    CHECK("redundant_pic_cnt");
    if (show_nal_details)
      fprint_msg("   redundant_pic_cnt %u\n",data->redundant_pic_cnt);
  }

  nal->decoded = TRUE;
  return 0;
}

/*
 * Look at the start of the picture parameter set.
 *
 * Assumes that this *is* a NAL unit representing a picture parameter set.
 *
 * Don't call this directly - call read_rbsp_data() instead.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int read_pic_param_set_data(nal_unit_p   nal,
                                   int          show_nal_details)
{
  int        err;
  bitdata_p  bd = nal->bit_data;
  nal_pic_param_data_p data = &(nal->u.pic);
  uint32_t   temp;
  // Values that don't get saved into our NAL unit
  uint32_t num_ref_idx_10_active;
  uint32_t num_ref_idx_11_active;
  byte     weighted_pred_flag;
  uint32_t weighted_bipred_idc;
  int32_t   pic_init_qp;
  int32_t   pic_init_qs;
  int32_t   chroma_qp_index_offset;
  byte     deblocking_filter_control_present_flag;
  byte     constrained_intra_pred_flag;

#undef CHECK
#define CHECK(name)                                                      \
  if (err)                                                               \
  {                                                                      \
    fprint_err("### Error reading %s field from picture parameter set\n",\
               (name));                                                  \
  }

  // We need to know the id of this picture parameter set, and also
  // which sequence parameter set it refers to
  err = read_exp_golomb(bd,&temp);
  CHECK("pic_parameter_set_id");
  data->pic_parameter_set_id = temp;
  err = read_exp_golomb(bd,&temp);
  CHECK("seq_parameter_set_id");
  data->seq_parameter_set_id = temp;
  err = read_bit(bd,&data->entropy_coding_mode_flag);
  CHECK("entropy_coding_mode_flag");

  // We care about pic_order_present_flag
  err = read_bit(bd,&data->pic_order_present_flag);
  CHECK("pic_order_present_flag");

  if (show_nal_details)
  {
    fprint_msg("@@ PPS " OFFSET_T_FORMAT_08 "/%04d: size %d\n"
               "   nal_ref_idc %x nal_unit_type %02x (%s)\n",
               nal->unit.start_posn.infile,nal->unit.start_posn.inpacket,
               nal->data_len,
               nal->nal_ref_idc,nal->nal_unit_type,
               NAL_UNIT_TYPE_STR(nal->nal_unit_type));
    fprint_msg("   pic_parameter_set_id %d, seq_parameter_set_id %d\n",
               data->pic_parameter_set_id,data->seq_parameter_set_id);
    fprint_msg("   entropy_coding_mode_flag %d\n",
               data->entropy_coding_mode_flag);
    fprint_msg("   pic_order_present_flag %d\n",data->pic_order_present_flag);
  }

  // After this, we don't (at the moment) really need any of the rest.
  // However, it is moderately useful (for paranoia's sake) to read the
  // redundant_pic_cnt_present_flag at the very end, and we should not
  // be interpreting the inside of a picture parameter set very often...
  
  err = read_exp_golomb(bd,&data->num_slice_groups); // minus 1
  CHECK("num_slice_groups");
  data->num_slice_groups ++;

  if (show_nal_details)
    fprint_msg("   num_slice_groups %u\n",data->num_slice_groups);

  if (data->num_slice_groups > 1)
  {
    err = read_exp_golomb(bd,&data->slice_group_map_type);
    CHECK("slice_group_map_type");
    if (show_nal_details)
      fprint_msg("   slice_group_map_type %u\n",data->slice_group_map_type);
    if (data->slice_group_map_type == 0)
    {
      // NB: 0..num_slice_groups-1, not 0..num_slice_groups-1 - 1
      unsigned int igroup;
      for (igroup=0; igroup < data->num_slice_groups; igroup++)
      {
        uint32_t ignint;
        err = read_exp_golomb(bd,&ignint); // run_length_minus1[igroup]
        CHECK("run_length_minus1[x]");
      }
    }
    else if (data->slice_group_map_type == 2)
    {
      // But this time, 0..num_slice_groups-1 - 1
      unsigned int igroup;
      for (igroup=0; igroup < (data->num_slice_groups - 1); igroup++)
      {
        uint32_t ignint;
        err = read_exp_golomb(bd,&ignint); // top_left[igroup]
        CHECK("top_left[x]");
        err = read_exp_golomb(bd,&ignint); // bottom_right[igroup]
        CHECK("bottom_right[x]");
      }
    }
    else if (data->slice_group_map_type == 3 ||
             data->slice_group_map_type == 4 ||
             data->slice_group_map_type == 5)
    {
      byte     ignbyte;
      uint32_t ignint;
      err = read_bit(bd,&ignbyte); // slice_group_change_direction_flag
      CHECK("slice_group_change_direction_flag");
      err = read_exp_golomb(bd,&ignint); // slice_group_change_rate_minus1
      CHECK("slice_group_change_rate_minus1");
    }
    else if (data->slice_group_map_type == 6)
    {
      uint32_t pic_size_in_map_units;
      unsigned int ii;
      int size;
      err = read_exp_golomb(bd,&pic_size_in_map_units); // minus 1
      pic_size_in_map_units ++;
      CHECK("pic_size_in_map_units");
      if (show_nal_details)
        fprint_msg("   pic_size_in_map_units %u\n",pic_size_in_map_units);
      size = (int) ceil(log2(data->num_slice_groups));
      // Again, notice the range
      for (ii=0; ii < pic_size_in_map_units; ii++)
      {
        uint32_t ignint;
        err = read_bits(bd,size,&ignint); // slice_group_id[ii]
        CHECK("slice_group_id[x]");
      }
    }
  }
  err = read_exp_golomb(bd,&num_ref_idx_10_active); // minus 1
  CHECK("num_ref_idx_10_active");
  num_ref_idx_10_active ++;
  if (show_nal_details)
    fprint_msg("   num_ref_idx_10_active %u\n",num_ref_idx_10_active);
  err = read_exp_golomb(bd,&num_ref_idx_11_active); // minus 1
  CHECK("num_ref_idx_11_active");
  num_ref_idx_11_active ++;
  if (show_nal_details)
    fprint_msg("   num_ref_idx_11_active %u\n",num_ref_idx_11_active);
  err = read_bit(bd,&weighted_pred_flag);
  CHECK("weighted_pred_flag");
  if (show_nal_details)
    fprint_msg("   weighted_pred_flag %d\n",weighted_pred_flag);
  err = read_bits(bd,2,&weighted_bipred_idc);
  CHECK("weighted_bipred_idc");
  if (show_nal_details)
    fprint_msg("   weighted_bipred_idc %u\n",weighted_bipred_idc);
  err = read_signed_exp_golomb(bd,&pic_init_qp); // minus 26
  CHECK("pic_init_qp");
  pic_init_qp += 26;
  if (show_nal_details)
    fprint_msg("   pic_init_qp %d\n",pic_init_qp);
  err = read_signed_exp_golomb(bd,&pic_init_qs); // minus 26
  CHECK("pic_init_qs");
  pic_init_qs += 26;
  if (show_nal_details)
    fprint_msg("   pic_init_qs %d\n",pic_init_qs);
  err = read_signed_exp_golomb(bd,&chroma_qp_index_offset);
  CHECK("chroma_qp_index_offset");
  if (show_nal_details)
    fprint_msg("   chroma_qp_index_offset %d\n",chroma_qp_index_offset);
  err = read_bit(bd,&deblocking_filter_control_present_flag);
  CHECK("deblocking_filter_control_present_flag");
  if (show_nal_details)
    fprint_msg("   deblocking_filter_control_present_flag %d\n",
               deblocking_filter_control_present_flag);
  err = read_bit(bd,&constrained_intra_pred_flag);
  CHECK("constrained_intra_pred_flag");
  if (show_nal_details)
    fprint_msg("   constrained_intra_pred_flag %d\n",constrained_intra_pred_flag);
  // We (sort of) care about redundant_pic_cnt_present_flag
  // in that we need to know it if we are going to read the later bits
  // of a slice header
  err = read_bit(bd,&data->redundant_pic_cnt_present_flag);
  CHECK("redundant_pic_cnt_present_flag");
  if (show_nal_details)
    fprint_msg("   redundant_pic_cnt_present_flag %d\n",
               data->redundant_pic_cnt_present_flag);

  nal->decoded = TRUE;
  return 0;
}

/*
 * Look at the start of the sequence parameter set.
 *
 * Assumes that this *is* a NAL unit representing a sequence parameter set.
 *
 * Don't call this directly - call read_rbsp_data() instead.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int read_seq_param_set_data(nal_unit_p   nal,
                                   int          show_nal_details)
{
  int        err;
  bitdata_p  bd = nal->bit_data;
  nal_seq_param_data_p data = &(nal->u.seq);
  uint32_t   temp;
  // Values that don't get saved into our NAL unit
  byte     reserved_zero_5bits;
  uint32_t num_ref_frames;
  byte     gaps_in_frame_num_value_allowed_flag;
  uint32_t pic_width_in_mbs;
  uint32_t pic_height_in_map_units;

#undef CHECK
#define CHECK(name)                                                       \
  if (err)                                                                \
  {                                                                       \
    fprint_err("### Error reading %s field from sequence parameter set\n",\
               (name));                                                   \
  }

  err = read_bits_into_byte(bd,8,&data->profile_idc);
  CHECK("profile_idc");
  err = read_bit(bd,&data->constraint_set0_flag);
  CHECK("constraint_set0_flag");
  err = read_bit(bd,&data->constraint_set1_flag);
  CHECK("constraint_set1_flag");
  err = read_bit(bd,&data->constraint_set2_flag);
  CHECK("constraint_set2_flag");
  err = read_bits_into_byte(bd,5,&reserved_zero_5bits);
  CHECK("reserved_zero_5bits");

  if (reserved_zero_5bits != 0)
  {
    fprint_err("### reserved_zero_5bits not zero (%d) in sequence"
               " parameter set NAL unit at " OFFSET_T_FORMAT "/%d\n",
               reserved_zero_5bits,
               nal->unit.start_posn.infile,nal->unit.start_posn.inpacket);
    print_data(FALSE,"   Data",nal->bit_data->data,nal->bit_data->data_len,
               20);
    // Should we carry on or give up? On the whole, if this is broken
    // we can't really trust the rest of its data...
    return 1;
  }

  err = read_bits_into_byte(bd,8,&data->level_idc);
  CHECK("level_idc");

  if (show_nal_details)
  {
    fprint_msg("@@ SPS " OFFSET_T_FORMAT_08 "/%04d: size %d\n"
               "   nal_ref_idc %x nal_unit_type %02x (%s)\n",
               nal->unit.start_posn.infile,nal->unit.start_posn.inpacket,
               nal->data_len,
               nal->nal_ref_idc,nal->nal_unit_type,
               NAL_UNIT_TYPE_STR(nal->nal_unit_type));
    fprint_msg("   profile_idc %u, constraint set flags: %d %d %d\n",
               data->profile_idc,
               data->constraint_set0_flag,
               data->constraint_set1_flag,
               data->constraint_set2_flag);
    fprint_msg("   level_idc %u\n",data->level_idc);
  }

  err = read_exp_golomb(bd,&temp);
  CHECK("seq_parameter_set_id");
  data->seq_parameter_set_id = temp;
  // We care about log2_max_frame_num_minus4
  err = read_exp_golomb(bd,&data->log2_max_frame_num); // minus 4
  CHECK("log2_max_frame_num");
  data->log2_max_frame_num += 4;
  // We care about pic_order_cnt_type
  err = read_exp_golomb(bd,&data->pic_order_cnt_type);
  CHECK("pic_order_cnt_type");

  if (show_nal_details)
  {
    fprint_msg("   seq_parameter_set_id %u\n",data->seq_parameter_set_id);
    fprint_msg("   log2_max_frame_num %u\n",data->log2_max_frame_num);
    fprint_msg("   pic_order_cnt_type %u\n",data->pic_order_cnt_type);
  }
  
  if (data->pic_order_cnt_type == 0)
  {
    err = read_exp_golomb(bd,&data->log2_max_pic_order_cnt_lsb); // minus 4
    CHECK("log2_max_pic_order_cnt_lsb");
    data->log2_max_pic_order_cnt_lsb += 4;
    if (show_nal_details)
      fprint_msg("   log2_max_pic_order_cnt_lsb %u\n",
                 data->log2_max_pic_order_cnt_lsb);
  }
  else if (data->pic_order_cnt_type == 1)
  {
    unsigned int ii;
    int32_t offset_for_non_ref_pic;
    int32_t offset_for_top_to_bottom_field;
    uint32_t num_ref_frames_in_pic_order_cnt_cycle;

    err = read_bit(bd,&data->delta_pic_order_always_zero_flag);
    CHECK("delta_pic_order_always_zero_flag");
    if (show_nal_details)
      fprint_msg("   delta_pic_order_always_zero_flag %d\n",
                 data->delta_pic_order_always_zero_flag);
    err = read_signed_exp_golomb(bd,&offset_for_non_ref_pic);
    CHECK("offset_for_non_ref_pic");
    err = read_signed_exp_golomb(bd,&offset_for_top_to_bottom_field);
    CHECK("offset_for_top_to_bottom_field");
    err = read_exp_golomb(bd,&num_ref_frames_in_pic_order_cnt_cycle);
    CHECK("num_ref_frames_in_pic_order_cnt_cycle");
    // The standard says that num_ref_frames_in_pic_order_cnt_cycle
    // shall be in the range 0..255
    for (ii=0; ii < num_ref_frames_in_pic_order_cnt_cycle; ii++)
    {
      int32_t offset_for_ref_frame_XX;
      err = read_signed_exp_golomb(bd,&offset_for_ref_frame_XX); // XX = [ii]
      CHECK("offset_for_ref_frame_X");
    }
  }
  err = read_exp_golomb(bd,&num_ref_frames);
  CHECK("num_ref_frames");
  if (show_nal_details)
    fprint_msg("   num_ref_frames %u\n",num_ref_frames);
  err = read_bit(bd,&gaps_in_frame_num_value_allowed_flag);
  CHECK("gaps_in_frame_num_value_allowed_flag");
  if (show_nal_details)
  if (show_nal_details)
    fprint_msg("   gaps_in_frame_num_value_allowed_flag %d\n",
               gaps_in_frame_num_value_allowed_flag);
  err = read_exp_golomb(bd,&pic_width_in_mbs); // minus 1
  CHECK("pic_width_in_mbs");
  pic_width_in_mbs ++;
  if (show_nal_details)
    fprint_msg("   pic_width_in_mbs %u\n",pic_width_in_mbs);
  err = read_exp_golomb(bd,&pic_height_in_map_units); // minus 1
  CHECK("pic_height_in_map_units");
  pic_height_in_map_units ++;
  if (show_nal_details)
    fprint_msg("   pic_height_in_map_units %u\n",pic_height_in_map_units);
  // We care about frame_mbs_only_flag
  err = read_bit(bd,&data->frame_mbs_only_flag);
  CHECK("frame_mbs_only_flag");
  if (show_nal_details)
    fprint_msg("   frame_mbs_only_flag %d\n",data->frame_mbs_only_flag);

  nal->decoded = TRUE;
  return 0;
}

/*
 * Read the data for an SEI recovery point
 *
 * Returns 0 if it succeeds, 1 if some error occurs
 */
static int read_SEI_recovery_point(nal_unit_p   nal,
                                   int          payloadSize,
                                   int          show_nal_details)
{
  int        err;
  bitdata_p  bd = nal->bit_data;
  nal_SEI_recovery_data_p data = &(nal->u.sei_recovery);
  uint32_t   temp;

#undef CHECK
#define CHECK(name)                                     \
  if (err)                                              \
  {                                                     \
    fprint_err("### Error reading %s field from SEI\n", \
               (name));                                 \
  }

  err = read_exp_golomb(bd,&temp);
  CHECK("recovery_frame_cnt");
  data->recovery_frame_cnt = temp;
  err = read_bit(bd,&data->exact_match_flag);
  CHECK("exact_match_flag");
  err = read_bit(bd,&data->broken_link_flag);
  CHECK("broken_link_flag");

  err = read_bits(bd,2,&data->changing_slice_group_idc);
  CHECK("changing_slice_group_idc");

  nal->decoded = TRUE; 

  if (show_nal_details)
  {
    print_msg("@@ Recovery Point SEI\n");
    fprint_msg("   recovery_frame_cnt %d\n   exact_match_flag %d\n", data->recovery_frame_cnt, data->exact_match_flag);
    fprint_msg("   broken_link_flag %d\n   changing_slice_group_idc %d", data->broken_link_flag, data->changing_slice_group_idc);
  }

  return 0;
}

/*
 * Look at the start of an SEI
 *
 * Assumes that this *is* a NAL unit representing an SEI
 *
 * Don't call this directly - call read_rbsp_data() instead.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int read_SEI(nal_unit_p   nal,
                    int          show_nal_details)
{
  int err;
  int SEI_payloadType = 0;
  int SEI_payloadSize = 0;   // in byte
  bitdata_p  bd = nal->bit_data;
  uint32_t temp = 0;

#undef CHECK
#define CHECK(name)                                     \
  if (err)                                              \
  {                                                     \
    fprint_err("### Error reading %s field from SEI\n", \
               (name));                                 \
  }

  // read payloadtype (see H.264:7.3.2.3.1)
  for (;;)
  {
    err = read_bits(bd,8,&temp);
    CHECK("payloadType");
    if (temp == 0xff)
      SEI_payloadType += 0xff;
    else
      break;
  }
  SEI_payloadType += temp;
  nal->u.sei_recovery.payloadType = SEI_payloadType;

  // read payloadSize
  for (;;)
  {
    err = read_bits(bd,8,&temp);
    CHECK("payloadSize");
    if (temp == 0xff)
      SEI_payloadSize += 0xff;
    else
      break;
  }
  SEI_payloadSize += temp;
  nal->u.sei_recovery.payloadSize = SEI_payloadSize;

  if (SEI_payloadType == 6)  // SEI recovery_point
    err = read_SEI_recovery_point(nal, SEI_payloadSize, show_nal_details); 
  
  return 0;
}

/*
 * Look at the start of the RBSP for a NAL unit
 *
 * Decodes some or all of the data for slices, sequence parameter sets
 * and picture parameter sets.
 *
 * (Note that calling this more than once does not read the data
 * more than once. Also note that the RBSP and bitdata datastructures
 * in the NAL unit do not persist after this call.)
 *
 *     Caveat: if either `seq_param_dict` or `pic_param_dict` is NULL, and the
 *     NAL unit being interpreted is an IDR or non-IDR unit (i.e., a slice),
 *     then only the first few values in the RBSP will be read (up to and
 *     including the slice_type and pic_parameter_set_id), and the RBSP will
 *     *not* be marked as decoded. Because of this, calling this
 *     function again later will cause the RBSP to be read again (strictly,
 *     to be re-extracted and read again).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
static int read_rbsp_data(nal_unit_p   nal,
                          param_dict_p seq_param_dict,
                          param_dict_p pic_param_dict,
                          int          show_nal_details)
{
  int err = 0;

  if (nal->decoded)
    return 0;

  err = prepare_rbsp(nal);
  if (err) return err;
  
  if (nal->nal_unit_type == 1 || nal->nal_unit_type == 5) // Coded slice of a (non) IDR picture 
    err = read_slice_data(nal,seq_param_dict,pic_param_dict,show_nal_details);
  else if (nal->nal_unit_type == 8)                       // Picture parameter set
    err = read_pic_param_set_data(nal,show_nal_details);
  else if (nal->nal_unit_type == 7)                       // Sequence parameter set
    err = read_seq_param_set_data(nal,show_nal_details);
  else if (nal->nal_unit_type == 6)                       // SEI 
    err = read_SEI(nal,show_nal_details);
  else if (show_nal_details)
    fprint_msg("@@ nal " OFFSET_T_FORMAT_08 "/%04d: size %d\n"
               "   nal_ref_idc %x nal_unit_type %02x (%s)\n",
               nal->unit.start_posn.infile,nal->unit.start_posn.inpacket,
               nal->data_len,
               nal->nal_ref_idc,nal->nal_unit_type,
               NAL_UNIT_TYPE_STR(nal->nal_unit_type));

  if (err)
  {
    fprint_err("### Error reading RBSP data for %s NAL (ref idc %x,"
               " unit type %x) at " OFFSET_T_FORMAT_08 "/%04d\n",
               NAL_UNIT_TYPE_STR(nal->nal_unit_type),
               nal->nal_ref_idc,
               nal->nal_unit_type,
               nal->unit.start_posn.infile,
               nal->unit.start_posn.inpacket);
  }

  // At this point, we've finished with the actual RBSP data
  // so we might as well free it and save some space.
  if (nal->rbsp != NULL)
  {
    free(nal->rbsp);
    nal->rbsp = NULL;
    nal->rbsp_len = 0;
    free_bitdata(&nal->bit_data);
  }
  return err;
}

/*
 * Is this NAL unit a slice?
 *
 * Returns true if its ``nal_unit_type`` is 1 (coded slice of IDR picture)
 * or 5 (coded slice of IDR picture).
 */
extern int nal_is_slice(nal_unit_p  nal)
{
  return nal->nal_unit_type == 1 || nal->nal_unit_type == 5;
}

/*
 * Is this NAL unit a picture parameter set?
 *
 * Returns true if its ``nal_unit_type`` is 8.
 */
extern int nal_is_pic_param_set(nal_unit_p  nal)
{
  return nal->nal_unit_type == 8;
}

/*
 * Is this NAL unit a sequence parameter set?
 *
 * Returns true if its ``nal_unit_type`` is 7.
 */
extern int nal_is_seq_param_set(nal_unit_p  nal)
{
  return nal->nal_unit_type == 7;
}

/*
 * Is this NAL unit marked as part of a redundant picture?
 */
extern int nal_is_redundant(nal_unit_p  nal)
{
  return nal_is_slice(nal) && nal->u.slice.redundant_pic_cnt_present &&
    nal->u.slice.redundant_pic_cnt;
}

/*
 * Is this VCL NAL unit the first of a new primary coded picture?
 *
 * - `nal` is the NAL unit we need to decide about.
 * - `last` is a slice NAL unit from the last primary coded picture
 *    (likely to be the first NAL unit therefrom, in fact)
 *
 * Both `nal` and `last` must be VCL NALs representing slices of a reference
 * picture - i.e., with nal_unit_type 1 or 5 (if we were supporting type A
 * slice data partitions, we would have to take them into account as well).
 *
 * Both `nal` and `last` must have had their innards decoded with
 * `read_slice_data`, which should have occurred automatically if they are
 * both appropriate NAL units for this process.
 *
 * Acording to H.264 7.4.1.2.4 (from the JVT-J010d7 draft):
 *     
 *   The first NAL unit of a new primary code picture can be detected
 *   because:
 *
 *   - its frame number differs in value from that of the last slice (NB:
 *     IDR pictures always have frame_num == 0)
 *
 *   - its field_pic_flag differs in value (i.e., one is a field slice, and
 *     the other a frame slice)
 *
 *   - the bottom_field_flag is present in both (determined by
 *     frame_mbs_only_flag in the sequence parameter set, and by
 *     field_pic_flag) and differs (i.e., both are field slices, but one
 *     is top and the other bottom) [*]_
 *
 *   - nal_ref_idc differs in value, and one of them has nal_ref_idc == 0
 *     (i.e., one is a reference picture and the other is not)
 *
 *   - pic_order_cnt_type (found in the sequence parameter set) == 0 for
 *     both and either pic_order_cnt_lsb differs in value or
 *     delta_pic_order_cnt_bottom differs in value [*]_
 *
 *   - pic_order_cnt_type == 1 for both and either delta_pic_order_cnt[0]
 *     or delta_pic_order_cnt[1] differs in value [*]_
 *
 *   - nal_unit_type == 5 for one and not in the other (i.e., one is IDR
 *     and the other is not)
 *
 *   - nal_unit_type == 5 for both (i.e., both are IDR), and idr_pic_id
 *     differs (i.e., they're not the same IDR)
 *
 * It is possible that later drafts may alter/augment these criteria -
 * that has already happened between JVT-G050r1 and JVT-J010d7.
 *
 * .. [*] For these three items, we need to have decoded the active
 *    sequence parameter set (which, for now, I'll assume to be the last
 *    set we found with the appropriate id).
 */
extern int nal_is_first_VCL_NAL(nal_unit_p   nal,
                                nal_unit_p   last)
{
  nal_slice_data_p  this,that;
  
  if (nal->starts_picture_decided)
    return nal->starts_picture;

  if (!nal->decoded)
  {
    print_err("### Cannot decide if NAL unit is first VCL NAL\n"
              "    its RBSP data has not been interpreted\n");
    return FALSE;
  }
  
  // Since we intend to transmit all sequence and picture parameter
  // sets, at least initially, we don't need to worry if they are
  // "inside" a picture, at its start, or whatever.

  // Since we're not supporting data partition slices A,B,C, we can
  // ignore them as well (at least, so I hope)

  if (nal->nal_unit_type != NAL_NON_IDR &&
      nal->nal_unit_type != NAL_IDR)
  {
    nal->starts_picture = FALSE;
    nal->starts_picture_decided = TRUE;
    return FALSE;
  }

  nal->starts_picture = TRUE;  // let's be optimistic...
  nal->starts_picture_decided = TRUE;

  if (last == NULL)
  {
    // With nothing else to compare to, we shall assume that we do
    // "start" a picture
    nal->start_reason = "First slice in data stream";
    return TRUE;
  }

  this = &(nal->u.slice);
  that = &(last->u.slice);

  if (this->frame_num != that->frame_num)
    nal->start_reason = "Frame number differs";
  else if (this->field_pic_flag != that->field_pic_flag)
    nal->start_reason = "One is field, the other frame";
  else if (this->bottom_field_flag_present &&
           that->bottom_field_flag_present &&
           this->bottom_field_flag != that->bottom_field_flag)
  {
    nal->start_reason = "One is bottom field, the other top";
    // In which case, we'll need to remember to output the OTHER
    // half of the frame when we find it...
  }
  else if (nal->nal_ref_idc != last->nal_ref_idc &&
           (nal->nal_ref_idc == 0 || last->nal_ref_idc == 0))
    nal->start_reason = "One is reference picture, the other is not";
  else if (this->seq_param_set_pic_order_cnt_type == 0 &&
           that->seq_param_set_pic_order_cnt_type == 0 &&
           (this->pic_order_cnt_lsb != that->pic_order_cnt_lsb ||
            this->delta_pic_order_cnt_bottom !=
            that->delta_pic_order_cnt_bottom))
    nal->start_reason = "Picture order counts differ";
  else if (this->seq_param_set_pic_order_cnt_type == 1 &&
           that->seq_param_set_pic_order_cnt_type == 1 &&
           (this->delta_pic_order_cnt[0] != that->delta_pic_order_cnt[0] ||
            this->delta_pic_order_cnt[1] != that->delta_pic_order_cnt[1]))
    nal->start_reason = "Picture delta counts differ";
  else if ((nal->nal_unit_type == 5 || last->nal_unit_type == 5) &&
           nal->nal_unit_type != last->nal_unit_type)
    nal->start_reason = "One IDR, one not";
  else if (nal->nal_unit_type == 5 && last->nal_unit_type == 5 &&
           this->idr_pic_id != that->idr_pic_id)
    nal->start_reason = "Different IDRs";
  else
    nal->starts_picture = FALSE;

  return nal->starts_picture;
}

/*
 * Print out useful information about this NAL unit, on the given stream.
 *
 * This is intended as a single line of information.
 */
extern void report_nal(int         is_msg,
                       nal_unit_p  nal)
{
  if (nal == NULL)
    fprint_msg_or_err(is_msg,".............: NAL unit <null>\n");
  else if (nal_is_slice(nal) && (nal->nal_unit_type == NAL_IDR ||
                                 nal->nal_unit_type == NAL_NON_IDR))
  {
    #define SARRAYSIZE 20
    char what[SARRAYSIZE];
    snprintf(what,SARRAYSIZE,"(%s)",NAL_UNIT_TYPE_STR(nal->nal_unit_type));
    // On Windows, snprintf does not guarantee to write a terminating NULL
    what[SARRAYSIZE-1] = '\0'; 
    fprint_msg_or_err(is_msg,OFFSET_T_FORMAT_08 "/%04d: %x/%02x %-20s %u (%s) frame %u",
                      nal->unit.start_posn.infile,
                      nal->unit.start_posn.inpacket,
                      nal->nal_ref_idc,
                      nal->nal_unit_type,
                      what,
                      nal->u.slice.slice_type,
                      NAL_SLICE_TYPE_STR(nal->u.slice.slice_type),
                      nal->u.slice.frame_num);
    if (nal->u.slice.field_pic_flag)
    {
      if (nal->u.slice.bottom_field_flag)
        fprint_msg_or_err(is_msg," [bottom]");
      else
        fprint_msg_or_err(is_msg," [top]");
    }
  }
  else if (nal_is_seq_param_set(nal))
  {
    fprint_msg_or_err(is_msg,OFFSET_T_FORMAT_08 "/%04d: %x/%02x (%s %u)",
                      nal->unit.start_posn.infile,
                      nal->unit.start_posn.inpacket,
                      nal->nal_ref_idc,
                      nal->nal_unit_type,
                      NAL_UNIT_TYPE_STR(nal->nal_unit_type),
                      nal->u.seq.seq_parameter_set_id);
  }
  else if (nal_is_pic_param_set(nal))
  {
    fprint_msg_or_err(is_msg,OFFSET_T_FORMAT_08 "/%04d: %x/%02x (%s %u)",
                      nal->unit.start_posn.infile,
                      nal->unit.start_posn.inpacket,
                      nal->nal_ref_idc,
                      nal->nal_unit_type,
                      NAL_UNIT_TYPE_STR(nal->nal_unit_type),
                      nal->u.pic.pic_parameter_set_id);
  }
  else
    fprint_msg_or_err(is_msg,OFFSET_T_FORMAT_08 "/%04d: %x/%02x (%s)",
                      nal->unit.start_posn.infile,
                      nal->unit.start_posn.inpacket,
                      nal->nal_ref_idc,
                      nal->nal_unit_type,
                      NAL_UNIT_TYPE_STR(nal->nal_unit_type));
#if REPORT_NAL_SHOWS_ADDRESS
  fprint_msg_or_err(is_msg," <%p>",nal);
#endif
  fprint_msg_or_err(is_msg,"\n");
}

// ------------------------------------------------------------
// Check profile
// ------------------------------------------------------------
/*
 * Issue a warning if the profile of this bitstream is "unsuitable".
 *
 * "suitable" bitstream declares itself as either conforming to
 * the main profile, or as obeying the constraints of the main profile.
 *
 * This function should be called on the first sequence parameter set
 * NAL unit in the bitstream, after its innards have been decoded.
 */
static void check_profile(nal_unit_p  nal,
                          int         show_nal_details)
{
  struct nal_seq_param_data  data;
  char *name;

  if (nal == NULL)
  {
    print_err("### Attempt to check profile on a NULL NAL unit\n");
    return;
  }
  else if (nal->nal_unit_type != 7)
  {
    print_err("### Attempt to check profile on a NAL unit that is not a "
              "sequence parameter set\n");
    report_nal(FALSE,nal);
    return;
  }
  else if (!nal->decoded)
  {
    // Note that we believe ourselves safe in passing NULLs for the
    // parameter NAL units, since we are reading a sequence parameter set,
    // which does not depend on anything else
    int err = read_rbsp_data(nal,NULL,NULL,show_nal_details);
    if (err)
    {
      print_err("### Error trying to decode RBSP for first sequence"
                " parameter set\n");
      return;
    }
  }
  
  data = nal->u.seq;
  name = (data.profile_idc==66?"baseline":
          data.profile_idc==77?"main":
          data.profile_idc==88?"extended":"<unknown>");

  if (data.profile_idc == 77 || data.constraint_set1_flag == 1)
    return;
  else
  {
    int sum = data.constraint_set0_flag + data.constraint_set1_flag +
      data.constraint_set2_flag;
    print_err("\n");
    fprint_err("Warning: This bitstream declares itself as %s profile (%d)",
               name,data.profile_idc);
    if (sum == 0)
      print_err(".\n");
    else
    {
      print_err(",\n");
      print_err("         and as obeying the constraints of the");
      if (data.constraint_set0_flag) print_err(" baseline");
      if (data.constraint_set1_flag) print_err(" main");
      if (data.constraint_set2_flag) print_err(" extended");
      fprint_err(" profile%s.\n",(sum==1?"":"s"));
    }
    fprint_err("         This software does not support %s profile,\n",
               name);
    print_err("         and may give incorrect results or fail.\n\n");
    return;
  }
}

// ------------------------------------------------------------
// NAL unit *data* stuff
// ------------------------------------------------------------
/*
 * Once we've read the *data* for a NAL unit, we can set up a bit more
 * of the datastructure.
 *
 * - `verbose` is true if a brief report on the NAL unit should be given
 * - `nal` is the NAL unit itself.
 *
 * CAVEAT: This function is declared external so that I can use it
 * in `stream_type.c`, but it is not exported to the nalunit_defns.h file
 * because I cannot see any sensible outside use for it...
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int setup_NAL_data(int         verbose,
                          nal_unit_p  nal)
{
  int forbidden_zero_bit;

  // Although we've read "all" the data (i.e., including the prefix),
  // for most purposes of working on it it's more convenient to act
  // as if those bytes aren't there...
  nal->data = &(nal->unit.data[3]);
  nal->data_len = nal->unit.data_len - 3;

  // The first byte of our data tells us what sort of NAL unit it is
  forbidden_zero_bit = nal->data[0] & 0x80;
  if (forbidden_zero_bit)
  {
    fprint_err("### NAL forbidden_zero_bit is non-zero, at "
               OFFSET_T_FORMAT "/%d\n",
               nal->unit.start_posn.infile,nal->unit.start_posn.inpacket);
    fprint_err("    First byte of NAL unit is %02x",nal->data[0]);
    if (nal->data[0] == 0xB3)
      print_err(", which is H.262 sequence header start code\n"
                "    Data may be MPEG-1 or MPEG-2");
    print_err("\n");
    return 1;
  }
  nal->nal_ref_idc = (nal->data[0] & 0x60) >> 5;
  nal->nal_unit_type = (nal->data[0] & 0x1F);  

  if (verbose)
  {
    #define SARRAYSIZE2 20
    char what[SARRAYSIZE2];
    snprintf(what,SARRAYSIZE2,"(%s)",NAL_UNIT_TYPE_STR(nal->nal_unit_type));
    // On Windows, snprintf does not guarantee to write a terminating NULL
    what[SARRAYSIZE2-1] = '\0'; 
    fprint_msg(OFFSET_T_FORMAT_08 "/%04d: NAL unit %d/%d %-20s",
               nal->unit.start_posn.infile,
               nal->unit.start_posn.inpacket,
               nal->nal_ref_idc,nal->nal_unit_type,what);
    
    // Show the start of the data bytes. This is a tailored form of what
    // `print_data` would do, more suited to our purposes here (i.e.,
    // wanting multiple rows of output to line up neatly in columns).
    if (nal->data_len > 0)
    {
      int ii;
      int show_len = (nal->data_len>10?10:nal->data_len);
      fprint_msg(" %6d:",nal->data_len);
      for (ii = 0; ii < show_len; ii++)
        fprint_msg(" %02x",nal->data[ii]);
      if (show_len < nal->data_len)
        print_msg("...");
    }
    print_msg("\n");
  }
  return 0;
}

/*
 * Find and read in the next NAL unit.
 *
 * - `context` is the NAL unit context we're reading from
 * - `verbose` is true if a brief report on the NAL unit should be given
 * - `nal` is the datastructure containing the NAL unit found, or NULL
 *   if there was none.
 *
 * Returns:
 * * 0 if it succeeds,
 * * EOF if the end-of-file is read (i.e., there is no next NAL unit),
 * * 2 if the NAL unit data does not make sense, so it should be ignored
 *   (specifically, if the NAL unit's RBSP data cannot be understood),
 * * 1 if some other error occurs.
 */
extern int find_next_NAL_unit(nal_unit_context_p  context,
                              int                 verbose,
                              nal_unit_p         *nal)
{
  static int need_first_seq_param_set = TRUE;
  int      err;

  err = build_nal_unit(nal);
  if (err) return 1;
  
  err = find_next_ES_unit(context->es,&(*nal)->unit);
  if (err) // 1 or EOF
  {
    free_nal_unit(nal);
    return err;
  }

  (context->count) ++;

  if (context->show_nal_details)
    print_msg("\n");

  err = setup_NAL_data(verbose,*nal);
  if (err)
  {
    free_nal_unit(nal);
    return err;
  }
  
  // By looking at the first sequence parameter set, we can
  // decide whether the data matches what we claim to support.
  // That also serves to bootstrap the decoding of other items
  if (nal_is_seq_param_set(*nal))
  {
    if (need_first_seq_param_set)
    {
      check_profile(*nal,context->show_nal_details);
      need_first_seq_param_set = FALSE;
    }
  }

  // Once we know we've got the first sequence parameter set in hand
  // (which we *assume* and hope is the first thing we find!), we can
  // decode the innards of later things.
  err = read_rbsp_data(*nal,context->seq_param_dict,context->pic_param_dict,
                       context->show_nal_details);
  if (err)
  {
    free_nal_unit(nal);
    return 2;
  }
 
  // If this is a picture parameter set, or a sequence parameter set,
  // we should remember it for later on
  if (nal_is_pic_param_set(*nal))
  {
    err = remember_param_data(context->pic_param_dict,
                              (*nal)->u.pic.pic_parameter_set_id,*nal);
    if (err)
    {
      print_err("### Error remembering picture parameter set ");
      report_nal(FALSE,*nal);
      free_nal_unit(nal);
      return 1;
    }
  }
  else if (nal_is_seq_param_set(*nal))
  {
    err = remember_param_data(context->seq_param_dict,
                              (*nal)->u.seq.seq_parameter_set_id,*nal);
    if (err)
    {
      print_err("### Error remembering sequence parameter set ");
      report_nal(FALSE,*nal);
      free_nal_unit(nal);
      return 1;
    }
  }

  return 0;
}

/*
 * Write (copy) the current NAL unit to the ES output stream.
 *
 * - `output` is the output stream (file descriptor) to write to
 * - `nal` is the NAL unit to write
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int write_NAL_unit_as_ES(FILE       *output,
                                nal_unit_p  nal)
{
  int err = write_ES_unit(output,&(nal->unit));
  if (err)
  {
    print_err("### Error writing NAL unit as ES\n");
    return err;
  }
  else
    return 0;
}

/*
 * Write (copy) the current NAL unit to the output stream, wrapped up in a
 * PES within TS.
 *
 * - `output` is the TS writer to write to
 * - `nal` is the NAL unit to write
 * - `video_pid` is the video PID to use
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int write_NAL_unit_as_TS(TS_writer_p tswriter,
                                nal_unit_p  nal,
                                uint32_t    video_pid)
{
  // Note that we write out *all* of the data for this NAL unit,
  // i.e., including its 00 00 01 prefix. Also note that we write
  // out the data with its emulation prevention 03 bytes intact.
  int err = write_ES_as_TS_PES_packet(tswriter,
                                      nal->unit.data,nal->unit.data_len,
                                      video_pid,DEFAULT_VIDEO_STREAM_ID);
  if (err)
  {
    print_err("### Error writing NAL unit as TS\n");
    return err;
  }
  else
    return 0;
}

// ------------------------------------------------------------
// Picture and sequence parameter set support
// ------------------------------------------------------------
/*
 * Create a new "dictionary" for remembering picture or sequence
 * parameter sets.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_param_dict(param_dict_p *param_dict)
{
  param_dict_p  new = malloc(SIZEOF_PARAM_DICT);
  if (new == NULL)
  {
    print_err("### Unable to allocate parameter 'dictionary' datastructure\n");
    return 1;
  }

  new->last_id = -1;
  new->last_index = -1;

  new->ids = malloc(sizeof(uint32_t)*NAL_PIC_PARAM_START_SIZE);
  if (new->ids == NULL)
  {
    print_err("### Unable to allocate array within 'dictionary'"
              " datastructure\n");
    free(new);
    return 1;
  }

  new->params = malloc(SIZEOF_NAL_INNARDS*NAL_PIC_PARAM_START_SIZE);
  if (new->params == NULL)
  {
    print_err("### Unable to allocate array within 'dictionary'"
              " datastructure\n");
    free(new->ids);
    free(new);
    return 1;
  }

  new->posns = malloc(SIZEOF_ES_OFFSET*NAL_PIC_PARAM_START_SIZE);
  if (new->posns == NULL)
  {
    print_err("### Unable to allocate array within 'dictionary'"
              " datastructure\n");
    free(new->params);
    free(new->ids);
    free(new);
    return 1;
  }

  new->data_lens = malloc(sizeof(uint32_t)*NAL_PIC_PARAM_START_SIZE);
  if (new->data_lens == NULL)
  {
    print_err("### Unable to allocate array within 'dictionary'"
              " datastructure\n");
    free(new->params);
    free(new->ids);
    free(new);
    return 1;
  }
  
  new->size = NAL_PIC_PARAM_START_SIZE;
  new->length = 0;
  
  *param_dict = new;
  return 0;
}

/*
 * Tidy up and free a parameters "dictionary" datastructure after we've
 * finished with it.
 *
 * Empties the datastructure, frees it, and sets `param_dict` to NULL.
 *
 * Does nothing if `param_dict` is already NULL.
 */
extern void free_param_dict(param_dict_p  *param_dict)
{
  if (*param_dict == NULL)
    return;
  free((*param_dict)->ids);
  free((*param_dict)->params);
  free((*param_dict)->posns);
  free((*param_dict)->data_lens);
  (*param_dict)->ids = NULL;
  (*param_dict)->params = NULL;
  (*param_dict)->posns = NULL;
  (*param_dict)->data_lens = NULL;
  free(*param_dict);
  *param_dict = NULL;
}

/*
 * Remember parameter set data in a "dictionary".
 *
 * - `param_dict` should be an appropriate "dictionary" - i.e., one
 *   being used to store picture or sequence parameter set data, as
 *   appropriate.
 * - `param_id` is the id for this picture or sequence parameter set.
 * - `nal` is the NAL unit containing the parameter set data.
 *   Note that a copy will be taken of the parameter set data, which
 *   means that the caller may free the NAL unit.
 *
 * Any previous data for this picture or sequence parameter set id will be
 * forgotten (overwritten).
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int remember_param_data(param_dict_p  param_dict,
                               uint32_t      param_id,
                               nal_unit_p    nal)
{
  int ii;
  if (param_id == param_dict->last_id)
  {
    param_dict->params[param_dict->last_index] = nal->u;
    return 0;
  }

  for (ii=0; ii<param_dict->length; ii++)
  {
    if (param_dict->ids[ii] == param_id)
    {
      param_dict->params[ii] = nal->u;
      param_dict->posns[ii] = nal->unit.start_posn;
      param_dict->data_lens[ii] = nal->unit.data_len;
      param_dict->last_id = param_id;
      param_dict->last_index = ii;
      return 0;
    }
  }

  if (param_dict->length == param_dict->size)
  {
    int newsize = param_dict->size + NAL_PIC_PARAM_INCREMENT;
    param_dict->ids = realloc(param_dict->ids,newsize*sizeof(uint32_t));
    if (param_dict->ids == NULL)
    {
      print_err("### Unable to extend parameter set dictionary array\n");
      return 1;
    }
    param_dict->params = realloc(param_dict->params,
                                 newsize*SIZEOF_NAL_INNARDS);
    if (param_dict->params == NULL)
    {
      print_err("### Unable to extend parameter set dictionary array\n");
      return 1;
    }
    param_dict->posns = realloc(param_dict->params,newsize*SIZEOF_ES_OFFSET);
    if (param_dict->posns == NULL)
    {
      print_err("### Unable to extend parameter set dictionary array\n");
      return 1;
    }
    param_dict->data_lens = realloc(param_dict->params,
                                    newsize*sizeof(uint32_t));
    if (param_dict->data_lens == NULL)
    {
      print_err("### Unable to extend parameter set dictionary array\n");
      return 1;
    }
    param_dict->size = newsize;
  }
  param_dict->ids[param_dict->length] = param_id;
  param_dict->params[param_dict->length] = nal->u;
  param_dict->posns[param_dict->length] = nal->unit.start_posn;
  param_dict->data_lens[param_dict->length] = nal->unit.data_len;
  param_dict->last_id = param_id;
  param_dict->last_index = param_dict->length;
  param_dict->length++;
  return 0;
}

/*
 * Look up a parameter set id in the "dictionary".
 *
 * - `param_dict` is a parameter "dictionary" of the appropriate type.
 * - `param_id` is the id to look up.
 * - `param_data` is the data for that id. Do not free this, it refers
 *   into the "dictionary" datastructure.
 *
 * Note that calls of `remember_param_data()` (i.e., altering the
 * "dictionary") may cause the underlying datastructures to be realloc'ed,
 * which in turn means that the address returned as `param_data` may not be
 * valid after such a call.
 *
 * Returns 0 if it succeeds, 1 if the id is not present.
 */
static inline int lookup_param_data(param_dict_p      param_dict,
                                    uint32_t          param_id,
                                    nal_innards_p    *param_data)
{
  int ii;
  for (ii=0; ii<param_dict->length; ii++)
  {
    if (param_dict->ids[ii] == param_id)
    {
      *param_data = &param_dict->params[ii];
      param_dict->last_id = param_id;
      param_dict->last_index = ii;
      return 0;
    }
  }
  return 1;
}

/*
 * Retrieve the picture parameter set data for the given id.
 *
 * - `pic_param_dict` is a parameter "dictionary" of the appropriate type.
 * - `pic_param_id` is the id to look up.
 * - `pic_param_data` is the data for that id. Do not free this, it refers
 *   into the "dictionary" datastructure.
 *
 * Note that altering the "dictionary" (with `remember_param_data()`) may
 * cause the underlying datastructures to be realloc'ed, which in turn means
 * that the address returned as `pic_param_data` may not be valid after such
 * an action.
 *
 * Returns 0 if it succeeds, 1 if the id is not recognised.
 */
extern int get_pic_param_data(param_dict_p pic_param_dict,
                              uint32_t     pic_param_id,
                              nal_pic_param_data_p *pic_param_data)
{
  nal_innards_p  innards;
  int absent = lookup_param_data(pic_param_dict,pic_param_id,&innards);
  if (absent)
  {
    fprint_err("### Unable to find picture parameter set with id %u\n",
               pic_param_id);
    return 1;
  }
  *pic_param_data = &(innards->pic);
  return 0;
}

/*
 * Retrieve the sequence parameter set data for the given id.
 *
 * - `seq_param_dict` is a parameter "dictionary" of the appropriate type.
 * - `seq_param_id` is the id to look up.
 * - `seq_param_data` is the data for that id. Do not free this, it refers
 *   into the "dictionary" datastructure.
 *
 * Note that altering the "dictionary" (with `remember_param_data()`) may
 * cause the underlying datastructures to be realloc'ed, which in turn means
 * that the address returned as `seq_param_data` may not be valid after such
 * an action.
 *
 * Returns 0 if it succeeds, 1 if the id is not recognised.
 */
extern int get_seq_param_data(param_dict_p seq_param_dict,
                              uint32_t     seq_param_id,
                              nal_seq_param_data_p *seq_param_data)
{
  nal_innards_p  innards;
  int absent = lookup_param_data(seq_param_dict,seq_param_id,&innards);
  if (absent)
  {
    fprint_err("### Unable to find sequence parameter set with id %u\n",
               seq_param_id);
    return 1;
  }
  *seq_param_data = &(innards->seq);
  return 0;
}

// ------------------------------------------------------------
// Lists of NAL units
//
// This duplicates the functionality provided by ES unit lists
// in es.c/h, but it works at the higher level of NAL units,
// which is useful if one wants to report on the content of the
// lists *as* NAL units.
// ------------------------------------------------------------

/*
 * Build a new list-of-nal-units datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_nal_unit_list(nal_unit_list_p  *list)
{
  nal_unit_list_p  new = malloc(SIZEOF_NAL_UNIT_LIST);
  if (new == NULL)
  {
    print_err("### Unable to allocate NAL unit list datastructure\n");
    return 1;
  }
  
  new->length = 0;
  new->size = NAL_UNIT_LIST_START_SIZE;
  new->array = malloc(sizeof(nal_unit_p)*NAL_UNIT_LIST_START_SIZE);
  if (new->array == NULL)
  {
    free(new);
    print_err("### Unable to allocate array in NAL unit list datastructure\n");
    return 1;
  }

  *list = new;
  return 0;
}

/*
 * Add a NAL unit to the end of the NAL unit list. Does not take a copy.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int append_to_nal_unit_list(nal_unit_list_p  list,
                                   nal_unit_p       nal)
{
  if (list->length == list->size)
  {
    int newsize = list->size + NAL_UNIT_LIST_INCREMENT;
    list->array = realloc(list->array,newsize*sizeof(nal_unit_p));
    if (list->array == NULL)
    {
      print_err("### Unable to extend NAL unit list array\n");
      return 1;
    }
    list->size = newsize;
  }
  list->array[list->length++] = nal;
  return 0;
}

/*
 * Tidy up a NAL unit list datastructure after we've finished with it.
 *
 * If `deep` is true, then any NAL units in the list will be freed
 * as well (this will be a Bad Thing if anywhere else is using them).
 */
static inline void clear_nal_unit_list(nal_unit_list_p  list,
                                int              deep)
{
  if (list->array != NULL)
  {
    int ii;
    for (ii=0; ii<list->length; ii++)
    {
      if (deep)
      {
        nal_unit_p nal = list->array[ii];
        if (nal != NULL)
        {
          clear_nal_unit(nal);
          free(nal);
        }
      }
      list->array[ii] = NULL;
    }
    free(list->array);
    list->array = NULL;
  }
  list->length = 0;
  list->size = 0;
}

/*
 * Reset (empty) a NAL unit list.
 *
 * If `deep` is true, then any NAL units in the list will be freed
 * as well (this will be a Bad Thing if anywhere else is using them).
 */
extern void reset_nal_unit_list(nal_unit_list_p  list,
                                int              deep)
{
  if (list->array != NULL)
  {
    int ii;
    for (ii=0; ii<list->length; ii++)
    {
      if (deep)
      {
        nal_unit_p nal = list->array[ii];
        if (nal != NULL)
        {
          clear_nal_unit(nal);
          free(nal);
        }
      }
      list->array[ii] = NULL;
    }
    // We *could* also shrink it - as it is, it will never get smaller
    // than its maximum size. Is that likely to be a problem?
  }
  list->length = 0;
}

/*
 * Tidy up and free a NAL unit list datastructure after we've finished with it.
 *
 * Clears the datastructure, frees it and returns `list` as NULL.
 *
 * If `deep` is true, then any NAL units in the list will be freed
 * as well (this will be a Bad Thing if anywhere else is using them).
 *
 * Does nothing if `list` is already NULL.
 */
extern void free_nal_unit_list(nal_unit_list_p  *list,
                               int               deep)
{
  if (*list == NULL)
    return;
  clear_nal_unit_list(*list,deep);
  free(*list);
  *list = NULL;
}

/*
 * Report on a NAL unit list's contents, to the given stream.
 */
extern void report_nal_unit_list(int    is_msg,
                                 char  *prefix,
                                 nal_unit_list_p  list)
{
  if (prefix == NULL)
    prefix = "";
  if (list->array == NULL)
    fprint_msg_or_err(is_msg,"%s<empty>\n",prefix);
  else
  {
    int ii;
    for (ii=0; ii<list->length; ii++)
    {
      fprint_msg_or_err(is_msg,"%s",prefix);
      report_nal(is_msg,list->array[ii]);
    }
  }
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

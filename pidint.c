/*
 * Support for lists (actually arrays) of PID versus integer
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

#include "compat.h"
#include "pidint_fns.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "ts_fns.h"
#include "h222_defns.h"

// ============================================================================
// PIDINT LIST maintenance
// ============================================================================
/*
 * Initialise a new pid/int list datastructure.
 */
extern int init_pidint_list(pidint_list_p  list)
{
  list->length = 0;
  list->size = PIDINT_LIST_START_SIZE;
  list->number = malloc(sizeof(int)*PIDINT_LIST_START_SIZE);
  if (list->number == NULL)
  {
    print_err("### Unable to allocate array in program list datastructure\n");
    return 1;
  }
  list->pid = malloc(sizeof(uint32_t)*PIDINT_LIST_START_SIZE);
  if (list->pid == NULL)
  {
    free(list->number);
    print_err("### Unable to allocate array in program list datastructure\n");
    return 1;
  }
  return 0;

}

/*
 * Build a new pid/int list datastructure.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int build_pidint_list(pidint_list_p  *list)
{
  pidint_list_p  new = malloc(SIZEOF_PIDINT_LIST);
  if (new == NULL)
  {
    print_err("### Unable to allocate pid/int list datastructure\n");
    return 1;
  }

  if (init_pidint_list(new))
    return 1;

  *list = new;
  return 0;
}

/*
 * Add a pid/integer pair to the end of the list
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int append_to_pidint_list(pidint_list_p  list,
                                 uint32_t       pid,
                                 int            program)
{
  if (list == NULL)
  {
    print_err("### Unable to append to NULL pid/int list\n");
    return 1;
  }

  if (list->length == list->size)
  {
    int newsize = list->size + PIDINT_LIST_INCREMENT;
    list->number = realloc(list->number,newsize*sizeof(int));
    if (list->number == NULL)
    {
      print_err("### Unable to extend pid/int list array\n");
      return 1;
    }
    list->pid = realloc(list->pid,newsize*sizeof(uint32_t));
    if (list->pid == NULL)
    {
      print_err("### Unable to extend pid/int list array\n");
      return 1;
    }
    list->size = newsize;
  }
  list->number[list->length] = program;
  list->pid[list->length] = pid;
  list->length++;
  return 0;
}

/*
 * Remove a pid/integer pair from the list
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int remove_from_pidint_list(pidint_list_p  list,
                                   uint32_t       pid)
{
  int  index;
  int  ii;
  if (list == NULL)
  {
    print_err("### Unable to remove entry from NULL pid/int list\n");
    return 1;
  }

  index = pid_index_in_pidint_list(list,pid);
  if (index == -1)
  {
    fprint_err("### Cannot remove PID %04x from pid/int list"
               " - it is not there\n",pid);
    return 1;
  }

  for (ii = index; ii < (list->length - 1); ii++)
  {
    list->pid[ii] = list->pid[ii+1];
    list->number[ii] = list->number[ii+1];
  }
  (list->length) --;
  return 0;
}

/*
 * Tidy up and free a pid/int list datastructure after we've finished with it
 *
 * Clears the datastructure, frees it and returns `list` as NULL.
 *
 * Does nothing if `list` is already NULL.
 */
extern void free_pidint_list(pidint_list_p  *list)
{
  if (*list == NULL)
    return;
  if ((*list)->number != NULL)
  {
    free((*list)->number);
    (*list)->number = NULL;
  }
  if ((*list)->pid != NULL)
  {
    free((*list)->pid);
    (*list)->pid = NULL;
  }
  (*list)->length = 0;
  (*list)->size = 0;
  free(*list);
  *list = NULL;
}

/*
 * Report on a pid/int list's contents
 */
extern void report_pidint_list(pidint_list_p  list,
                               char          *list_name,
                               char          *int_name,
                               int            pid_first)
{
  if (list == NULL)
    fprint_msg("%s is NULL\n",list_name);
  else if (list->length == 0)
    fprint_msg("%s is empty\n",list_name);
  else
  {
    int ii;
    fprint_msg("%s:\n",list_name);
    for (ii=0; ii<list->length; ii++)
    {
      if (pid_first)
        fprint_msg("    PID %04x (%d) -> %s %d\n",
                   list->pid[ii],list->pid[ii],int_name,list->number[ii]);
      else
        fprint_msg("    %s %d -> PID %04x (%d)\n",
                   int_name,list->number[ii],list->pid[ii],list->pid[ii]);
    }
  }
}

/*
 * Lookup a PID to find its index in a pid/int list.
 *
 * Note that if `list` is NULL, then -1 will be returned - this is to
 * allow the caller to make a query before they have read a list from the
 * bitstream.
 *
 * Returns its index (0 or more) if the PID is in the list, -1 if it is not.
 */
extern int pid_index_in_pidint_list(pidint_list_p  list,
                                    uint32_t        pid)
{
  int ii;
  if (list == NULL)
    return -1;
  for (ii = 0; ii < list->length; ii++)
  {
    if (list->pid[ii] == pid)
      return ii;
  }
  return -1;
}

/*
 * Lookup a PID to find the corresponding integer value in a pid/int list.
 *
 * Returns 0 if the PID is in the list, -1 if it is not.
 */
extern int pid_int_in_pidint_list(pidint_list_p  list,
                                  uint32_t        pid,
                                  int            *number)
{
  int ii;
  if (list == NULL)
    return -1;
  for (ii = 0; ii < list->length; ii++)
  {
    if (list->pid[ii] == pid)
    {
      *number = list->number[ii];
      return 0;
    }
  }
  return -1;
}

/*
 * Lookup a PID to see if it is in a pid/int list.
 *
 * Note that if `list` is NULL, then FALSE will be returned - this is to
 * allow the caller to make a query before they have read a list from the
 * bitstream.
 *
 * Returns TRUE if the PID is in the list, FALSE if it is not.
 */
extern int pid_in_pidint_list(pidint_list_p  list,
                              uint32_t        pid)
{
  return pid_index_in_pidint_list(list,pid) != -1;
}

/*
 * Check if two pid/int lists have the same content.
 *
 * Note that:
 *
 *  - a list always compares as the same as itself
 *  - two NULL lists compare as the same
 *  - the *order* of PID/int pairs in the lists does not matter
 *
 * Returns TRUE if the two have the same content, FALSE otherwise.
 */
extern int same_pidint_list(pidint_list_p  list1,
                            pidint_list_p  list2)
{
  int ii;
  if (list1 == list2)
    return TRUE;
  else if (list1 == NULL || list2 == NULL)
    return FALSE;
  else if (list1->length != list2->length)
    return FALSE;
  for (ii = 0; ii < list1->length; ii++)
  {
    uint32_t pid = list1->pid[ii];
    int      idx = pid_index_in_pidint_list(list2,pid);
    if (idx == -1)
      return FALSE;
    else if (list1->number[ii] != list2->number[idx])
      return FALSE;
  }
  return TRUE;
}

/*
 * Report on a program stream list (a specialisation of report_pidint_list).
 *
 * - `list` is the stream list to report on
 * - `prefix` is NULL or a string to put before each line printed
 */
extern void report_stream_list(pidint_list_p  list,
                               char           *prefix)
{
  if (prefix!=NULL) print_msg(prefix);
  if (list == NULL)
    print_msg("Program stream list is NULL\n");
  else if (list->length == 0)
    print_msg("Program stream list is empty\n");
  else
  {
    int ii;
    print_msg("Program streams:\n");
    for (ii=0; ii<list->length; ii++)
    {
      if (prefix!=NULL) print_msg(prefix);
      fprint_msg("    PID %04x (%d) -> Stream type %3d (%s)\n",
                 list->pid[ii],list->pid[ii],list->number[ii],
                 h222_stream_type_str(list->number[ii]));
    }
  }
}

// ============================================================================
// PMT data maintenance
// ============================================================================
/*
 * Initialise a PMT datastructure's stream lists
 */
static int init_pmt_streams(pmt_p  pmt)
{
  pmt->num_streams = 0;
  pmt->streams_size = PMT_STREAMS_START_SIZE;
  pmt->streams = malloc(SIZEOF_PMT_STREAM*PMT_STREAMS_START_SIZE);
  if (pmt->streams == NULL)
  {
    print_err("### Unable to allocate streams in PMT datastructure\n");
    return 1;
  }
  return 0;

}

/*
 * Build a new PMT datastructure.
 *
 * `version_number` should be in the range 0-31, and will be treated as a
 * number modulo 32 if it is not.
 *
 * `PCR_pid` should be a legitimate PCR PID - i.e., in the range 0x0010 to
 * 0x1FFE, or 0x1FFF to indicate "unset". However, for convenience, the
 * value 0 will also be accepted, and converted to 0x1FFF.
 *
 * Returns (a pointer to) the new PMT datastructure, or NULL if some error
 * occurs.
 */
extern pmt_p build_pmt(uint16_t program_number, byte version_number,
                       uint32_t PCR_pid)
{
  pmt_p  new;

  if (version_number > 31)
    version_number = version_number % 32;

  if (PCR_pid == 0)
    PCR_pid = 0x1FFF;   // unset

  if (PCR_pid != 0x1FFF && (PCR_pid < 0x0010 || PCR_pid > 0x1ffe))
  {
    fprint_err("### Error building PMT datastructure\n"
               "    PCR PID %04x is outside legal program stream range\n",
               PCR_pid);
    return NULL;
  }

  new = malloc(SIZEOF_PMT);
  if (new == NULL)
  {
    print_err("### Unable to allocate PMT datastructure\n");
    return NULL;
  }

  new->program_number = program_number;
  new->version_number = version_number;
  new->PCR_pid = PCR_pid;
  new->program_info_length = 0;
  new->program_info = NULL;

  if (init_pmt_streams(new))
  {
    free(new);
    return NULL;
  }

  return new;
}

/*
 * Set the descriptor data on a PMT. Specifically, 'program info',
 * the descriptor data in the PMT "as a whole".
 *
 * Any previous program information for this PMT is lost.
 *
 * A copy of the program information bytes is taken.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int set_pmt_program_info(pmt_p    pmt,
                                uint16_t program_info_length,
                                byte    *program_info)
{
  if (program_info_length > PMT_MAX_INFO_LENGTH)
  {
    fprint_err("### Program info length %d is more than %d\n",
               program_info_length,PMT_MAX_INFO_LENGTH);
    return 1;
  }
  if (pmt->program_info == NULL)
  {
    pmt->program_info = malloc(program_info_length);
    if (pmt->program_info == NULL)
    {
      print_err("### Unable to allocate program info in PMT datastructure\n");
      return 1;
    }
  }
  else if (program_info_length != pmt->program_info_length)
  {
    // well, we might be shrinking it rather than growing it, but still
    pmt->program_info = realloc(pmt->program_info,program_info_length);
    if (pmt->program_info == NULL)
    {
      print_err("### Unable to extend program info in PMT datastructure\n");
      return 1;
    }
  }
  memcpy(pmt->program_info,program_info,program_info_length);
  pmt->program_info_length = program_info_length;
  return 0;
}

/*
 * Add a program stream to a PMT datastructure
 *
 * If `ES_info_length` is greater than 0, then `ES_info` is copied.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int add_stream_to_pmt(pmt_p      pmt,
                             uint32_t   elementary_PID,
                             byte       stream_type,
                             uint16_t   ES_info_length,
                             byte      *ES_info)
{
  if (pmt == NULL)
  {
    print_err("### Unable to append to NULL PMT datastructure\n");
    return 1;
  }

  if (elementary_PID < 0x0010 || elementary_PID > 0x1ffe)
  {
    fprint_err("### Error adding stream to PMT\n"
               "    Elementary PID %04x is outside legal program stream range\n",
               elementary_PID);
    return 1;
  }

  if (ES_info_length > PMT_MAX_INFO_LENGTH)
  {
    fprint_err("### ES info length %d is more than %d\n",
               ES_info_length,PMT_MAX_INFO_LENGTH);
    return 1;
  }

  if (pmt->num_streams == pmt->streams_size)
  {
    int newsize = pmt->streams_size + PMT_STREAMS_INCREMENT;
    pmt->streams = realloc(pmt->streams,newsize*SIZEOF_PMT_STREAM);
    if (pmt->streams == NULL)
    {
      print_err("### Unable to extend PMT streams array\n");
      return 1;
    }
    pmt->streams_size = newsize;
  }
  pmt->streams[pmt->num_streams].stream_type = stream_type;
  pmt->streams[pmt->num_streams].elementary_PID = elementary_PID;
  pmt->streams[pmt->num_streams].ES_info_length = ES_info_length;
  if (ES_info_length > 0)
  {
    pmt->streams[pmt->num_streams].ES_info = malloc(ES_info_length);
    if (pmt->streams[pmt->num_streams].ES_info == NULL)
    {
      print_err("### Unable to allocate PMT stream ES info\n");
      return 1;
    }
    memcpy(pmt->streams[pmt->num_streams].ES_info,ES_info,ES_info_length);
  }
  else
    pmt->streams[pmt->num_streams].ES_info = NULL;
  pmt->num_streams++;
  return 0;
}

/*
 * Free a PMT stream datastructure
 */
static void free_pmt_stream(pmt_stream_p  stream)
{
  if (stream == NULL) return;
  if (stream->ES_info != NULL)
  {
    free(stream->ES_info);
    stream->ES_info = NULL;
  }
}

/*
 * Remove a program stream from a PMT.
 *
 * Returns 0 if it succeeds, 1 if some error occurs.
 */
extern int remove_stream_from_pmt(pmt_p         pmt,
                                  uint32_t      pid)
{
  int  index;
  int  ii;
  if (pmt == NULL)
  {
    print_err("### Unable to remove entry from NULL PMT datastructure\n");
    return 1;
  }

  index = pid_index_in_pmt(pmt,pid);
  if (index == -1)
  {
    fprint_err("### Cannot remove PID %04x from PMT datastructure"
               " - it is not there\n",pid);
    return 1;
  }

  free_pmt_stream(&pmt->streams[index]);

  for (ii = index; ii < (pmt->num_streams - 1); ii++)
    pmt->streams[ii] = pmt->streams[ii+1];
  (pmt->num_streams) --;
  return 0;
}

/*
 * Tidy up and free a PMT datastructure after we've finished with it
 *
 * Clears the datastructure, frees it and returns `pmt` as NULL.
 *
 * Does nothing if `pmt` is already NULL.
 */
extern void free_pmt(pmt_p  *pmt)
{
  if (*pmt == NULL)
    return;
  if ((*pmt)->num_streams > 0)
  {
    int ii;
    for (ii = 0; ii < (*pmt)->num_streams; ii++)
      free_pmt_stream(&(*pmt)->streams[ii]);
    (*pmt)->num_streams = 0;
  }
  if ((*pmt)->program_info != NULL)
  {
    free((*pmt)->program_info);
    (*pmt)->program_info = NULL;
  }
  free((*pmt)->streams);
  (*pmt)->program_info_length = 0;
  free(*pmt);
  *pmt = NULL;
}

/*
 * Lookup a PID to find its index in a PMT datastructure.
 *
 * Note that if `pmt` is NULL, then -1 will be returned.
 *
 * Returns its index (0 or more) if the PID is in the list, -1 if it is not.
 */
extern int pid_index_in_pmt(pmt_p     pmt,
                            uint32_t  pid)
{
  int ii;
  if (pmt == NULL)
    return -1;
  for (ii = 0; ii < pmt->num_streams; ii++)
  {
    if (pmt->streams[ii].elementary_PID == pid)
      return ii;
  }
  return -1;
}

/*
 * Lookup a PID to find the corresponding program stream information.
 *
 * Returns a pointer to the stream information if the PID is in the list,
 * NULL if it is not.
 */
extern pmt_stream_p pid_stream_in_pmt(pmt_p          pmt,
                                      uint32_t       pid)
{
  int ii;
  if (pmt == NULL)
    return NULL;
  for (ii = 0; ii < pmt->num_streams; ii++)
  {
    if (pmt->streams[ii].elementary_PID == pid)
      return &pmt->streams[ii];
  }
  return NULL;
}

/*
 * Lookup a PID to see if it is in a PMT datastructure.
 *
 * Note that if `pmt` is NULL, then FALSE will be returned.
 *
 * Returns TRUE if the PID is in the PMT's stream list, FALSE if it is not.
 */
extern int pid_in_pmt(pmt_p     pmt,
                      uint32_t  pid)
{
  return pid_index_in_pmt(pmt,pid) != -1;
}

/*
 * Check if two PMT streams have the same content.
 *
 * Returns TRUE if the two have the same content, FALSE otherwise.
 */
static int same_pmt_stream(pmt_stream_p  str1,
                           pmt_stream_p  str2)
{
  if (str1 == str2)                                     // !!!
    return TRUE;
  else if (str1 == NULL || str2 == NULL)                // !!!
    return FALSE;
  else if (str1->elementary_PID != str2->elementary_PID)
    return FALSE;
  else if (str1->ES_info_length != str2->ES_info_length)
    return FALSE;
  else if (memcmp(str1->ES_info,str2->ES_info,str1->ES_info_length))
    return FALSE;
  else
    return TRUE;
}

/*
 * Check if two PMT datastructures have the same content.
 *
 * Note that:
 *
 *  - a PMT datastructure always compares as the same as itself
 *  - two NULL datastructures compare as the same
 *  - a different version number means a different PMT
 *  - the *order* of program streams in the PMTs does not matter
 *  - descriptors must be identical as well, and byte order therein
 *    does matter (this may need changing later on)
 *
 * Returns TRUE if the two have the same content, FALSE otherwise.
 */
extern int same_pmt(pmt_p  pmt1,
                    pmt_p  pmt2)
{
  int ii;
  if (pmt1 == pmt2)
    return TRUE;
  else if (pmt1 == NULL || pmt2 == NULL)
    return FALSE;
  else if (pmt1->PCR_pid != pmt2->PCR_pid)
    return FALSE;
  else if (pmt1->version_number != pmt2->version_number)
    return FALSE;
  else if (pmt1->program_info_length != pmt2->program_info_length)
    return FALSE;
  else if (pmt1->num_streams != pmt2->num_streams)
    return FALSE;
  else if (memcmp(pmt1->program_info,pmt2->program_info,
                  pmt1->program_info_length))
    return FALSE;

  for (ii = 0; ii < pmt1->num_streams; ii++)
  {
    uint32_t pid = pmt1->streams[ii].elementary_PID;
    int      idx = pid_index_in_pmt(pmt2,pid);
    if (idx == -1)
      return FALSE;
    else if (!same_pmt_stream(&pmt1->streams[ii],&pmt2->streams[idx]))
      return FALSE;
  }
  return TRUE;
}

/*
 * Report on a PMT datastructure.
 *
 * - if `is_msg`, report as a message, otherwise as an error
 * - `prefix` is NULL or a string to put before each line printed
 * - `pmt` is the PMT to report on
 */
extern void report_pmt(int     is_msg,
                       char   *prefix,
                       pmt_p   pmt)
{
  if (prefix!=NULL) fprint_msg_or_err(is_msg,prefix);
  if (pmt == NULL)
  {
    fprint_msg_or_err(is_msg,"PMT is NULL\n");
    return;
  }
  else
    fprint_msg_or_err(is_msg,"Program %d, version %d, PCR PID %04x (%d)\n",
                      pmt->program_number,pmt->version_number,pmt->PCR_pid,pmt->PCR_pid);

  if (pmt->program_info_length > 0)
  {
    if (prefix!=NULL) fprint_msg_or_err(is_msg,prefix);
    print_data(is_msg,"   Program info",pmt->program_info,
               pmt->program_info_length,pmt->program_info_length);
    print_descriptors(is_msg,prefix,"   ",pmt->program_info,
                      pmt->program_info_length);
  }
  if (pmt->num_streams > 0)
  {
    int ii;
    if (prefix!=NULL) fprint_msg_or_err(is_msg,prefix);
    fprint_msg_or_err(is_msg,"Program streams:\n");
    for (ii=0; ii<pmt->num_streams; ii++)
    {
      if (prefix!=NULL) fprint_msg_or_err(is_msg,prefix);
      fprint_msg_or_err(is_msg,"  PID %04x (%4d) -> Stream type %02x (%3d) %s\n",
                        pmt->streams[ii].elementary_PID,
                        pmt->streams[ii].elementary_PID,
                        pmt->streams[ii].stream_type,
                        pmt->streams[ii].stream_type,
                        h222_stream_type_str(pmt->streams[ii].stream_type));
      if (pmt->streams[ii].ES_info_length > 0)
      {
        if (prefix!=NULL) fprint_msg_or_err(is_msg,prefix);
        print_data(is_msg,"      ES info",
                   pmt->streams[ii].ES_info,
                   pmt->streams[ii].ES_info_length,
                   pmt->streams[ii].ES_info_length);
        print_descriptors(is_msg,prefix,"      ",
                          pmt->streams[ii].ES_info,
                          pmt->streams[ii].ES_info_length);
      }
    }
  }
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

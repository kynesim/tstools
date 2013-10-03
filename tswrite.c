/*
 * Support for writing out TS packets, to file, or over TCP/IP or UDP
 *
 * When writing asynchronously, provides automated producer/consumer
 * behaviour via a circular buffer, optionally taking timing from the
 * TS PCR entries.
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
#include <ctype.h>  // for isprint

#include <sys/types.h>
#include <time.h>        // Sleeping and timing

#ifdef _WIN32
#include <sys/timeb.h>
#include <winsock2.h>
#include <process.h>
#else  // _WIN32
#include <unistd.h>
#include <sys/time.h>    // gettimeofday
#include <sys/mman.h>    // memory mapping
#include <sys/wait.h>
#include <sys/socket.h>  // send
#endif // _WIN32

#include "compat.h"
#include "misc_fns.h"
#include "printing_fns.h"
#include "tswrite_fns.h"
#include "ts_fns.h"

// ------------------------------------------------------------
// Global flags affecting debugging

#define DEBUG_DATA_WAIT 0

// If the user is giving commands to tell the process what to do
// (as read in read_comand), do we want to report on each command
// character as it is read?
#define DEBUG_COMMANDS 0

// Do we want to be able to display the circular buffer contents?
#define DISPLAY_BUFFER 1
#if DISPLAY_BUFFER
static int global_show_circular = FALSE;
#endif

static int global_parent_debug = FALSE;
static int global_show_all_times = FALSE; // extra info for the above
static int global_child_debug = FALSE;

// Should we try to simulate network choppiness by randomly perturbing
// the child process's idea of time? If `global_perturb_range` is non-zero,
// then yes, we should (admittedly with rather a blunt hammer).
// In which case, we then specify a seed to use for our random perturbations,
// and a range for the time in milliseconds that we should use as a range
// (we'll generate values for that range on either side of zero).
static unsigned global_perturb_seed;
static unsigned global_perturb_range = 0;
static int      global_perturb_verbose = FALSE;
// ------------------------------------------------------------

// The default number of set-of-N-packets to allow for in priming the
// output buffers
#define DEFAULT_PRIME_SIZE 10

// A millisecond is a useful unit for waiting, but nanosleep works
// in nanoseconds, so let's define one in terms of the other
// (nanosecond == 10e-9s, microsecond = 10e-6s, millisecond = 10e-3s)
#define ONE_MS_AS_NANOSECONDS   1000000

// Default waits
// The parent can afford to wait longer than the child
// 10ms seems reasonable as a default for the child
#define DEFAULT_PARENT_WAIT  50
#define DEFAULT_CHILD_WAIT   10

// We need some guess at an initial data rate, if the user does not give us one
// (note that this is in bytes per second)
#define DEFAULT_BYTE_RATE 250000

// I'm happy to have these affect everyone using this "module",
// at least for the moment - changing the values is likely to be only
// for experimentation, and then the default values will be settled
// appropriately
//
// Wait times for parent and child, in milliseconds
static int global_parent_wait = DEFAULT_PARENT_WAIT;
static int global_child_wait = DEFAULT_CHILD_WAIT;

// If the child waits for a very long time, it may (is allowed to) assume that
// the parent has stopped feeding it. We need a number of times it should try
// waiting its global_child_wait before it decides to give up (so we may
// assume it has waited, in total, at least this number times the length of
// time it waits each time).
#define CHILD_GIVE_UP_AFTER 1000

// And a similar quantity for the parent, in case the child dies
#define PARENT_GIVE_UP_AFTER 1000

// If not being quiet, report progress every REPORT_EVERY packets read
#define REPORT_EVERY 10000




// ============================================================
// CIRCULAR BUFFER
// ============================================================

// We default to using a "packet" of 7 transport stream packets because 7*188 =
// 1316, but 8*188 = 1504, and we would like to output as much data as we can
// that is guaranteed to fit into a single ethernet packet, size 1500.
#define DEFAULT_TS_PACKETS_IN_ITEM      7

// For simplicity, we'll have a maximum on that (it allows us to have static
// array sizes in some places). This should be a big enough size to more than
// fill a jumbo packet on a gigabit network.
#define MAX_TS_PACKETS_IN_ITEM          100

// ------------------------------------------------------------
// A circular buffer, usable as a queue
//
// We "waste" one buffer item so that we don't have to maintain a count
// of items in the buffer
//
// To get an understanding of how it works, choose a small BUFFER_SIZE
// (e.g., 11), enable DISPLAY_BUFFER, and select --visual - this will show the
// reading/writing of the circular buffer in action, including the
// "unused item".
//
// The data for the circular buffer
// Each circular buffer item "contains" (up to) N TS packets (where N defaults
// to 7, and is specified as `item_size` in the circular buffer header), and a
// time (in microseconds) when we would like it to be output (relative to the
// time for the first packet "sent").
//
// Said data is stored at the address indicated by the circular buffer
// "header", as `item_data`.
//
struct circular_buffer_item
{
  uint32_t time;              // when we would like this data output
  int      discontinuity;     // TRUE if our timeline has "broken"
  int      length;            // number of bytes of data in the array
};
typedef struct circular_buffer_item *circular_buffer_item_p;

#define SIZEOF_CIRCULAR_BUFFER_ITEM sizeof(struct circular_buffer_item)



typedef struct rtp_hdr_info_s
{
  uint16_t seq;
  uint32_t ssrc;
} rtp_hdr_info_t;


// ------------------------------------------------------------
// The header for the circular buffer
//
// Note that `start` is only ever written to by the child process, and this is
// the only thing that the child process ever changes in the circular buffer.
//
// `maxnowait` is the maximum number of packets to send to the target host
// without forcing an intermediate wait - required to stop us "swamping" the
// target with too much data, and overrunning its buffers.
struct circular_buffer
{
  volatile int      start;      // start of data "pointer"
  volatile int      end;        // end of completed data "pointer" (you guessed)
  volatile int      pending;    // end of buffered but not ready for xmit
  int      size;       // the actual length of the `item` array

  volatile int eos;    // end of stream

  int      TS_in_item; // max number of TS packets in a circular buffer item
  int      item_size;  // and thus the size of said item's data array
  int      hdr_size;
  tswrite_pkt_hdr_type_t hdr_type;
  union {
    rtp_hdr_info_t rtp;
  } hdr;

  int      maxnowait;  // max number consecutive packets to send with no wait
  int      waitfor;    // the number of microseconds to wait thereafter

  // The location of the packet data for the circular buffer items
  byte     *item_data;

  // The "header" data for each circular buffer item
  struct circular_buffer_item item[];
};
typedef struct circular_buffer *circular_buffer_p;

// Note that size doesn't include the final `item`
#define SIZEOF_CIRCULAR_BUFFER sizeof(struct circular_buffer)

// For PCR2 pacing we accumulate the initial packets here so it must be big
// enough to cope with a max bitrate stream
// Say 100Mbits is the fastest we are going to care about
// So we need to buffer 2 PCRs which have a max spacing of .1s (by the std)
// and another .1s before that for having just missed a PCR = .2s = 20Mbits
// = 2.5Mbytes. 2048 * 188 * 7 = 2.7M
#define DEFAULT_CIRCULAR_BUFFER_SIZE  2048              // used to be 100

#define PRIME_SPEED_NORMAL 100

// ============================================================
// BUFFERED OUTPUT
// ============================================================

// Information about each TS packet in our circular buffer item
struct TS_packet_info
{
  int                index;
  uint32_t           pid;       // do we need the PIDs?
  int                got_pcr;
  uint64_t           pcr;
};
typedef struct TS_packet_info *TS_packet_info_p;
#define SIZEOF_TS_PACKET_INFO sizeof(struct TS_packet_info);

// PCR interpolation structure
typedef struct pcr_pace_env_s
{
  uint32_t gap_bytes;
  uint32_t next_bytes;
  int32_t next_offset;
  int next_index;
  int pcr1_set;  // Seen 1st PCR
  int gap_set;  // Seen 2nd PCR
  int pkt1;  // 1st pkt dealt with? (!discontinuity)
  uint64_t pcr1;
  uint64_t pcr_base;

  int prime_req;
  int prime_speed;
  uint64_t prime_last_pcr;

  uint32_t prev_gap_bytes;
  uint64_t prev_pcr_gap;
  uint64_t next_pcr_base;
} pcr_pace_env;


// If we're going to support output via our circular buffer in a manner
// similar to that for output to a file or socket, then we need a structure
// to maintain the relevant information. It seems a bit wasteful to burden
// the circular buffer itself with this, particularly as only the writer
// cares about this data, so it needn't be shared.
struct buffered_TS_output
{
  circular_buffer_p  buffer;
  int                which;   // Which buffer index we're writing to
  int                started; // TRUE if we've started writing therein

  // For each TS packet in the circular buffer, remember its `count`
  // within the input stream, whether it had a PCR, and if so what that
  // PCR was. To make it simpler to access these arrays, also keep a fill
  // index into them (the alternative would be to always re-zero the
  // `got_pcr` values whenever we start a new circular buffer entry,
  // which would be a pain...)
  int                    num_packets;  // how many TS packets we've got
  struct TS_packet_info  packet[MAX_TS_PACKETS_IN_ITEM];

  // `rate` is the rate (in bytes per second) we would like to output data at
  uint32_t           rate;

  // `pcr_scale` is a multiplier for PCRs - each PCR found gets its value
  // multiplied by this
  double             pcr_scale;

  // `use_pcrs` indicates if we should use PCRs in the data to drive our
  // timing, rather than use the specified byte rate directly. The `priming`
  // values are only relevant if `use_pcrs` is true.
  tswrite_pcr_mode   pcr_mode;

  // 'prime_size' is the amount of space/time to 'prime' the circular buffer
  // output timing mechanism with. This is effectively multiples of the
  // size of a circular buffer item.
  int                prime_size;

  // Percentage "too fast" speedup for our priming rate
  int                prime_speedup;

  pcr_pace_env       pcr_pace;
};

#ifdef _WIN32
// ============================================================
// Windows specific - gettimeofday replacement
// ============================================================
/*
 * Windows does not provide gettimeofday, but it has equivalent functionality,
 * and does provide timeval, so wae can pretend...
 */
static inline void gettimeofday(struct timeval *tv,
                                void           *timezone)
{
  struct _timeb timebuffer;
  _ftime(&timebuffer);
  tv->tv_sec  = (long)timebuffer.time;
  tv->tv_usec = timebuffer.millitm * 1000;
}
#endif

// ============================================================
// Low level circular buffer support
// ============================================================
/*
 * Set up our circular buffer in shared memory
 *
 * - `buf` is a pointer to the new shared memory
 * - `circ_buf_size` is the number of buffer entries (plus one) we would
 *   like.
 * - `TS_in_packet` is the number of TS packets to allow in each network
 *   packet/circular buffer item.
 * - `maxnowait` is the maximum number of packets to send to the target
 *   host with no wait between packets
 * - `waitfor` is the number of microseconds to wait for thereafter
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int map_circular_buffer(circular_buffer_p  *circular,
                               int                 circ_buf_size,
                               int                 TS_in_packet,
                               int                 maxnowait,
                               int                 waitfor,
                               const tswrite_pkt_hdr_type_t hdr_type)
{
  // Rather than map a file, we'll map anonymous memory
  // BSD supports the MAP_ANON flag as is,
  // Linux (bless it) deprecates MAP_ANON and would prefer us to use
  // the more verbose MAP_ANONYMOUS (but MAP_ANON is still around, so
  // we'll stick with that while we can)

  // The shared memory starts with the circular buffer "header". This ends with
  // an array of `circular_buffer_item` structures, of length `circ_buf_size`.
  //
  // Each circular buffer item needs enough space to store (up to)
  // `TS_in_packet` TS entries (so, `TS_in_packet`*188 bytes). Since that size
  // is not fixed, we can't just allocate it "inside" the buffer items (it
  // wouldn't be nice to allocate the *maximum* possible space we might want!).
  // Instead, we'll put it as a byte array after the rest of our data.
  //
  // Space may be left to add an RTP header before each items data
  //
  // So:
  const int hdr_size = (hdr_type == PKT_HDR_TYPE_RTP) ? 12 : 0;
  int base_size = SIZEOF_CIRCULAR_BUFFER +
                  (circ_buf_size * SIZEOF_CIRCULAR_BUFFER_ITEM);
  int data_size = circ_buf_size * (TS_in_packet * TS_PACKET_SIZE + hdr_size);
  int total_size = base_size + data_size;
  circular_buffer_p cb;

  *circular = NULL;

#ifdef _WIN32
  // Under Windows, we're using threading to manage our parent/child
  // processes, so we can just use malloc here
  cb = malloc(total_size);
  if (cb == NULL)
  {
    fprint_err("### Error mapping circular buffer as shared memory: %s\n",
               strerror(errno));
    return 1;
  }
#else // _WIN32
  cb = mmap(NULL,total_size,
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

  if (cb == MAP_FAILED)
  {
    fprint_err("### Error mapping circular buffer as shared memory: %s\n",
               strerror(errno));
    return 1;
  }
#endif // _WIN32

  cb->start = 1;
  cb->end = 0;
  cb->pending = 0;
  cb->eos = FALSE;
  cb->size = circ_buf_size;
  cb->TS_in_item = TS_in_packet;
  cb->item_size = TS_in_packet * TS_PACKET_SIZE + hdr_size;
  cb->hdr_size = hdr_size;
  cb->hdr_type = hdr_type;
  if (hdr_type == PKT_HDR_TYPE_RTP)
  {
    struct timeval now;
    gettimeofday(&now, NULL);

    cb->hdr.rtp.seq = 0;
    cb->hdr.rtp.ssrc = (uint32_t)(now.tv_sec ^ now.tv_usec << 12);  // A somewhat random number
  }
  cb->maxnowait = maxnowait;
  cb->waitfor = waitfor;
  cb->item_data = (byte *) cb + base_size + hdr_size;
  *circular = cb;
  return 0;
}

/*
 * Release the shared memory containing our circular buffer
 *
 * - `buf` is a pointer to the shared memory
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int unmap_circular_buffer(circular_buffer_p  circular)
{
  int base_size = SIZEOF_CIRCULAR_BUFFER +
                  (circular->size * SIZEOF_CIRCULAR_BUFFER_ITEM);
  int data_size = circular->size * circular->item_size;
  int total_size = base_size + data_size;
#ifdef _WIN32
  // Under Windows, we're using threading to manage our parent/child
  // processes, so we malloced our circular buffer
  free(circular);
#else // _WIN32
  int err = munmap(circular,total_size);
  if (err)
  {
    fprint_err("### Error unmapping circular buffer from shared memory: %s\n",
               strerror(errno));
    return 1;
  }
#endif // _WIN32
  return 0;
}

/*
 * Is the buffer empty?
 */
static inline int circular_buffer_empty(circular_buffer_p  circular)
{
  return (circular->start == (circular->end + 1) % circular->size);
}

/*
 * Is the buffer full?
 */
static inline int circular_buffer_full(circular_buffer_p  circular)
{
  return ((circular->pending + 2) % circular->size == circular->start);
}

// Is the buffer full and never going to empty?
static inline int circular_buffer_jammed(circular_buffer_p  circular)
{
  return ((circular->pending + 1) % circular->size == circular->end);
}


/*
 * If the circular buffer is empty, wait until it gains some data.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static inline int wait_if_buffer_empty(circular_buffer_p  circular)
{
  static int count = 0;
#ifndef _WIN32
  struct timespec   time = {0,global_child_wait*ONE_MS_AS_NANOSECONDS};
  int    err;
#endif // _WIN32

  while (circular_buffer_empty(circular) && !circular->eos)
  {
#if DISPLAY_BUFFER
    if (global_show_circular && !global_parent_debug) print_msg("<-- wait\n");
#endif
    if (global_parent_debug) print_msg("<-- wait\n");
    count ++;

#ifdef _WIN32
    Sleep(global_child_wait);
#else // _WIN32
    err = nanosleep(&time,NULL);
    if (err == -1 && errno == EINVAL)
    {
      fprint_err("### Child: bad value (%ld) for wait time\n",time.tv_nsec);
      return 1;
    }
#endif // _WIN32

    // If we wait for a *very* long time, maybe our parent has crashed
    if (count > CHILD_GIVE_UP_AFTER)
    {
      print_err("### Child: giving up (parent not responding)\n");
      return 1;
    }
  }
  count = 0;
  return circular_buffer_empty(circular);  // If empty then EOS so return 1
}

/*
 * Wait for the circular buffer to fill up
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static inline int wait_for_buffer_to_fill(circular_buffer_p  circular)
{
  static int count = 0;
#ifndef _WIN32
  struct timespec   time = {0,global_child_wait*ONE_MS_AS_NANOSECONDS};
  int    err;
#endif // _WIN32

  while (!circular_buffer_full(circular) && !circular->eos)
  {
#if DISPLAY_BUFFER
    if (global_show_circular && !global_child_debug)
      print_msg("<-- wait for buffer to fill\n");
#endif
    if (global_child_debug) print_msg("<-- wait for buffer to fill\n");
    count ++;

#ifdef _WIN32
    Sleep(global_child_wait);
#else // _WIN32
    err = nanosleep(&time,NULL);
    if (err == -1 && errno == EINVAL)
    {
      fprint_err("### Child: bad value (%ld) for wait time\n",time.tv_nsec);
      return 1;
    }
#endif // _WIN32

    // If we wait for a *very* long time, maybe our parent has crashed
    if (count > CHILD_GIVE_UP_AFTER)
    {
      print_err("### Child: giving up (parent not responding)\n");
      return 1;
    }
  }
  count = 0;
  return 0;
}

/*
 * If the circular buffer is full, wait until it gains some room.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static inline int wait_if_buffer_full(circular_buffer_p  circular)
{
  static int count = 0;
#ifndef _WIN32
  struct timespec   time = {0,global_parent_wait*ONE_MS_AS_NANOSECONDS};
  int    err;
#endif // _WIN32

  while (circular_buffer_full(circular))
  {
#if DISPLAY_BUFFER
    if (global_show_circular && !global_parent_debug) print_msg("--> wait\n");
#endif
    if (global_parent_debug) print_msg("--> wait\n");
    count ++;

#ifdef _WIN32
    Sleep(global_parent_wait);
#else // _WIN32
    err = nanosleep(&time,NULL);
    if (err == -1 && errno == EINVAL)
    {
      fprint_err("### Parent: bad value (%ld) for wait time\n",time.tv_nsec);
      return 1;
    }
#endif // _WIN32

    if (circular_buffer_jammed(circular))
    {
      print_err("### Circular buffer jammed: No PCRs found\n");
      circular->eos = TRUE;
      return 1;
    }

    // If we wait for a *very* long time, maybe our child has crashed
    if (count > PARENT_GIVE_UP_AFTER)
    {
      print_err("### Parent: giving up (child not responding)\n");
      return 1;
    }
  }
  count = 0;
  return 0;
}

/*
 * Print out the buffer contents, prefixed by a prefix string
 */
static void print_circular_buffer(char              *prefix,
                                  circular_buffer_p  circular)
{
  int ii;
  if (prefix != NULL)
    fprint_msg("%s ",prefix);
  for (ii = 0; ii < circular->size; ii++)
  {
    byte* offset = circular->item_data + (ii * circular->item_size);
    fprint_msg("%s",(circular->start == ii ? "[":" "));
    if (*offset == 0)
      print_msg("..");
    else
      fprint_msg("%02x",*offset);
    fprint_msg("%s ",(circular->end == ii ? "]":" "));
  }
  print_msg("\n");
}

// ============================================================
// Low level buffered TS output support
// ============================================================



static void
reset_pcr_time(pcr_pace_env * const ppe, const uint64_t next_pcr_base)
{
  memset(ppe, 0, sizeof(*ppe));
  ppe->pcr_base = next_pcr_base;
  ppe->prime_speed = PRIME_SPEED_NORMAL;
  ppe->prime_last_pcr = INT64_MIN;
}




/*
 * Build a buffered output context
 *
 * - `writer` is the new buffered output context
 * - `circ_buf_size` is the number of buffer entries (plus one) we would
 *   like in the underlying circular buffer.
 * - `TS_in_packet` is the number of TS packets to allow in each network
 *   packet/circular buffer item.
 * - `maxnowait` is the maximum number of packets to send to the target
 *   host with no wait between packets
 * - `waitfor` is the number of microseconds to wait for thereafter
 * - `rate` is the (initial) rate at which we'd like to output our data
 * - `use_pcrs` is TRUE if PCRs in the data stream are to be used for
 *   timing output (the normal case), otherwise the specified byte rate
 *   will be used directly.
 * - `prime_size` is how much to prime the circular buffer output timer
 * - `prime_speedup` is the percentage of "normal speed" to use for the priming
 *   rate. This should normally be set to 100 (i.e., no effect).
 * - `pcr_scale` indicates how much PCRs should be "inflated"
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int build_buffered_TS_output(buffered_TS_output_p  *writer,
                                    int                    circ_buf_size,
                                    int                    TS_in_packet,
                                    int                    maxnowait,
                                    int                    waitfor,
                                    int                    rate,
                                    tswrite_pcr_mode       pcr_mode,
                                    int                    prime_size,
                                    int                    prime_speedup,
                                    double                 pcr_scale,
                                    const tswrite_pkt_hdr_type_t   hdr_type)
{
  int err, ii;
  circular_buffer_p     circular;
  buffered_TS_output_p  new = calloc(1, SIZEOF_BUFFERED_TS_OUTPUT);
  if (new == NULL)
  {
    print_err("### Unable to allocate buffered output\n");
    return 1;
  }
  reset_pcr_time(&new->pcr_pace, 0);

  err = map_circular_buffer(&circular,circ_buf_size,TS_in_packet,
                            maxnowait,waitfor,hdr_type);
  if (err)
  {
    print_err("### Error building buffered output\n");
    free(new);
    return 1;
  }
  new->buffer  = circular;
  new->started = FALSE;
  new->which   = (circular->pending + 1) % circular->size;
  new->num_packets = 0;

  new->rate = rate;
  new->pcr_mode = pcr_mode;
  new->prime_size = prime_size;
  new->prime_speedup = prime_speedup;

  new->pcr_scale = pcr_scale;

  new->pcr_pace.prime_speed = prime_speedup;
  new->pcr_pace.prime_req = (prime_speedup != PRIME_SPEED_NORMAL);
  fprint_msg("prime speed set to %d\n", prime_speedup);

  // And make sure we're absolutely safe against finding "false" PCR
  // values when we output the first few items...
  for (ii = 0; ii < MAX_TS_PACKETS_IN_ITEM; ii++)
    new->packet[ii].got_pcr = FALSE;
  
  *writer = new;
  return 0;
}

/*
 * Free a buffered output context
 *
 * `writer` is cleared and freed, and returned as NULL. The internal
 * circular buffer is unmapped.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int free_buffered_TS_output(buffered_TS_output_p  *writer)
{
  if ((*writer)->buffer != NULL)
  {
    int err = unmap_circular_buffer((*writer)->buffer);
    if (err) 
    {
      print_err("### Error freeing buffered output\n");
      return 1;
    }
  }
  (*writer)->buffer  = NULL;
  (*writer)->started = FALSE;
  
  free(*writer);
  *writer = NULL;
  return 0;
}


// Get a useful unsigned diff between two PCRs allowing for wrap through zero
#define PCR_WRAP (0x200000000LL * 300LL)
#define PCR_MS(n) ((int64_t)(n) * 90LL * 300LL)  // 27MHz units

static inline uint64_t pcr_delta_u(const uint64_t a, const uint64_t b)
{
  return a < b ? a + PCR_WRAP - b : a - b;
}

// ============================================================
// Timing
// ============================================================
/*
 * Set the time indicator for the next circular buffer item, using PCRs
 *
 * - `writer` is our buffered output context
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int set_buffer_item_time_pcr1(buffered_TS_output_p writer)
{
  int  ii;
  circular_buffer_p  circular = writer->buffer;

  static int32_t available_bytes = 0;
  static double  available_time  = 0;

  static int      last_pcr_index = -1;
  static uint64_t last_pcr;
  static double   pcr_rate = 0;
  static uint32_t last_timestamp_near_PCR = 0;

  static uint32_t last_timestamp = 0;

  static int had_first_pcr  = FALSE;  // Did we *have* a previous PCR?
  static int had_second_pcr = FALSE;  // And the second PCR is special, too

  // Remember our initial "priming" so we can replace it with a better
  // estimate later on
  static double  initial_prime_time  = 0;
  static int32_t initial_prime_bytes = 0;

  // Some simple statistics
  static int64_t  total_available_bytes = 0;
  static double   total_available_time = 0.0;
  static int      num_availables = 0;
  
  int      found_pcr = FALSE;
  int      num_bytes;
  double   num_microseconds;
  uint32_t timestamp;

  // A silly rate just means we haven't started yet...
  if (pcr_rate < 1.0)
    pcr_rate = writer->rate;
  
  // We start off with our time/bytes available zero to trigger this.
  // Thereafter, they should only really become zero/negative if we don't find
  // any PCRs.
  // Note that the greater `prime_size` is, the longer we can go between
  // PCRs, and the more smoothing effect we will have on the difference
  // in rates indicated by adjacent PCRs
  if (available_bytes <= 0 || available_time <= 0)
  {
    // We need to seed our time and data counts
    available_bytes = TS_PACKET_SIZE * circular->TS_in_item * writer->prime_size;
    available_time  = available_bytes * 1000000.0 /
      (pcr_rate * writer->prime_speedup/100.0);
    if (global_parent_debug)
      fprint_msg("PRIMING: bytes available %6d, time available %8.1f"
                 " (using rate %.1f x %d%%)\n",
                 available_bytes,available_time,pcr_rate,writer->prime_speedup);

    if (!had_second_pcr)
    {
      initial_prime_time  = available_time;
      initial_prime_bytes = available_bytes;
    }
  }

  // Have we got a PCR in our set-of-N packets?
  // For the moment, we're going to ignore the case where we have more than
  // one PCR in our set-of-N packets - it should be quite rare to have two
  // packets with PCRs that close together, and hopefully if we *do* get
  // such an instance, our compensation mechanisms will work it out.
  for (ii=0; ii<writer->num_packets; ii++)
  {
    if (writer->packet[ii].got_pcr)
    {
      found_pcr = TRUE;
      break;
    }
  }

  // Output our bytes using the prevailing conditions
  num_bytes = TS_PACKET_SIZE*writer->num_packets;
  num_microseconds = ((double)num_bytes / available_bytes) * available_time;
  timestamp = (uint32_t) (last_timestamp + num_microseconds);
  
  available_bytes -= num_bytes;
  available_time -= num_microseconds;

  if (global_parent_debug && global_show_all_times)
    fprint_msg("%06d:     num bytes %6d, time %8.1f, timestamp %8d"
               " => available bytes %6d, time %8.1f\n",
               writer->packet[0].index,num_bytes,num_microseconds,timestamp,
               available_bytes,available_time);

  if (found_pcr)
  {
    uint64_t delta_pcr = pcr_delta_u(writer->packet[ii].pcr, last_pcr);

    if (delta_pcr > PCR_MS(2000))
    {
      // We've suffered a discontinuity (quite likely because we've looped
      // back to the start of the file). We plainly don't want to continue
      // using previous PCRs as our basis for calculation, so let's fake
      // starting again...
      had_first_pcr = FALSE;
      had_second_pcr = FALSE;
      // And since we don't know what "time" is it, we'd better force
      // repriming next time round
      available_bytes = 0;
      available_time = 0.0;
    }
    else if (!had_first_pcr)
    {
      // This is our first PCR, so we can't do much with it except remember it
      had_first_pcr = TRUE;
      if (global_parent_debug)
        fprint_msg("%06d+%d: PCR %10" LLU_FORMAT_STUMP "\n",
                   writer->packet[0].index,ii,writer->packet[ii].pcr);
    }
    else
    {
      // This is our second or later PCR - we can calculate interesting things
      int     delta_bytes = (writer->packet[ii].index-last_pcr_index)*TS_PACKET_SIZE;
      int     extra_bytes;
      double  extra_time;
      pcr_rate = (delta_bytes * 27.0 / delta_pcr) * 1000000;
      extra_bytes = delta_bytes;
      extra_time  = extra_bytes * 1000000.0 / pcr_rate;

      available_bytes += extra_bytes;
      available_time += extra_time;

      total_available_bytes += available_bytes;
      total_available_time  += available_time;
      num_availables ++;
      
      if (global_parent_debug)
      {
        fprint_msg("%06d+%d: PCR %10" LLU_FORMAT_STUMP
                   ", rate %9.1f, add %6d/%8.1f  "
                   " => available bytes %6d, time %8.1f\n",
                   writer->packet[0].index,ii,writer->packet[ii].pcr,pcr_rate,
                   extra_bytes,extra_time,
                   available_bytes,available_time);
        fprint_msg("      (approximate actual rate %9.1f,"
                   " mean available bytes %8.1f, time %8.1f)\n",
                   1000000.0 * delta_bytes /
                   (timestamp - last_timestamp_near_PCR),
                   (double)total_available_bytes/num_availables,
                   total_available_time/num_availables);
      }
      if (!had_second_pcr)  // i.e., *this* is the second PCR
      {
        double old_time = available_time;
        // Our initial priming of the available bytes/time was based on
        // a guessed-at rate. However, now we have a real data rate, so
        // we can "remove" the original priming, and substitute one based
        // on this new rate (which will hopefully smooth out better)
        available_time -= initial_prime_time;
        available_time += initial_prime_bytes * 1000000.0 / pcr_rate;
        if (global_parent_debug)
          fprint_msg("RE-PRIMING: bytes available %6d, time available %8.1f"
                     " (was %8.1f) (using rate %.1f x %d%%)\n",
                     available_bytes,available_time,old_time,pcr_rate,
                     writer->prime_speedup);
        total_available_bytes = 0;
        total_available_time = 0.0;
        num_availables = 0;
        // And we mustn't do this again
        had_second_pcr = TRUE;
      }
    }
    last_timestamp_near_PCR = timestamp;
    last_pcr = writer->packet[ii].pcr;
    last_pcr_index = writer->packet[ii].index;
  }

  last_timestamp = circular->item[writer->which].time = timestamp;
  return writer->which;
}

static inline void
set_32_be(uint8_t * const p, const uint32_t x)
{
  p[0] = x >> 24;
  p[1] = (x >> 16) & 0xff;
  p[2] = (x >> 8) & 0xff;
  p[3] = x & 0xff;
}

static inline void
set_16_be(uint8_t * const p, const unsigned int x)
{
  p[0] = (x >> 8) & 0xff;
  p[1] = x & 0xff;
}


// Set times on all packets between where we were and where we are now
// Sets the time on both the first & last packets
// Returns the index of the last circ buffer entry modified
static int
set_circ_times(const circular_buffer_p circ,
    const uint32_t index_start, const uint32_t len_bytes,
    const int32_t pcr1_byte_offset, const uint64_t pcr1,
    const uint64_t pcr_gap, const uint32_t gap_bytes,
    const int64_t prime_last_pcr, const int prime_speed,
    uint64_t * const pNew_pcr_base)
{
  int32_t offset = pcr1_byte_offset;
  const int32_t end_offset = offset + len_bytes;
  int32_t i = index_start;
  int idx;

  do
  {
    struct circular_buffer_item * const item = circ->item + i;
    int64_t pcr = (int64_t)pcr1 + (int64_t)offset * (int64_t)pcr_gap / (int64_t)gap_bytes;
    int64_t adj_pcr = (pcr >= prime_last_pcr) ? pcr :
      prime_last_pcr - ((prime_last_pcr - pcr) * (int64_t)100) / (int64_t)prime_speed;

    if (circ->hdr_type == PKT_HDR_TYPE_RTP)
    {
      uint32_t timestamp = (uint32_t)(pcr / (uint64_t)300);
      uint8_t * rtp_buf = circ->item_data + i * circ->item_size - 12;

      rtp_buf[0] = 0x80;
      rtp_buf[1] = 33; // TS
      set_16_be(rtp_buf + 2, ++circ->hdr.rtp.seq);
      set_32_be(rtp_buf + 4, timestamp);
      set_32_be(rtp_buf + 8, circ->hdr.rtp.ssrc);
    }

    item->time = (uint32_t)(adj_pcr / 27);  // "time" in us
    offset += item->length;
    idx = i;

    if (++i >= circ->size)
      i = 0;
  } while (offset <= end_offset);

  // Predict PCR at the end of this packet if wanted
  if (pNew_pcr_base != NULL)
  {
    *pNew_pcr_base = (int64_t)pcr1 + (int64_t)offset * (int64_t)pcr_gap / (int64_t)gap_bytes;
  }

//  fprint_msg("s: %d->%d\n", index_start, idx);
  return idx;
}

static int
finalize_pcr_time(buffered_TS_output_p writer, pcr_pace_env * const ppe)
{
  const circular_buffer_p circ = writer->buffer;
  int idx = -1;

//  fprint_msg("%s\n", __func__);

  if (!ppe->gap_set)
  {
    // Can't do anything - forget any pcr we may have had - but
    // leave accumulated bytes to be output in the prologue of any subsequent
    // segment
    ppe->pcr1_set = FALSE;
  }
  else
  {
    if (ppe->next_bytes != 0)
    {
      idx = set_circ_times(circ, ppe->next_index, ppe->next_bytes - 1, ppe->next_offset, ppe->pcr1 + ppe->pcr_base,
          ppe->prev_pcr_gap, ppe->prev_gap_bytes, ppe->prime_last_pcr, ppe->prime_speed, &ppe->next_pcr_base);
//      fprint_msg("%s: idx %d->%d\n", __func__, ppe->next_index, idx);
    }

    reset_pcr_time(ppe, ppe->next_pcr_base);
  }

  return idx;
}

static int
discontinuity_pkt_pcr_time(buffered_TS_output_p writer, pcr_pace_env * const ppe)
{
  return finalize_pcr_time(writer, ppe);
}

static int
add_pkt_pcr_time(buffered_TS_output_p writer, pcr_pace_env * const ppe)
{
  const circular_buffer_p circ = writer->buffer;
  const circular_buffer_item_p item = circ->item + writer->which;
  const TS_packet_info_p pkt0 = writer->packet + 0;
  int idx = -1;

  item->discontinuity = FALSE;

retry:
  // Mark 1st packet after reset as discontinuity
  if (!ppe->pkt1)
  {
    ppe->pkt1 = TRUE;
    ppe->next_index = writer->which;
  }

  // If we have a pcr then we expect it to be on the 1st pkt in this group
  if (!pkt0->got_pcr)
  {
    ppe->gap_bytes += item->length;
    ppe->next_bytes += item->length;

//    fprint_msg("%u/%u\n", ppe->gap_bytes, ppe->next_bytes);
  }
  else
  {
    const uint64_t pcr1 = ppe->pcr1;
    const uint64_t pcr2 = pkt0->pcr;

//    fprint_msg("pcr: %lld\n", pcr2);

    if (!ppe->pcr1_set)
    {
      ppe->next_offset = 0 - ppe->next_bytes;
      ppe->next_bytes += item->length;
      ppe->pcr_base -= pcr2;
      // next_index set by discontinuity spotter
      if (ppe->prime_req)
      {
        // ** Really should account for bytes before 1st pcr
        ppe->prime_last_pcr = 27000000 * 5;
        ppe->prime_req = FALSE;
      }
      ppe->pcr1_set = TRUE;
    }
    else
    {
      const uint64_t pcr_gap = pcr_delta_u(pcr2, pcr1);

      if (pcr_gap > PCR_MS(2000))
      {
        // Discontinuity
        fprint_msg("PCR2: Discontinuity[%d]: gap=%lld\n", writer->which, pcr_gap);

        idx = finalize_pcr_time(writer, ppe);
        goto retry;
      }

      idx = set_circ_times(circ,
          ppe->next_index, ppe->next_bytes, ppe->next_offset, ppe->pcr_base + pcr1,
          pcr_gap, ppe->gap_bytes, ppe->prime_last_pcr, ppe->prime_speed, &ppe->next_pcr_base);
//      fprint_msg("%s: idx %d->%d (%d)\n", __func__, ppe->next_index, idx, writer->which);
      ppe->next_offset = item->length;
      ppe->next_bytes = 0;
      ppe->next_index = writer->which + 1;
      if (ppe->next_index >= circ->size)
        ppe->next_index = 0;

      // Remember in case we have to predict the next segment from this one
      ppe->prev_pcr_gap = pcr_gap;
      ppe->prev_gap_bytes = ppe->gap_bytes;
      ppe->gap_set = TRUE;

      // If non-discontinuity wrap then add wrap value to base time
      if (pcr1 > pcr2)
        ppe->pcr_base += PCR_WRAP;
    }

    ppe->pcr1 = pcr2;
    ppe->gap_bytes = item->length;
  }

  return idx;
}


/*
 * Set the time indicator for the next circular buffer item, based solely
 * on the rate selected by the user
 *
 * - `writer` is our buffered output context
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static int set_buffer_item_time_plain(buffered_TS_output_p writer)
{
  static uint32_t last_time = 0;      // The last circular buffer time stamp
  circular_buffer_p  circular = writer->buffer;
  int num_bytes = writer->num_packets * TS_PACKET_SIZE;// Bytes since last time
  uint32_t elapsed_time = (uint32_t) (num_bytes * 1000000.0 / writer->rate);
  last_time += elapsed_time;
  circular->item[writer->which].time = last_time;
  return writer->which;
}

/*
 * Set the time indicator for the next circular buffer item
 *
 * - `writer` is our buffered output context
 *
 * Returns new idx that can be written or -1 if unchanged
 */
static int set_buffer_item_time(const buffered_TS_output_p writer, const int finalize)
{
  switch (writer->pcr_mode)
  {
    case TSWRITE_PCR_MODE_PCR2:
      return finalize ?
        finalize_pcr_time(writer, &writer->pcr_pace) :
        add_pkt_pcr_time(writer, &writer->pcr_pace);
    case TSWRITE_PCR_MODE_PCR1:
      return set_buffer_item_time_pcr1(writer);
    case TSWRITE_PCR_MODE_NONE:
    default:
      // Allow the user to choose not to look at PCRs, and just do the
      // calculation based on the rate they've specified
      return set_buffer_item_time_plain(writer);
  }
}

// ============================================================
// EOF and the circular buffer
// ============================================================
/*
 * Add a buffer entry that is flagged to mean "EOF"
 *
 * This is done by inserting a circular buffer entry with length 1 and
 * first data byte 1 (instead of the normal 0x47 transport stream sync byte).
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int add_eof_entry(buffered_TS_output_p  writer)
{
  circular_buffer_p  circular = writer->buffer;

  int data_pos;
  int err = wait_if_buffer_full(circular);
  if (err)
  {
    print_err("### Internal error - waiting because circular buffer full\n");
    return 1;
  }
  
  // Work out where we want to write
  data_pos = (circular->pending + 1) % circular->size;

#if DISPLAY_BUFFER
  if (global_show_circular)
    fprint_msg("Parent: storing buffer %2d (EOF)\n",data_pos);
#endif

  // Set the `time` within the item appropriately (it doesn't really
  // matter for EOF, since we're not actually going to *write* anything
  // out, but it won't hurt to get it right)
  set_buffer_item_time(writer, TRUE);
  
  // And mark EOF by setting the first byte to something that isn't 0x47,
  // and the length to 1.
  circular->item_data[data_pos * circular->item_size] = 1;
  circular->item[data_pos].length = 1;
  circular->end = data_pos;
#if DISPLAY_BUFFER
  if (global_show_circular) print_circular_buffer("eof",circular);
#endif
  circular->eos = TRUE;
  return 0;
}

// ============================================================
// Output via buffered TS output
// ============================================================
/*
 * Flush the current circular buffer item. It must contain sensible data.
 *
 * - `writer` is our buffered output context
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static void internal_flush_buffered_TS_output(const buffered_TS_output_p  writer)
{
  const circular_buffer_p circular = writer->buffer;
  int idx;

  if (!writer->started || circular->item[writer->which].length == 0)
  {
    // Nothing to do
    return;
  }

  // Set the `time` within the item appropriately
  idx = set_buffer_item_time(writer, FALSE);
  if (idx >= 0)
    circular->end = idx;

  // Make this item available for reading
  circular->pending = writer->which;

  // And then prepare for the next index
  writer->which   = (circular->pending + 1) % circular->size;
  writer->started = FALSE;
  writer->num_packets   = 0;
  writer->packet[0].got_pcr = FALSE;  // Careful or paranoid?
}


static void discontinuity_buffered_TS_output(buffered_TS_output_p  writer)
{
  circular_buffer_p circular = writer->buffer;
  int idx;

  if (writer->pcr_mode != TSWRITE_PCR_MODE_PCR2)
    return;

  // Set the `time` within the item appropriately
  idx = discontinuity_pkt_pcr_time(writer, &writer->pcr_pace);
  if (idx >= 0)
    circular->end = idx;

  // We need to update the end of the circular buffer but we haven't added
  // any packets so no need to update any of that
}

/*
 * Write an EOF indicator to the buffered output
 *
 * This is done by flushing any current buffered output, and then
 * starting a new buffer item that contains a single byte, set to
 * 1 (in normal data, all TS packets start with 0x47, so this is
 * easily distinguished). The child fork knows that such a buffer
 * item signifies end of data.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_EOF_to_buffered_TS_output(buffered_TS_output_p  writer)
{
  int  err;

  // Make sure anything we were working on beforehand has been output
  internal_flush_buffered_TS_output(writer);

  if (global_parent_debug)
    print_msg("--> writing EOF\n");
  
  err = add_eof_entry(writer);
  if (err)
  {
    print_err("### Error adding EOF indicator\n");
    return 1;
  }
  return 0;
}

/*
 * Write the given TS packet out via the circular buffer.
 *
 * - `writer` is our buffered output
 * - `packet` is the TS packet
 * - `count` is its index in the input stream
 * - `pid` is its PID
 * - `got_pcr` is true if it contained a PCR
 * - in which case, `pcr` is the PCR
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_to_buffered_TS_output(buffered_TS_output_p  writer,
                                       byte                  packet[TS_PACKET_SIZE],
                                       int                   count,
                                       uint32_t              pid,
                                       int                   got_pcr,
                                       uint64_t              pcr)
{
  int  err;
  const circular_buffer_p circular =   writer->buffer;
  int               which;
  byte             *data;
  int              *length;

  // Force PCRs to start a buffer
  if (writer->pcr_mode == TSWRITE_PCR_MODE_PCR2 && got_pcr)
  {
//    fprint_msg("got_pcr: %lld\n", pcr);
    internal_flush_buffered_TS_output(writer);
  }

  which    =   writer->which;
  data     =   circular->item_data + which*circular->item_size;
  length   = &(circular->item[which].length);

  // If we haven't yet started writing to the (next) index in the
  // circular buffer, we must check that it is not full
  if (!writer->started)
  {
    err = wait_if_buffer_full(circular);
    if (err)
    {
      print_err("### Internal error - waiting because circular buffer full\n");
      return 1;
    }
    writer->started = TRUE;
    writer->num_packets = 0;
    *length = 0;
//    fprint_msg("> ");
  }

//  fprint_msg("[%d] @ %d\n", writer->which, *length);

  // Copy our data into the circular buffer item, and adjust appropriately
  memcpy(&(data[*length]),packet,TS_PACKET_SIZE);
  (*length) += TS_PACKET_SIZE;

  // Allow the user to specify that PCRs are inflated/deflated
  if (got_pcr)
  {
#if 0
    fprint_msg("@@ PCR %10" LLU_FORMAT_STUMP " * %g",pcr,writer->pcr_scale);
    fprint_msg(" => %10" LLU_FORMAT_STUMP "\n", (uint64_t)(pcr*writer->pcr_scale));
#endif
    pcr = (uint64_t)((double)pcr * writer->pcr_scale);
  }
  else
    pcr = 0;

  // Remember the other data we'll need later on
  writer->packet[writer->num_packets].index   = count;
  writer->packet[writer->num_packets].pid     = pid;
  writer->packet[writer->num_packets].got_pcr = got_pcr;
  writer->packet[writer->num_packets].pcr     = pcr;
  writer->num_packets ++;

  // Have we filled this entry in the circular buffer?
  if ((*length) >= circular->item_size - circular->hdr_size)
    internal_flush_buffered_TS_output(writer);
  return 0;
}

// ============================================================
// Child process - writing out data from the circular buffer
// ============================================================
/*
 * Wait for a given number of microseconds (or longer). Must be < 1s.
 *
 * Note that on a "normal" Linux or BSD machine, the shortest wait possible
 * may be as long as 10ms (10000 microseconds)
 *
 * On Windows, this will actually wait for a number of milliseconds,
 * using 0 milliseconds if the number of microseconds is too small.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
static void wait_microseconds(int  microseconds)
{
#ifdef _WIN32
  // Best we can (easily) do is to wait for the nearest (rounded down!)
  // number of milliseconds - hopefully this will do
  Sleep(microseconds / 1000);
#else // _WIN32
  struct  timespec   time = {0};
  struct  timespec   remaining;
  uint32_t nanoseconds = microseconds * 1000;
  int     err = 0;

  time.tv_sec = 0;
  time.tv_nsec = nanoseconds;

  errno = 0;
  err = nanosleep(&time,&remaining);
  while (err == -1 && errno == EINTR)  // cope with being woken too early
  {
    errno = 0;
    time = remaining;
    err = nanosleep(&time,&remaining);
  }
#endif // _WIN32
  return;
}

/*
 * Write data out to a file
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `data` is the data to write out
 * - `data_len` is how much of it there is
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_file_data(TS_writer_p  tswriter,
                           byte         data[],
                           int          data_len)
{
  size_t  written = 0;
  errno = 0;
  written = fwrite(data,1,data_len,tswriter->where.file);
  if (written != data_len)
  {
    fprint_err("### Error writing out TS packet data: %s\n",strerror(errno));
    return 1;
  }
  return 0;
}

/*
 * Write data out to a socket
 *
 * - `output` is a socket for our output
 * - `data` is the data to write out
 * - `data_len` is how much of it there is
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_socket_data(SOCKET output,
                             byte   data[],
                             int    data_len)
{
#ifdef _WIN32
  int      written = 0;
  int      left    = data_len;
#else  // _WIN32
  ssize_t  written = 0;
  ssize_t  left    = data_len;
#endif // _WIN32
  int     start   = 0;

  // (When writing to a file, we don't expect to ever write less than
  // the requested number of bytes. However, if `output` is a socket,
  // it is possible that the underlying buffering might cause a
  // partial write.)
  errno = 0;
  while (left > 0)
  {
    written = send(output,&(data[start]),left,0);
#ifdef _WIN32
    if (written == SOCKET_ERROR)
    {
      int err = WSAGetLastError();
      if (err == WSAENOBUFS)
      {
        print_err("!!! Warning: 'no buffer space available' writing out"
                  " TS packet data - retrying\n");
      }
      else
      {
        print_err("### Error writing out TS packet data:");
        print_winsock_err(err);
        print_err("\n");
        return 1;
      }
    }
#else // _WIN32
    if (written == -1)
    {
      if (errno == ENOBUFS)
      {
        print_err("!!! Warning: 'no buffer space available' writing out"
                  " TS packet data - retrying\n");
        errno = 0;
      }
      else
      {
        fprint_err("### Error writing out TS packet data: %s\n",
                   strerror(errno));
        return 1;
      }
    }
#endif // _WIN32
    left -= written;
    start += written;
  }
  return 0;
}

/*
 * Read a command character from the command input socket
 *
 * `command` comes in with the previous command character, and exits with
 * the current command character. `command_changed` is set TRUE if the
 * command character is changed, but *is not altered* if it is not
 * (i.e., it is up to someone else to "unset" `command_changed`).
 *
 * Returns 0 if all goes well, 1 if there is an error, and EOF if end-of-file
 * is read.
 */
static int read_command(SOCKET    command_socket,
                        byte     *command,
                        int      *command_changed)
{
  byte    thing;
#ifdef _WIN32
  int     length = recv(command_socket,&thing,1,0);
#else  
  ssize_t length = read(command_socket,&thing,1);
#endif
  if (length == 0)
  {
    print_err("!!! EOF reading from command socket\n");
    *command = COMMAND_QUIT;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[EOF -> quit]]\n");
#endif
    return 0;
    //return EOF;
  }
#ifdef _WIN32
  else if (length == SOCKET_ERROR)
  {
    int err = WSAGetLastError();
    print_err("!!! Error reading from command socket:");
    print_winsock_err(err);
    print_err("\n");
    *command = COMMAND_QUIT;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[Error -> quit]]\n");
#endif
    return 0;
    //return 1;
  }
#else
  else if (length == -1)
  {
    fprint_err("!!! Error reading from command socket: %s\n",strerror(errno));
    *command = COMMAND_QUIT;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[Error -> quit]]\n");
#endif
    return 0;
    //return 1;
  }
#endif

  switch (thing)
  {
  case 'q':
    *command = COMMAND_QUIT;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[quit]]\n");
#endif
    break;

  case 'n':
    *command = COMMAND_NORMAL;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[normal]]\n");
#endif
    break;

  case 'p':
    *command = COMMAND_PAUSE;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[pause]]\n");
#endif
    break;

  case 'f':
    *command = COMMAND_FAST;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[fast-forward]]\n");
#endif
    break;

  case 'F':
    *command = COMMAND_FAST_FAST;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[fast-fast-forward]]\n");
#endif
    break;

  case 'r':
    *command = COMMAND_REVERSE;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[reverse]]\n");
#endif
    break;

  case 'R':
    *command = COMMAND_FAST_REVERSE;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[fast-reverse]]\n");
#endif
    break;

  case '>':
    *command = COMMAND_SKIP_FORWARD;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[skip-forward]]\n");
#endif
    break;

  case '<':
    *command = COMMAND_SKIP_BACKWARD;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[skip-backward]]\n");
#endif
    break;

  case ']':
    *command = COMMAND_SKIP_FORWARD_LOTS;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[big-skip-forward]]\n");
#endif
    break;

  case '[':
    *command = COMMAND_SKIP_BACKWARD_LOTS;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[big-skip-backward]]\n");
#endif
    break;

  case '0':
    *command = COMMAND_SELECT_FILE_0;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-0]]\n");
#endif
    break;

  case '1':
    *command = COMMAND_SELECT_FILE_1;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-1]]\n");
#endif
    break;

  case '2':
    *command = COMMAND_SELECT_FILE_2;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-2]]\n");
#endif
    break;

  case '3':
    *command = COMMAND_SELECT_FILE_3;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-3]]\n");
#endif
    break;

  case '4':
    *command = COMMAND_SELECT_FILE_4;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-4]]\n");
#endif
    break;

  case '5':
    *command = COMMAND_SELECT_FILE_5;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-5]]\n");
#endif
    break;

  case '6':
    *command = COMMAND_SELECT_FILE_6;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-6]]\n");
#endif
    break;

  case '7':
    *command = COMMAND_SELECT_FILE_7;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-7]]\n");
#endif
    break;

  case '8':
    *command = COMMAND_SELECT_FILE_8;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-8]]\n");
#endif
    break;

  case '9':
    *command = COMMAND_SELECT_FILE_9;
    *command_changed = TRUE;
#if DEBUG_COMMANDS
    print_msg("[[select-file-9]]\n");
#endif
    break;
    
  case '\n':  // Newline is needed to send commands to us
#if DEBUG_COMMANDS
    print_msg("[[newline/ignored]]\n");
#endif
    break;    // so ignore it silently

  default:
#if DEBUG_COMMANDS
    fprint_msg("[[%c ignored]]\n",(isprint(thing)?thing:'?'));
#endif
    break;
  }
  return 0;
}

/*
 * Write data out to a socket using TCP/IP (and maybe reading commands as well)
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `data` is the data to write out
 * - `data_len` is how much of it there is. If this is 0, then we will
 *   not write any data (or, if command input is enabled, wait for permission
 *   to write data).
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_tcp_data(TS_writer_p  tswriter,
                          byte         data[],
                          int          data_len)
{
  int   err;

  if (tswriter->command_socket == -1)
  {
    if (data_len == 0)
      return 0;

    // If we're not soliciting commands, then our output socket will
    // be blocking, and we can just write to it...
    err = write_socket_data(tswriter->where.socket,data,data_len);
    if (err) return 1;
  }
  else
  {
    // Otherwise, we must check for command input, and also whether our
    // output socket is ready to be written to

    int    not_written = TRUE;
    fd_set read_fds, write_fds;

#if DEBUG_DATA_WAIT
    int    waiting = FALSE;
#endif

    int    num_to_check = max((int)tswriter->command_socket,
                              (int)tswriter->where.socket) + 1;

    while (not_written)
    {
      int result;

      FD_ZERO(&read_fds);
      FD_ZERO(&write_fds);

      // Only look for a new command if the last is not still outstanding
      // (remember, it is up to our caller to unset the "command changed" flag)
      if (!tswriter->command_changed)
        FD_SET(tswriter->command_socket,&read_fds);

      if (data_len > 0)
        FD_SET(tswriter->where.socket,&write_fds);

      result = select(num_to_check,&read_fds,&write_fds,NULL,NULL);
      if (result == -1)
      {
        fprint_err("### Error in select: %s\n",strerror(errno));
        return 1;
      }
      else if (result == 0) // Hmm - wouldn't expect this
        continue;           // So try again
      
      if (FD_ISSET(tswriter->command_socket,&read_fds))
      {
        err = read_command(tswriter->command_socket,
                           &tswriter->command,&tswriter->command_changed);
        if (err) return 1;
      }

      // Note that, unless we've quit, we always write out the outstanding
      // packet if we have been told that we *can* write.
      if (FD_ISSET(tswriter->where.socket,&write_fds))
      {
        err = write_socket_data(tswriter->where.socket,data,data_len);
        if (err) return 1;
        not_written = FALSE;
      }
      else if (data_len == 0)
        not_written = FALSE;  // well, sort of


#if DEBUG_DATA_WAIT
      if (not_written)
      {
        waiting = TRUE;
        fprint_msg(".. still waiting to write data (last command '%c', %s)..\n",
                   (isprint(tswriter->command)?tswriter->command:'?'),
                   (tswriter->command_changed?"changed":"unchanged"));
      }
      else if (waiting)
      {
        waiting = FALSE;
        fprint_msg(".. data written (last command '%c', %s)..\n",
                   (isprint(tswriter->command)?tswriter->command:'?'),
                   (tswriter->command_changed?"changed":"unchanged"));
      }
#endif
    }
  }
  return 0;
}

/*
 * Wait for a new command after 'p'ausing.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int wait_for_command(TS_writer_p  tswriter)
{
  if (tswriter->command_socket == -1)
  {
    print_err("### Cannot wait for new command when command input"
              " is not enabled\n");
    return 1;
  }
  else
  {
    int    err;
    fd_set read_fds;
    int    num_to_check = (int)tswriter->command_socket + 1;

    FD_ZERO(&read_fds);

    while (!tswriter->command_changed)
    {
      int result;
      FD_SET(tswriter->command_socket,&read_fds);
      result = select(num_to_check,&read_fds,NULL,NULL,NULL);
      if (result == -1)
      {
        fprint_err("### Error in select: %s\n",strerror(errno));
        return 1;
      }
      else if (result == 0) // Hmm - wouldn't expect this
        continue;           // So try again
      
      if (FD_ISSET(tswriter->command_socket,&read_fds))
      {
        err = read_command(tswriter->command_socket,
                           &tswriter->command,&tswriter->command_changed);
        if (err) return 1;
      }
    }
    return 0;
  }
}

/*
 * Write the next data item in our buffer
 *
 * - `output` is a socket for our output
 * - `circular` is our circular buffer of "packets"
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_circular_data(const SOCKET             output,
                               const circular_buffer_p  circular)
{
  int     err;
  byte   *buffer  = circular->item_data + circular->start*circular->item_size - circular->hdr_size;
  int     length  = circular->item[circular->start].length + circular->hdr_size;
#if DISPLAY_BUFFER
  int  oldend = circular->pending;
  int  oldstart = circular->start;
  int  newend,newstart;
#endif

  err = write_socket_data(output,buffer,length);

  if (err)
  {
    // If we're writing out over UDP, it's possible our write fails for
    // some reason. In general, it's best for us to ignore this, so that
    // the parent process can just keep dumping data to us, and we can
    // keep trying to write it.
    // In fact, probably the best thing to do is just *ignore* the error
    // at this level (write_socket_data will already have output some sort
    // of error or warning message).
  }

#if DISPLAY_BUFFER
  if (global_show_circular)
  {
    newend = circular->pending;
    newstart = circular->start;
    if (oldend != newend || oldstart != newstart)
    {
      fprint_msg("get [%2d,%2d] became [%2d,%2d]",
                 oldend,oldstart,newend,newstart);
      if (oldstart != newstart)
        print_msg(" (!!)");
      if (newstart == (newend + 1) % circular->size)
        print_msg(" ->empty");
      if ((newend + 2) % circular->size == newstart)
        print_msg(" ->full");
      print_msg("\n");
    }
  }
#endif

  // Once we've finished writing it, we can relinquish this entry in
  // the circular buffer
  buffer[0] = 0; // just for debug output's sake
  circular->start = (circular->start + 1) % circular->size;

#if DISPLAY_BUFFER
  if (global_show_circular)
    print_circular_buffer("<--",circular);
#endif
  return 0;
}

/*
 * Check if we have received an end-of-file indicator
 *
 * - `circular` is our circular buffer of "packets"
 *
 * Returns TRUE if we have received an end-of-file indicator, FALSE
 * if not.
 */
static int received_EOF(circular_buffer_p  circular)
{
  byte   *buffer  = circular->item_data + circular->start*circular->item_size;
  int     length = circular->item[circular->start].length;

  if (length == 1 && buffer[0] == 1)
  {
    // Relinquish the buffer entry, just in case...
    circular->start = (circular->start + 1) % circular->size;
#if DISPLAY_BUFFER
    if (global_show_circular)
    {
      print_msg("Child: found EOF\n");
      print_circular_buffer("<--",circular);
    }
#else
  if (child_parent_debug)
    print_msg("<-- found EOF\n");
#endif
    return TRUE;
  }
  else
    return FALSE;
}

/*
 * Calculate a value to perturb time by. Returns a number of microseconds.
 */
static int32_t perturb_time_by(void)
{
  static int  first_time = TRUE;
  unsigned    double_range;
  int32_t     result;

  if (first_time)
  {
    if (global_perturb_verbose)
      fprint_msg("... perturb seed %ld, range %u\n",
                 (long)global_perturb_seed,(unsigned)global_perturb_range);
    srand(global_perturb_seed);
    first_time = FALSE;
  }

  // We want values in the range -<range> .. <range>
  // So double the range to give us a number we can shift downwards
  // by <range> to get negative numbers as well, and add one to <range>
  // so we get 0..<range> instead of 0..<range>-1.
  double_range = (global_perturb_range+1) * 2;

  result = (unsigned int)((double)double_range * ((double)rand() / (RAND_MAX + 1.0)));

  // Shift it to give range centred on zero
  result -= global_perturb_range;

  if (global_perturb_verbose)
    fprint_msg("... perturb %ldms\n",(long)result);

  return result * 1000;
}

/*
 * Write the next data item in our buffer
 *
 * - `output` is a socket for our output
 * - `circular` is our circular buffer of "packets"
 * - if `quiet` then don't output extra messages (about filling up
 *   circular buffer)
 * - `had_eof` is set TRUE if we read a packet flagged to indicate
 *   that it is the end of data - this is how we know when to stop.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
static int write_from_circular(SOCKET             output,
                               circular_buffer_p  circular,
                               int                quiet,
                               int               *had_eof)
{
  int  err;

  // Are we starting up for the first time?
  static int starting = TRUE;

  // Do we need to (re)set our relative timeline? At the start we do.
  static int reset = TRUE;

  // Monitor time as seen by the parent
  // The parent prefixes each circular buffer item with the time
  // (in microseconds since some arbitrary start time) at which it would
  // like it to be displayed. For a constant rate bitstream, these "ticks"
  // will be evenly spaced.
         uint32_t this_packet_time;     // time stamp for this packet
  static uint32_t last_packet_time = 0; // time stamp for last packet
  int32_t packet_time_gap;  // the difference between the two, in microseconds

  // Monitor time as seen by us
  // We have to deduce both an arbitrary start time from which to measure
  // "ticks", and also when we should (according to the requested gaps,
  // and the progress through time) be outputing the next packet - i.e.,
  // as near to the correct tick as possible.
  struct timeval now;
  static struct timeval start = {0,0};  // our arbitrary start time
  uint32_t our_time_now;    // our time, relative to our start time
  static int32_t  delta_start;  // difference between our time and the parent's
  uint32_t adjusted_now;   // our time, adjusted by delta_start
  int32_t  waitfor; // how long we think we need to wait to adjust

  // How many items have we sent without *any* delay?
  // (not used if maxnowait is off)
  static int sent_without_delay = 0;

  // When grumbling about having had to restart our time sequence,
  // it is nice to be able to say which packet we were outputting
  // (so the user can tell how frequently we're doing this)
  static unsigned int count = 0;

  count ++;

  if (starting)
  {
    // If we're starting up for the first time, it's probably worth waiting
    // for the circular buffer to fill up
    if (!quiet)
      print_msg("Circular buffer filling...\n");
    err = wait_for_buffer_to_fill(circular);
    if (err)
    {
      print_err("### Error - waiting for circular buffer to fill\n");
      return 1;
    }
    if (!quiet)
      print_msg("Circular buffer filled - starting to send data\n");
    starting = FALSE;
  }
  else
  {
    // If the buffer is empty, there's really not much else we can do but
    // wait for it not to be empty.
    err = wait_if_buffer_empty(circular);
    if (err)
    {
      print_err("### Error - waiting because circular buffer is empty\n");
      return 1;
    }
  }

  // If the next item is an end-of-file indicator, we can exit at once
  // - we don't need to wait for the right time to "write" it
  if (received_EOF(circular))
  {
    *had_eof = TRUE;
    return 0;
  }

  // Work out the interval that the parent is asking for
  this_packet_time = circular->item[circular->start].time;
  packet_time_gap  = this_packet_time - last_packet_time;

  // Work out the actual position on our own timeline
  gettimeofday(&now, NULL);
  // We're *actually* at this distance along our time line
  our_time_now = (now.tv_sec - start.tv_sec) * 1000000 +
    (now.tv_usec - start.tv_usec);

  if (global_perturb_range)
  {
    // Add a (positive or negative) delta to that so that our
    // time appears to jump around a bit, hopefully leading to
    // an output that looks like an unreliable network delay
    our_time_now += perturb_time_by();
  }

  // Check whether we've asked for a reset, or if the parent process
  // has told us that the timeline has changed radically
  if (reset || circular->item[circular->start].discontinuity)
  {
//    fprint_msg("%s: Discontinuity[%d]: reset=%d, pkt_time=%u\n", __func__, circular->start, reset, this_packet_time);

    // We believe out timeline has gone askew - start a new one
    // Set up "now" as our base time, and output our packet right away
    start = now;
    our_time_now = 0;
    delta_start =  this_packet_time;
    waitfor = 0;
    if (global_child_debug)
      fprint_msg("<-- packet %6u, gap %6u; STARTING delta %6d ",
                 this_packet_time,packet_time_gap,delta_start);
    reset = FALSE;
  }
  else
  {
    // We can try to relate that to the parent's timeline
    adjusted_now = our_time_now + delta_start;

    // So how long do we (notionally) need to wait for the right time?
    waitfor = this_packet_time - adjusted_now;

    if (global_child_debug)
      fprint_msg("<-- packet %6u, gap %6u; our time %6u = %6u -> wait %6d ",
                 this_packet_time,packet_time_gap,our_time_now,adjusted_now,
                 waitfor);
  }

  // So how long *should* we wait for the correct time to write?
  if (waitfor > 0)
  {
    if (waitfor > 200000)
    {
      fprint_msg("###[%d] (%d) >0.2s, RESET\n", circular->start, waitfor);
      reset = TRUE;
      waitfor = 200000;
    }
    if (global_child_debug) print_msg("(waiting");
  }
  else if (waitfor > -200000) // less than 0.2 seconds gap - "small", so ignore
  {
    if (global_child_debug) print_msg("(<0.2s, ignore");
    waitfor = 0;
  }
  else // more than 0.2 seconds - makes us reset our idea of time
  {
    if (global_perturb_range == 0) // but only if we're not mucking about with time
    {
      if (global_child_debug)
        print_msg("(>0.2s, RESET");
      else
      {
        // Let the user know we're having some problems.
        // Use the amended `count` as the primary index since the parent
        // process logs progress in terms of the number of TS packets
        // output - (count-1)*7+1 should be the index of the first packet
        // in our circular buffer item, which is a decent approximation
        fprint_err("!!! [%d] Packet %d (item %d): Outputting %.2fs late -"
                   " restarting time sequence: time=%u\n",
            circular->start,
                   (count-1)*7+1,count,-(double)waitfor/1000000, this_packet_time);
        if (circular->maxnowait >= 0)
          fprint_err("    Maybe consider running with -maxnowait greater"
                     " than %d\n",circular->maxnowait);
      }
      // Ask for a reset, and output the packet right away
      reset = TRUE;
      waitfor = 0;
    }
  }

  // We are not allowed to send more than three consecutive packets
  // with no delay (or we might swamp the receiving hardware)
  if (waitfor == 0 && circular->maxnowait != -1)
  {
    if (sent_without_delay < circular->maxnowait)
    {
      sent_without_delay ++;
      if (global_child_debug) fprint_msg(", %d)\n",sent_without_delay);
    }
    else
    {
      if (global_child_debug) fprint_msg(", %d -> wait)\n",
                                         sent_without_delay+1);
      waitfor = circular->waitfor; // enforce a minimal wait
    }
  }
  else
    if (global_child_debug) print_msg(")\n");

  // So, finally, do we need to wait before writing?
  if (waitfor > 0)
  {
    wait_microseconds(waitfor);
    sent_without_delay = 0;
  }

  // Write it...
  err = write_circular_data(output,circular);
  if (err) return 1;

  // Don't forget to update our memory before we finish
  last_packet_time = this_packet_time;
  return 0;
}

/*
 * The child process just writes the contents of the circular buffer out,
 * as it receives it.
 *
 * - `tswriter` is the context to use for writing TS output
 *
 * The intent is that, after forking, all the code needs to do is::
 *
 *   else if (pid == 0)
 *   {
 *     _exit(tswrite_child_process(tswriter));
 *   }
 *
 * and the child process will "just work".
 *
 * Note that the end of the data to read/write is detected when a
 * circular buffer entry with length 1 and first data byte 1 is found
 * (see `write_EOF_to_buffered_TS_output()`)
 *
 * Returns the value that should be returned by the the child process
 * (0 for success, 1 for failure).
 */
static int tswrite_child_process(TS_writer_p  tswriter)
{
  int had_eof = FALSE;
  for (;;)
  {
    int err = write_from_circular(tswriter->where.socket,
                                  tswriter->writer->buffer,
                                  tswriter->quiet,
                                  &had_eof);
    if (err) return 1;
    if (had_eof) break;
  }
  return 0;
}
#ifdef _WIN32
// ============================================================
// Windows threading ("fork" alternative)
// ============================================================
/*
 * Wrapper for tswrite_child_process, used to coerce args, etc.
 */
static void child_thread_fn(void_p arg)
{
  TS_writer_p  tswriter = (TS_writer_p)arg;
  (void) tswrite_child_process(tswriter);

#ifdef _WIN32
  {
    int err;
    // On Windows, only the "child" knows when it has finished using its
    // resources (i.e., the circular buffer and output socket), so only the
    // "child" can sensibly release them...
    err = disconnect_socket(tswriter->where.socket);
    if (err == EOF)
      fprint_err("### Error closing output: %s\n",strerror(errno));

    // And free the buffering stuff
    err = free_buffered_TS_output(&(tswriter->writer));
    if (err)
      print_err("### Error freeing TS buffer\n");

    free(tswriter);
  }
#endif
}

/*
 * Start up the child thread, to handle the circular buffering
 */
static int start_child(TS_writer_p  tswriter)
{
  tswriter->child = (HANDLE) _beginthread(child_thread_fn,0,(void_p)tswriter);

  if (tswriter->child == (HANDLE) -1)
  {
    fprint_err("Error creating child process: %s\n",strerror(errno));
    return 1;
  }
  return 0;
}
#else  // _WIN32
// ============================================================
// Unix forking ("thread" alternative)
// ============================================================
/*
 * Start up the child fork, to handle the circular buffering
 */
static int start_child(TS_writer_p  tswriter)
{
  pid_t pid;

  tswriter->child = 0;

  pid = fork();
  if (pid == -1)
  {
    fprint_err("Error forking: %s\n",strerror(errno));
    return 1;
  }
  else if (pid == 0)
  {
    // Aha - we're the child
    _exit(tswrite_child_process(tswriter));
  }

  // Otherwise, we're the parent - carry on
  tswriter->child = pid;
  return 0;
}

/*
 * Wait for the child fork to exit
 */
static int wait_for_child_to_exit(TS_writer_p  tswriter,
                                  int          quiet)
{
  int    err;
  pid_t  result;
  if (!quiet) print_msg("Waiting for child to finish writing and exit\n");
  result = waitpid(tswriter->child,&err,0);
  if (result == -1)
  {
    fprint_err("### Error waiting for child to exit: %s\n",
               strerror(errno));
    return 1;
  }
  if (WIFEXITED(err))
  {
    if (!quiet) print_msg("Child exited normally\n");
  }
  tswriter->child = 0;
  return 0;
}
#endif // _WIN32

// ============================================================
// Writing
// ============================================================
/*
 *
 * Build the basics of a TS writer context.
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
static int tswrite_build(TS_WRITER_TYPE  how,
                         int             quiet,
                         TS_writer_p    *tswriter)
{
  TS_writer_p  new = NULL;
  new = malloc(SIZEOF_TS_WRITER);
  if (new == NULL)
  {
    print_err("### Unable to allocate space for TS_writer datastructure\n");
    return 1;
  }
  new->how = how;
  new->writer = NULL;
  new->child = 0;
  new->count = 0;
  new->quiet = quiet;
  new->server = FALSE;            // not being a server
  new->command_socket = -1;       // not taking commands
  new->command = COMMAND_PAUSE;   // start in pause
  new->command_changed = FALSE;   // no new command
  new->atomic_command = FALSE;    // but any command is interruptable
  new->drop_packets = 0;
  *tswriter = new;
  return 0;
}

/*
 * Open a file for TS output.
 *
 * - `how` is how to open the file or connect to the host
 * - `name` is the name of the file or host to open/connect to
 *   (this is ignored if `how` is TS_W_STDOUT)
 * - if `how` is TS_W_UDP, and `name` is a multicast address,
 *   then `multicast_if` is the IP address of the network
 *   address to use, or NULL if the default interface should
 *   be used. If `how` is not TS_W_UDP, `multicast_if` is ignored.
 * - if it is a socket (i.e., if `how` is TS_W_TCP or TS_W_UDP),
 *   then `port` is the port to use, otherwise this is ignored
 * - `quiet` is true if only error messages should be printed
 * - `tswriter` is the new context to use for writing TS output,
 *   which should be closed using `tswrite_close`.
 *
 * For TS_W_STDOUT, there is no need to open anything.
 *
 * For TS_W_FILE, ``open(name,O_CREAT|O_WRONLY|O_TRUNC|O_BINARY,00777)``
 * is used - i.e., the file is opened so that anyone may read/write/execute
 * it. If ``O_BINARY`` is not defined (e.g., on Linux), then it is
 * omitted.
 *
 * For TS_W_TCP and TS_W_UDP, the ``connect_socket`` function is called,
 * which uses ``socket`` and ``connect``.
 *
 * In all cases (even when using TS_W_STDOUT), the `tswriter` should be
 * closed using `tswrite_stdout`.
 *
 * For TS_W_UDP, the ``tswrite_start_buffering`` function must be called
 * before any output is written via the `tswriter`. For the other forms of
 * output, this is not allowed.
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
extern int tswrite_open(TS_WRITER_TYPE  how,
                        char           *name,
                        char           *multicast_if,
                        int             port,
                        int             quiet,
                        TS_writer_p    *tswriter)
{
  TS_writer_p  new;
  int err = tswrite_build(how,quiet,tswriter);
  if (err) return 1;

  new = *tswriter;
  switch (how)
  {
  case TS_W_STDOUT:
    if (!quiet) print_msg("Writing to <stdout>\n");
    new->where.file = stdout;
    break;
  case TS_W_FILE:
    if (!quiet) fprint_msg("Writing to file %s\n",name);
    new->where.file = fopen(name,"wb");
    if (new->where.file == NULL)
    {
      fprint_err("### Unable to open output file %s: %s\n",
                 name,strerror(errno));
      return 1;
    }
    break;
  case TS_W_TCP:
    if (!quiet) fprint_msg("Connecting to %s via TCP/IP on port %d\n",
                           name,port);
    new->where.socket = connect_socket(name,port,TRUE, NULL);
    if (new->where.socket == -1)
    {
      fprint_err("### Unable to connect to %s\n",name);
      return 1;
    }
    if (!quiet) fprint_msg("Writing    to %s via TCP/IP\n",name);
   break;
  case TS_W_UDP:
    if (!quiet)
    {
      // We don't *know* at this stage if the `name` *is* a multicast address,
      // but we'll assume the user only specifies `multicast_if` is it is, for
      // the purposes of these messages (amending `connect_socket`, which does
      // know, to output this message iff `!quiet` is a bit overkill)
      fprint_msg("Connecting to %s via UDP on port %d",name,port);
      if (multicast_if)
        fprint_msg(" (multicast interface %s)",multicast_if);
      print_msg("\n");
    }
    new->where.socket = connect_socket(name,port,FALSE,multicast_if);
    if (new->where.socket == -1)
    {
      fprint_err("### Unable to connect to %s\n",name);
      return 1;
    }
    if (!quiet) fprint_msg("Writing    to %s via UDP\n",name);
    break;
  default:
    fprint_err("### Unexpected writer type %d to tswrite_open()\n",how);
    free(new);
    return 1;
  }
  return 0;
}

/*
 * Open a network connection for TS output.
 *
 * This is a convenience wrapper around `tswrite_open`.
 *
 * - `name` is the name of the host to connect to
 * - `port` is the port to connect to
 * - `use_tcp` is TRUE if TCP/IP should be use, FALSE if UDP should be used
 * - `quiet` is true if only error messages should be printed
 * - `tswriter` is the new context to use for writing TS output,
 *   which should be closed using `tswrite_close`.
 *
 * In all cases, the `tswriter` should be closed using `tswrite_stdout`.
 *
 * For TS_W_UDP, the ``tswrite_start_buffering`` function must be called
 * before any output is written via the `tswriter`. For other forms of output,
 * this not allowed.
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
extern int tswrite_open_connection(int             use_tcp,
                                   char           *name,
                                   int             port,
                                   int             quiet,
                                   TS_writer_p    *tswriter)
{
  return tswrite_open((use_tcp?TS_W_TCP:TS_W_UDP),name,NULL,port,quiet,tswriter);
}

/*
 * Open a file for TS output.
 *
 * This is a convenience wrapper around `tswrite_open`.
 *
 * - `name` is the name of the file to open, or NULL if stdout should be used
 * - `quiet` is true if only error messages should be printed
 * - `tswriter` is the new context to use for writing TS output,
 *   which should be closed using `tswrite_close`.
 *
 * In all cases (even when using TS_W_STDOUT), the `tswriter` should be
 * closed using `tswrite_stdout`.
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
extern int tswrite_open_file(char           *name,
                             int             quiet,
                             TS_writer_p    *tswriter)
{
  return tswrite_open((name==NULL?TS_W_STDOUT:TS_W_FILE),name,NULL,0,quiet,
                      tswriter);
}

/*
 * Wait for a client to connect and then both write TS data to it and
 * listen for command from it. Uses TCP/IP.
 *
 * - `server_socket` is the socket on which we will listen for a connection
 * - `quiet` is true if only error messages should be printed
 * - `tswriter` is the new context to use for writing TS output,
 *   which should be closed using `tswrite_close`.
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
extern int tswrite_wait_for_client(int           server_socket,
                                   int           quiet,
                                   TS_writer_p  *tswriter)
{
  TS_writer_p  new;
  int err = tswrite_build(TS_W_TCP,quiet,tswriter);
  if (err) return 1;
  new = *tswriter;

  new->server = TRUE;

  // Listen for someone to connect to it
  err = listen(server_socket,1);
  if (err == -1)
  {
#ifdef _WIN32
    err = WSAGetLastError();
    print_err("### Error listening for client: ");
    print_winsock_err(err);
    print_err("\n");
#else  // _WIN32      
    fprint_err("### Error listening for client: %s\n",strerror(errno));
#endif // _WIN32
    return 1;
  }

  // Accept the connection
  new->where.socket = accept(server_socket,NULL,NULL);
  if (new->where.socket == -1)
  {
#ifdef _WIN32
    err = WSAGetLastError();
    print_err("### Error accepting connection: ");
    print_winsock_err(err);
    print_err("\n");
#else  // _WIN32      
    fprint_err("### Error accepting connection: %s\n",strerror(errno));
#endif // _WIN32
    return 1;
  }
  return 0;
}

/*
 * Set up internal buffering for TS output. This is necessary for UDP
 * output, and not allowed for other forms of output.
 *
 * 1. Builds the internal circular buffer and other datastructures
 * 2. Starts a child process to read from the circular buffer and send
 *    data over the socket.
 * 3. Starts a parent process which calls the supplied function, which
 *    is expected to use `tswrite_write()` to write to the circular
 *    buffer.
 *
 * See also `tswrite_start_buffering_from_context`, which uses the `context`
 * datastructure that is prepared by `tswrite_process_args`.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `circ_buf_size` is the number of buffer entries (plus one) we would
 *   like in the underlying circular buffer.
 * - `TS_in_packet` is the number of TS packets to allow in each network
 *   packet.
 * - `maxnowait` is the maximum number of packets to send to the target
 *   host with no wait between packets
 * - `waitfor` is the number of microseconds to wait for thereafter
 * - `byterate` is the (initial) rate at which we'd like to output our data
 * - `use_pcrs` is TRUE if PCRs in the data stream are to be used for
 *   timing output (the normal case), otherwise the specified byte rate
 *   will be used directly.
 * - `prime_size` is how much to prime the circular buffer output timer
 * - `prime_speedup` is the percentage of "normal speed" to use for the priming
 *   rate. This should normally be set to 100 (i.e., no effect).
 * - `pcr_scale` determines how much to "accelerate" each PCR - see the
 *   notes elsewhere on how this works.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int tswrite_start_buffering(TS_writer_p  tswriter,
                                   int          circ_buf_size,
                                   int          TS_in_packet,
                                   int          maxnowait,
                                   int          waitfor,
                                   int          byterate,
                                   tswrite_pcr_mode pcr_mode,
                                   int          prime_size,
                                   int          prime_speedup,
                                   double       pcr_scale,
                                   const tswrite_pkt_hdr_type_t hdr_type)
{
  int   err;

  if (tswriter->how != TS_W_UDP)
  {
    fprint_err("### Buffered output not supported for %s output\n",
               (tswriter->how == TS_W_TCP?"TCP/IP":
                tswriter->how == TS_W_FILE?"file":
                tswriter->how == TS_W_STDOUT?"<standard output>":"???"));
    return 1;
  }
  
  err = build_buffered_TS_output(&(tswriter->writer),
                                 circ_buf_size,TS_in_packet,
                                 maxnowait,waitfor,byterate,pcr_mode,
                                 prime_size,prime_speedup,pcr_scale,
	                             hdr_type);
  if (err) return 1;

  err = start_child(tswriter);
  if (err) 
  {
    (void) free_buffered_TS_output(&tswriter->writer);
    return 1;
  }
  return 0;
}

/*
 * Set up internal buffering for TS output. This is necessary for UDP
 * output, and not allowed for other forms of output.
 *
 * This alternative takes the `context` datastructure that is prepared
 * by `tswrite_process_args`.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `context` contains the necessary information, as given by the user
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int tswrite_start_buffering_from_context(TS_writer_p  tswriter,
                                                TS_context_p context)
{
  return tswrite_start_buffering(tswriter,
                                 context->circ_buf_size,
                                 context->TS_in_item,
                                 context->maxnowait,
                                 context->waitfor,
                                 context->byterate,
                                 context->pcr_mode,
                                 context->prime_size,
                                 context->prime_speedup,
                                 context->pcr_scale,
                                 context->pkt_hdr_type);
}

/*
 * Indicate to a TS output context that `input` is to be used as
 * command input.
 *
 * This function may only be used if output is via TCP/IP.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `input` is the socket (or, on Linux/BSD, file descriptor) on which
 *   to listen for commands.
 *
 *   Note that this should either be ``tswriter->where.socket`` or
 *   STDIN_FILENO - no other values are currently supported (particularly
 *   since no attempt is made to close this socket when things are finished,
 *   which doesn't matter for the given values).
 *
 * This function:
 *
 * - makes the socket on which data will be written non-blocking
 *   (i.e., if the socket is not ready to be written to, it will not
 *   accept input and block until it can be used, which means that it
 *   becomes our responsibility to ask if the socket is ready for output)
 * - makes tswrite_write "look" on the `input` to see if a (single
 *   character) command has been given, and if it has, put it into
 *   the `tswriter` datastructure for use
 *
 * The command state is set to 'p'ause - i.e., as if the client had sent
 * a COMMAND_PAUSE command.
 *
 * Returns 0 if all went well, 1 if something went wrong.
 */
extern int tswrite_start_input(TS_writer_p  tswriter,
                               SOCKET       input)
{
  int err;
#ifdef _WIN32
  u_long  one = 1;
#else
  int flags;
#endif // _WIN32

  if (tswriter->how != TS_W_TCP)
  {
    print_err("### Command input is only supported for TCP/IP\n");
    return 1;
  }

  // Make our output socket non-blocking
#ifdef _WIN32
  err = ioctlsocket(tswriter->where.socket,FIONBIO,&one);
  if (err == SOCKET_ERROR)
  {
    err = WSAGetLastError();
    print_err("### Unable to set socket nonblocking: ");
    print_winsock_err(err);
    print_err("\n");
    return 1;
  }
#else  // _WIN32  
  flags = fcntl(tswriter->where.socket,F_GETFL,0);
  if (flags == -1)
  {
    fprint_err("### Error getting flags for output socket: %s\n",
               strerror(errno));
    return 1;
  }
  err = fcntl(tswriter->where.socket,F_SETFL,flags | O_NONBLOCK);
  if (err == -1)
  {
    fprint_err("### Error setting output socket non-blocking: %s\n",
               strerror(errno));
    return 1;
  }
#endif  // _WIN32

  tswriter->command_socket = input;
  tswriter->command = COMMAND_PAUSE;
  return 0;
}

/*
 * Set/unset "atomic" status - i.e., whether a command may be interrupted
 * by the next command.
 *
 * Most commands (normal play, fast forwards, etc.) should be interrupted
 * by a new command. However, some (the skip forwards and backwards commands)
 * make sense only if they will always complete. This function allows that
 * state to be toggled.
 */
extern void tswrite_set_command_atomic(TS_writer_p  tswriter,
                                       int          atomic)
{
  tswriter->atomic_command = atomic;
}

/*
 * Ask a TS writer if changed input is available.
 *
 * If the TS writer is enabled for command input, then if the command
 * currently being executed has declared itself "atomic" (i.e., not able to be
 * interrupted), it returns FALSE, otherwise it returns TRUE if the command
 * character has changed.
 */
extern int tswrite_command_changed(TS_writer_p  tswriter)
{
  if (tswriter->command_socket == -1)
    return FALSE;
  else
  {
    if (tswriter->atomic_command)
      return FALSE;
    else
      return tswriter->command_changed;
  }
}

/*
 * Finish off buffered output, and wait for the child to exit
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `quiet` should be true if only error messages are to be output
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
static int tswrite_close_child(TS_writer_p  tswriter,
                               int          quiet)
{
  int    err;

  if (tswriter->writer == NULL)
    return 0;

  if (tswriter->child == 0)
    return 0;

  if (tswriter->writer)
  {
    // We're writing to a child through a circular buffer 
    // Indicate "end of file" to the child
    err = write_EOF_to_buffered_TS_output(tswriter->writer);
    if (err)
    {
      print_err("### Error adding EOF indicator to TS buffer\n");
      (void) free_buffered_TS_output(&tswriter->writer);
      return 1;
    }
  }

#ifndef _WIN32
  // On Linux/BSD, we have forked, and thus it is reasonable for the parent
  // process to tidy up when it has finished (since the child process is in
  // separate memory space). On Windows, this has to be done by the "child".

  // So wait for the child to complete
  err = wait_for_child_to_exit(tswriter,quiet);
  if (err)
  {
    (void) free_buffered_TS_output(&tswriter->writer);
    return 1;
  }

  if (tswriter->writer)
  {
    // And free the shared memory resources
    err = free_buffered_TS_output(&(tswriter->writer));
    if (err)
    {
      print_err("### Error freeing TS buffer\n");
      return 1;
    }
  }
#endif // not _WIN32
  return 0;
}

/*
 * Close a file or socket.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
static int tswrite_close_file(TS_writer_p  tswriter)
{
  int err;

  switch (tswriter->how)
  {
  case TS_W_STDOUT:
    // Nothing to do for standard output
    break;
  case TS_W_FILE:
    err = fclose(tswriter->where.file);
    if (err == EOF)
    {
      fprint_err("### Error closing output: %s\n",strerror(errno));
      return 1;
    }
    break;
  case TS_W_TCP:
  case TS_W_UDP:
    err = disconnect_socket(tswriter->where.socket);
    if (err == EOF)
    {
      fprint_err("### Error closing output: %s\n",strerror(errno));
      return 1;
    }
    break;
  default:
    fprint_err("### Unexpected writer type %d to tswrite_close()\n",
               tswriter->how);
    return 1;
  }
  return 0;
}

/*
 * Close a file or socket opened using `tswrite_open`, and if necessary,
 * send the child process used for output buffering an end-of-file
 * indicator, and wait for it to finish.
 *
 * Also frees the TS writer datastructure.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - if `quiet` is true, then waiting for the child to exit should
 *   not be reported on (i.e., only errors should produce output)
 *
 * Returns 0 if all goes well, 1 if something went wrong.
 */
extern int tswrite_close(TS_writer_p  tswriter,
                         int          quiet)
{
  int err;

  if (tswriter == NULL)
    return 0;

  // Only does anything if there *is* a child to close/buffer to shut down
  err = tswrite_close_child(tswriter,quiet);
  if (err)
  {
    print_err("### Error closing child process\n");
#ifdef _WIN32
    if (!tswriter->writer)
    {
#endif
      (void) tswrite_close_file(tswriter);
      free(tswriter);
#ifdef _WIN32
    }
#endif
    return 1;
  }

#ifdef _WIN32
  if (tswriter->writer)
  {
    // We're doing buffered output. On Windows, this means that we are using a
    // parent thread and a child thread. Only one thread should close/free the
    // remaining resources, and since only the child thread knows when it
    // stops, it has to be the child thread that does it. This function is
    // called by the parent thread, so it should not. Moreover, having asked
    // the child thread to shut down (above), it cannot tell when the child
    // will have free the tswriter, so it must not refer to it again...
  }
  else
  {
#endif
    err = tswrite_close_file(tswriter);
    if (err)
    {
      print_err("### Error closing output\n");
      free(tswriter);
      return 1;
    }
  
    if (!quiet)
      fprint_msg("Output %d TS packets\n",tswriter->count);

    free(tswriter);
#ifdef _WIN32
  }
#endif
  return 0;
}

/*
 * Write a Transport Stream packet out via the TS writer.
 *
 * - `tswriter` is the TS output context returned by `tswrite_open`
 * - `packet` is the TS packet
 * - if the packets payload_unit_start_indicator is set, then
 *   `pid` is the PID for this packet, `got_pcr` is TRUE if it
 *   contains a PCR in its adaptation field, and `pcr` contains
 *   said PCR. These values are only used when outputting via
 *   buffered output.
 *
 * Returns 0 if all goes well, 1 if something went wrong, and EOF if command
 * input is enabled (only allowed for TCP/IP output) and the 'q'uit command
 * has been given (in which case, no further commands will be read, and no
 * more output will be written, by any subsequent calls of this function).
 */
extern int tswrite_write(TS_writer_p  tswriter,
                         byte         packet[TS_PACKET_SIZE],
                         uint32_t     pid,
                         int          got_pcr,
                         uint64_t     pcr)
{
  int err;

  if (tswriter->drop_packets)
  {
    // Output drop_packets packets, and then omit drop_number
    static int packet_count = 0;
    static int drop_count = 0;
    if (drop_count > 0)  // we're busy ignoring packets
    {
#if 0
      print_msg("x");
#endif
      drop_count --;
      return 0;
    }
    else if (packet_count < tswriter->drop_packets)
    {
#if 0
      if (packet_count == 0) print_msg("\n");
      print_msg(".");
#endif
      packet_count ++;
    }
    else
    {
#if 0
      print_msg("X");
#endif
      packet_count = 0;
      drop_count = tswriter->drop_number - 1;
      return 0;
    }
  }

  if (tswriter->writer == NULL)
  {
    // We're writing directly
    switch (tswriter->how)
    {
    case TS_W_STDOUT:
    case TS_W_FILE:
      err = write_file_data(tswriter,packet,TS_PACKET_SIZE);
      if (err) return 1;
      break;
    case TS_W_TCP:
      err = write_tcp_data(tswriter,packet,TS_PACKET_SIZE);
      if (err) return err;  // important, because it might be 0, 1 or EOF
      break;
    case TS_W_UDP:
      err = write_socket_data(tswriter->where.socket,packet,TS_PACKET_SIZE);
      if (err) return 1;
      break;
    default:
      fprint_err("### Unexpected writer type %d to tswrite_write()\n",
                 tswriter->how);
      return 1;
    }
    (tswriter->count)++;
  }
  else
  {
    // We're writing via buffered output
    err = write_to_buffered_TS_output(tswriter->writer,packet,
                                      (tswriter->count)++,
                                      pid,got_pcr,pcr);
    if (err) return 1;
  }
  return 0;
}

/*
 * Discontinuity on the stream being written (e.g. file looping)
 * If we are pacing the output then this resets the timing info
*/

int tswrite_discontinuity(const TS_writer_p  tswriter)
{
  if (tswriter->writer == NULL)
    return 0;

  internal_flush_buffered_TS_output(tswriter->writer);

  discontinuity_buffered_TS_output(tswriter->writer);

  return 0;
}

// ============================================================
// Common option handling - helpers for utility writers
// ============================================================
/*
 * Write a usage string (to standard output) describing the tuning
 * options processed by tswrite_process_args.
 */
extern void tswrite_help_tuning(void)
{
  fprint_msg(
    "Output Tuning:\n"
    "  -bitrate <n>      Try for an initial data rate of <n> bits/second,\n"
    "                    so -bitrate 3000 is 3000 bits/second, i.e., 3kbps\n"
    "  -byterate <n>     Specify the initial data rate in bytes per second,\n"
    "                    instead of bits/second.\n"
    "  -nopcrs           Ignore PCRs when working out the packet times,\n"
    "                    just use the selected bit/byte rate.\n"
    "\n"
    "The data rate is stored internally as bytes/second, so if a -bitrate value\n"
    "is given that is not a multiple of 8, it will be approximated internally.\n"
    "If no initial data rate is specified, an arbitrary default rate of\n"
    "%d bytes/second (%d bits/second) is used. If the input data contains\n"
    "PCRs, this will then be adjusted towards the data rate indicated by\n"
    "the PCRs.\n"
    "\n"
    "  -maxnowait <n>    Specify the maximum number of packets that can be\n"
    "                    sent to the target host with no gap. Sending too\n"
    "                    many packets with no gap can overrun the target's\n"
    "                    buffers. [default: off]\n"
    "  -maxnowait off    Do not enforce any limit on how many packets may be\n"
    "                    sent without any intermediate delay.\n"
    "\n"
    "  -waitfor <n>      The number of microseconds to wait *after* 'maxnowait'\n"
    "                    packets have been sent with no gap. The default is 1000.\n"
    "\n"
    "  -buffer <size>    Use a circular buffer of size <size>+1.\n"
    "                    The default is %d.\n"
    "\n"
    "  -tsinpkt <n>      How many TS packets to put in each circular buffer item\n"
    "                    (i.e., how many TS packets will end up in each UDP packet).\n"
    "                    This defaults to 7, which is the number guaranteed to fit\n"
    "                    into a single ethernet packet. Specifying more than 7 will\n"
    "                    give fragmented packets on 'traditional' networks. Specifying\n"
    "                    less will cause more packets than necessary.\n"
    "\n"
    "When the child process starts up, it waits for the circular buffer to fill\n"
    "up before it starts sending any data.\n"
    "\n"
    "  -prime <n>        Prime the PCR timing mechanism with 'time' for\n"
    "                    <n> circular buffer items. The default is %d\n"
    "  -speedup <n>      Percentage of 'normal speed' to use when\n"
    "                    calculating the priming time.\n"
    "\n"
    "Unless -nopcrs is selected, packet times are calculated using PCRs,\n"
    "as they are found. The program starts with a number of bytes\n"
    "'in hand', and a corresponding time calculated using the default\n"
    "byterate. As data is actually output, the number of bytes output is\n"
    "subtracted from the total 'in hand', and the time remaining amended\n"
    "likewise. When a new PCR is found, the number of bytes and given\n"
    "number of microseconds since the last PCR is added to the 'in hand'\n"
    "totals.\n"
    "\n"
    "The -prime switch can be used to determine how many circular buffer\n"
    "items (i.e., 188*7 byte packets) should be used to prime the number\n"
    "of bytes and time held 'in hand'. Larger numbers will allow the\n"
    "program to cope with longer distances between PCRs, and will also\n"
    "tend to smooth out the byte rates indicated by adjacent PCRs.\n"
    "\n"
    "  -pcr_scale <percentage>    Scale PCR values by this percentage.\n"
    "                             <percentage> is a floating (double) value.\n"
    "\n"
    "If a PCR scale is given, then all PCRs will be multiplied by\n"
    "<percentage>/100. Thus '-pcr_scale 100' will have no effect,\n"
    "'-pcr_scale 200' will double each PCR, and '-pcr_scale 50' will halve\n"
    "each PCR value.\n"
    "\n"
    "  -pwait <n>        The parent process should wait <n>ms when the\n"
    "                    buffer is full before checking again.\n"
    "                    The default is 50ms.\n"
    "  -cwait <n>        The child processs should wait <n>ms when the\n"
    "                    buffer is empty, before checking again.\n"
    "                    The default is 10ms.\n"
    "\n"
    "For convenience, the '-hd' switch is provided for playing HD video:\n"
    "\n"
    "  -hd               equivalent to '-bitrate 20000000 -maxnowait off\n"
    "                                   -pwait 4 -cwait 1'\n"
    "\n"
    "(the exact values may change in future releases of this software).\n"
    "It may also sometimes help to specify '-nopcr' as well (i.e., ignore\n"
    "the timing information in the video stream itself).\n"
    "",
    DEFAULT_BYTE_RATE,
    DEFAULT_BYTE_RATE*8,
    DEFAULT_CIRCULAR_BUFFER_SIZE,
    DEFAULT_PRIME_SIZE);
}

/*
 * Write a usage string (to standard output) describing the testing
 * options processed by tswrite_process_args.
 */
extern void tswrite_help_testing(void)
{
  print_msg(
    "Testing:\n"
    "In order to support some form of automatic 'jitter' in the output,\n"
    "the child process's idea of time can be randomly perturbed:\n"
    "\n"
    "  -perturb <seed> <range> <verbose>\n"
    "\n"
    "<seed> is the initial seed for the random number generator (1 is a\n"
    "traditional default), and <range> is the maximum amount to perturb\n"
    "time by -- this will be used in both the positive and negative\n"
    "directions, and is in milliseconds. <verbose> is either 0 or 1 --\n"
    "if it is 1 then each perturbation time will be reported.\n"
    "It is probably worth selecting a large value for -maxnowait when\n"
    "using -perturb.\n"
    );
}

/*
 * Write a usage string (to standard output) describing the
 * debugging options processed by tswrite_process_args.
 */
extern void tswrite_help_debug(void)
{
  print_msg(
    "Debugging:\n"
    "  -pdebug           Output debugging messages for the parent process\n"
    "  -pdebug2          Output debugging messages for the parent process\n"
    "                    (report on times intermediate between PCRs)\n"
    "  -cdebug           Output debugging messages for the child process\n"
#if DISPLAY_BUFFER
    "  -visual           Output a visual representation of how the\n"
    "                    internal cicular buffer works. It is recommended\n"
    "                    that this is done with small datasets and low\n"
    "                    (e.g., 10) values for the circular buffer size\n"
#endif
    );
}

/*
 * Report on the values within our argument context.
 *
 * Also reports on the various global/debug values.
 */
extern void tswrite_report_args(TS_context_p  context)
{
  fprint_msg("Circular buffer size %d (+1)\n",context->circ_buf_size);
  fprint_msg("Transmitting %s%d TS packet%s (%d bytes) per network"
             " packet/circular buffer item\n",
             context->TS_in_item==1?"":"(up to) ",
             context->TS_in_item,
             context->TS_in_item==1?"":"s",
             context->TS_in_item*TS_PACKET_SIZE);

  if (context->bitrate % 1000000 == 0)
    fprint_msg("Requested data rate is %d Mbps ",context->bitrate/1000000);
  else if (context->bitrate % 1000 == 0)
    fprint_msg("Requested data rate is %d kbps ",context->bitrate/1000);
  else
    fprint_msg("Requested data rate is %d bps ",context->bitrate);
  fprint_msg("(%d bytes/second)\n",context->byterate);
  
  if (context->maxnowait == -1)
    print_msg("Maximum number of packets to send with no wait: No limit\n");
  else
  {
    fprint_msg("Maximum number of packets to send with no wait: %d\n",
               context->maxnowait);
    fprint_msg("Number of microseconds to wait thereafter: %d\n",
               context->waitfor);
  }
  
  if (context->pcr_mode != TSWRITE_PCR_MODE_NONE)
  {
    fprint_msg("PCR mechanism 'primed' with time for %d circular buffer items\n",
               context->prime_size);
    if (context->prime_speedup != 100)
      fprint_msg("PCR mechanism 'prime speedup' is %d%%\n",
                 context->prime_speedup);
  }
  else
    print_msg("Using requested data rate directly to time packets"
              " (ignoring any PCRs)\n");

  if (context->pcr_scale)
    fprint_msg("Multiply PCRs by %g\n",context->pcr_scale);

  if (global_parent_wait != DEFAULT_PARENT_WAIT)
    fprint_msg("Parent will wait %dms for buffer to unfill\n",
               global_parent_wait);
  if (global_child_wait != DEFAULT_CHILD_WAIT)
    fprint_msg("Child will wait %dms for buffer to unempty\n",
               global_child_wait);

  if (global_perturb_range)
  {
    fprint_msg("Randomly perturbing child time by -%u..%ums"
               " with seed %u\n",global_perturb_range,global_perturb_range,
               global_perturb_seed);
  }
}

/*
 * Various command line switches that are useful for tswrite are really
 * only interpretable by tswrite itself. Thus we provide a function that
 * will process such switches.
 *
 * This function extracts appropriate switches from `argv`, and returns it
 * altered appropriately.
 *
 * - `prefix` is a prefix for any error messages - typically the
 *   short name of the program running.
 * - `argc` and `argv` are as passed to `main`. After
 *   this function has finished, any arguments that it has processed will have
 *   had their `argv` array elements changed to point to the string
 *   "<processed>" (this is defined as the string TSWRITE_PROCESSED in the
 *   tswrite.h header file).
 * - values are set in `context` to indicate the user's requests,
 *   and also any appropriate defaults.
 *
 * Note that `tswrite_print_usage` may be used to print out a description of
 * the switches processed by this function.
 *
 * Returns 0 if all goes well, 1 if there was an error. Note that not
 * specifying an output file or host counts as an error.
 */
extern int tswrite_process_args(char           *prefix,
                                int             argc,
                                char           *argv[],
                                TS_context_p    context)
{
  int    err = 0;
  int    ii = 1;

  context->circ_buf_size = DEFAULT_CIRCULAR_BUFFER_SIZE;
  context->TS_in_item    = DEFAULT_TS_PACKETS_IN_ITEM;
  context->maxnowait     = -1;
  context->waitfor       = 1000;
  context->byterate      = DEFAULT_BYTE_RATE;
  context->bitrate       = context->byterate * 8;
  context->pcr_mode      = TSWRITE_PCR_MODE_PCR2;
  context->prime_size    = DEFAULT_PRIME_SIZE;
  context->prime_speedup = 100;
  context->pcr_scale     = 1.0;
  context->pkt_hdr_type  = PKT_HDR_TYPE_NONE;

  while (ii < argc)
  {
    if (!strcmp("-nopcrs",argv[ii]))
    {
      context->pcr_mode = TSWRITE_PCR_MODE_NONE;
      argv[ii] = TSWRITE_PROCESSED;
    }
    else if (!strcmp("-bitrate",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->bitrate);
      if (err) return 1;
      context->byterate = context->bitrate / 8;
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-byterate",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->byterate);
      if (err) return 1;
      context->bitrate = context->byterate * 8;
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-prime",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->prime_size);
      if (err) return 1;
      if (context->prime_size < 1)
      {
        fprint_err("### %s: -prime 0 does not make sense\n",prefix);
        return 1;
      }
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-speedup",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->prime_speedup);
      if (err) return 1;
      if (context->prime_speedup < 1)
      {
        fprint_err("### %s: -speedup 0 does not make sense\n",prefix);
        return 1;
      }
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-pcr_scale",argv[ii]))
    {
      double    percentage;
      CHECKARG(prefix,ii);
      err = double_value(prefix,argv[ii],argv[ii+1],TRUE,&percentage);
      if (err) return 1;
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
      context->pcr_scale = percentage / 100.0;
      fprint_msg("PCR accelerator = %g%% = PCR * %g\n",percentage,context->pcr_scale);
    }
    else if (!strcmp("-maxnowait",argv[ii]))
    {
      CHECKARG(prefix,ii);
      if (!strcmp(argv[ii+1],"off"))
        context->maxnowait = -1;
      else
      {
        err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                        &context->maxnowait);
        if (err) return 1;
      }
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-waitfor",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->waitfor);
      if (err) return 1;
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-buffer",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->circ_buf_size);
      if (err) return 1;
      if (context->circ_buf_size < 1)
      {
        fprint_err("### %s: -buffer 0 does not make sense\n",prefix);
        return 1;
      }
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-tsinpkt",argv[ii]))
    {
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,
                      &context->TS_in_item);
      if (err) return 1;
      if (context->TS_in_item < 1)
      {
        fprint_err("### %s: -tsinpkt 0 does not make sense\n",prefix);
        return 1;
      }
      else if (context->TS_in_item > MAX_TS_PACKETS_IN_ITEM)
      {
        fprint_err("### %s: -tsinpkt %d is too many (maximum is %d)\n",
                   prefix,context->TS_in_item,MAX_TS_PACKETS_IN_ITEM);
        return 1;
      }
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-rtp", argv[ii]))
    {
      context->pkt_hdr_type = PKT_HDR_TYPE_RTP;
      argv[ii] = TSWRITE_PROCESSED;
    }
    else if (!strcmp("-hd", argv[ii]))
    {
      context->maxnowait = 40;
      context->bitrate   = 20000000;
      context->byterate  = context->bitrate / 8;
      global_parent_wait = 4;
      global_child_wait  = 1;
      argv[ii] = TSWRITE_PROCESSED;
    }
    else if (!strcmp("-cdebug",argv[ii]))
    {
      global_child_debug = TRUE;
      argv[ii] = TSWRITE_PROCESSED;
    }
    else if (!strcmp("-pdebug",argv[ii]))
    {
      global_parent_debug = TRUE;
      argv[ii] = TSWRITE_PROCESSED;
    }
    else if (!strcmp("-pdebug2",argv[ii]))
    {
      global_parent_debug = TRUE;
      global_show_all_times = TRUE;
      argv[ii] = TSWRITE_PROCESSED;
    }
    else if (!strcmp("-pwait",argv[ii]))
    {
      int temp;
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,&temp);
      if (err) return 1;
      if (temp == 0)
      {
        fprint_err("### %s: -pwait 0 does not make sense\n",prefix);
        return 1;
      }
      if (temp > 999)
      {
        fprint_err("### %s: -pwait %d (more than 999) not allowed\n",
                   prefix,temp);
        return 1;
      }
      global_parent_wait = temp;
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-cwait",argv[ii]))
    {
      int temp;
      CHECKARG(prefix,ii);
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,&temp);
      if (err) return 1;
      if (temp == 0)
      {
        fprint_err("### %s: -cwait 0 does not make sense\n",prefix);
        return 1;
      }
      if (temp > 999)
      {
        fprint_err("### %s: -cwait %d (more than 999) not allowed\n",
                   prefix,temp);
        return 1;
      }
      global_child_wait = temp;
      argv[ii] = argv[ii+1] = TSWRITE_PROCESSED;
      ii++;
    }
    else if (!strcmp("-perturb",argv[ii]))
    {
      int temp;
      if (ii+3 >= argc)
      {
        fprint_err("### %s: -perturb should have three arguments: "
                   "<seed> <range> <verbose>\n",prefix);
        return 1;
      }
      err = int_value(prefix,argv[ii],argv[ii+1],TRUE,10,&temp);
      if (err) return 1;
      global_perturb_seed = temp;
      err = int_value(prefix,argv[ii],argv[ii+2],TRUE,10,&temp);
      if (err) return 1;
      if (temp == 0)
      {
        fprint_err("### %s: a range of 0 for -perturb does not make sense\n",prefix);
        return 1;
      }
      global_perturb_range = temp;
      if (strlen(argv[ii+3]) != 1)
      {
        fprint_err("### %s: the <verbose> flag for -perturb must be 0 or 1,"
                   " not '%s'\n",prefix,argv[ii+3]);
        return 1;
      }
      switch (argv[ii+3][0])
      {
      case '0':
        global_perturb_verbose = FALSE;
        break;
      case '1':
        global_perturb_verbose = TRUE;
        break;
      default:
        fprint_err("### %s: the <verbose> flag for -perturb must be 0 or 1,"
                   "not '%c'\n",prefix,argv[ii+3][0]);
        return 1;
      }
      argv[ii] = argv[ii+1] = argv[ii+2] = argv[ii+3] = TSWRITE_PROCESSED;
      ii+=3;
    }
#if DISPLAY_BUFFER
    else if (!strcmp("-visual",argv[ii]))
    {
      global_show_circular = TRUE;
      argv[ii] = TSWRITE_PROCESSED;
    }
#endif
    ii++;
  }
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

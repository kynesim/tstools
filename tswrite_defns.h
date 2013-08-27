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

#ifndef _tswrite_defns
#define _tswrite_defns

#include "compat.h"
#include "ts_defns.h"
#include "h222_defns.h"

#ifdef _WIN32
#include <winsock2.h>  // for definition of SOCKET
#else
typedef int SOCKET;    // for compatibility with Windows
#include <termios.h>   // for struct termios
#endif


struct buffered_TS_output;
typedef struct buffered_TS_output *buffered_TS_output_p;
#define SIZEOF_BUFFERED_TS_OUTPUT sizeof(struct buffered_TS_output)

// ============================================================
// EXTERNAL DATASTRUCTURES - these are *intended* for external use
// ============================================================

// Our supported target types
// On Unix-type systems, there is little distinction between file and
// socket, but on Windows this becomes more interesting
enum TS_writer_type
{
  TS_W_UNDEFINED,
  TS_W_STDOUT,  // standard output
  TS_W_FILE,    // a file
  TS_W_TCP,     // a socket, over TCP/IP
  TS_W_UDP,     // a socket, over UDP
};
typedef enum TS_writer_type TS_WRITER_TYPE;

// ------------------------------------------------------------
// So, *is* it a file or a socket?
union TS_writer_output
{
  FILE   *file;
  SOCKET  socket;
};

// ------------------------------------------------------------
// A datastructure to allow us to write to various different types of target
//
// When writing to a file, "how" will be TS_W_STDOUT or TS_W_FILE, and
// "where" will be the appropriate file interface. "writer" is not necessary
// (there's no point in putting a circular buffer and other stuff above
// the file writes), and no child process is needed.
//
// When writing over UDP, "how" will be TS_W_UDP, and "where" will be the
// socket that is being written to. For UDP, timing needs to be managed, and
// thus the circular buffer support is necessary, so "writer" should be
// set to a buffered output context. Since the circular buffer is being
// used, there will also be a child process.
//
// When writing over TCP/IP, "how" will be TS_W_TCP, and "where" will be the
// socket that is being written to. Timing is not an issue, so "writer" will
// not be needed, and nor will there be a child process.  However, it is
// possible that we will want to respond to commands (over the same or another
// socket (or, on Linux/BSD, file descriptor)), so "commander" may be set.
struct TS_writer
{
  enum  TS_writer_type   how;    // what type of output we want
  union TS_writer_output where;  // where it's going to
  buffered_TS_output_p   writer; // our buffered output interface, if needed
  int                    count;  // a count of how many TS packets written

  // Support for the child fork/thread, which actually does the writing when
  // buffered output is enabled.
#ifdef _WIN32
  HANDLE                 child;  // the handle for the child thread (if any)
#else  // _WIN32
  pid_t                  child;  // the PID of the child process (if any)
#endif // _WIN32
  int                    quiet;  // Should the child be as quiet as possible?

  // Support for "commands" being sent to us via a socket (or, on Linux/BSD,
  // from any other file descriptor). The "normal" way this is used is for
  // our application (tsserve) to act as a server, listening on a socket
  // for an incoming connection, and then both playing data to that
  // connection, and listening for commands from it.
  int                    server;         // are we acting as a server?
  SOCKET                 command_socket; // where to read commands from/through

  // When the user sends a new command (a different character than is
  // currently in `command`), the underpinnings of tswrite_write() set
  // `command` to that command letter, and `command_changed` to TRUE.
  // Various key functions that write to TS check `command_changed`, and
  // return COMMAND_RETURN_CODE if it is true.
  // Note, however, that it is left up to the top level to *unset*
  // `command_changed` again.
  byte   command;          // A single character "command" for what to do
  int    command_changed;  // Has it changed?
  // Some commands (notably, the "skip" commands) want to be atomic - that
  // is, they should not be interrupted by the user "typing ahead". Since
  // the fast forward and reverse mechanisms (used for skipping as well)
  // call tswrite_command_changed() to tell if there is a new command that
  // should interrup them, we can provide a flag to say "don't do that"...
  int    atomic_command;

  // Should some TS packets be thrown away every <n> packets? This can be
  // useful for debugging other applications
  int    drop_packets;  // 0 to keep all packets, otherwise keep <n> packets
  int    drop_number;   // and then drop this many
};
typedef struct TS_writer *TS_writer_p;
#define SIZEOF_TS_WRITER sizeof(struct TS_writer)

// ------------------------------------------------------------
// Command letters
#define COMMAND_NOT_A_COMMAND '_' // A guaranteed non-command letter

#define COMMAND_QUIT          'q' // quit/exit
#define COMMAND_NORMAL        'n' // normal playing speed
#define COMMAND_PAUSE         'p' // pause until another command
#define COMMAND_FAST          'f' // fast forward
#define COMMAND_FAST_FAST     'F' // faster forward
#define COMMAND_REVERSE       'r' // reverse/rewind
#define COMMAND_FAST_REVERSE  'R' // faster reverse/rewind
#define COMMAND_SKIP_FORWARD       '>'  // aim at 10s
#define COMMAND_SKIP_BACKWARD      '<'  // ditto
#define COMMAND_SKIP_FORWARD_LOTS  ']'  // aim at 100s
#define COMMAND_SKIP_BACKWARD_LOTS '['  // ditto

#define COMMAND_SELECT_FILE_0 '0'
#define COMMAND_SELECT_FILE_1 '1'
#define COMMAND_SELECT_FILE_2 '2'
#define COMMAND_SELECT_FILE_3 '3'
#define COMMAND_SELECT_FILE_4 '4'
#define COMMAND_SELECT_FILE_5 '5'
#define COMMAND_SELECT_FILE_6 '6'
#define COMMAND_SELECT_FILE_7 '7'
#define COMMAND_SELECT_FILE_8 '8'
#define COMMAND_SELECT_FILE_9 '9'

// And a "return code" that means "the command character has changed"
#define COMMAND_RETURN_CODE  -999


typedef enum tswrite_pcr_mode_e {
  TSWRITE_PCR_MODE_NONE,
  TSWRITE_PCR_MODE_PCR1,
  TSWRITE_PCR_MODE_PCR2
} tswrite_pcr_mode;

typedef enum tswrite_pkt_hdr_type_e
{
  PKT_HDR_TYPE_NONE = 0,
  PKT_HDR_TYPE_RTP
} tswrite_pkt_hdr_type_t;


// ------------------------------------------------------------
// Context for use in decoding command line - see `tswrite_process_args()`
struct TS_context
{
  // Values used in setting up buffered output
  int circ_buf_size; // number of buffer entries (+1) for circular buffer
  int TS_in_item;    // number of TS packets in each circular buffer item
  int maxnowait;     // max number of packets to send without waiting
  int waitfor;       // the number of microseconds to wait thereafter
  int bitrate;       // suggested bit rate  (byterate*8) - both are given
  int byterate;      // suggested byte rate (bitrate/8)  - for convenience
  tswrite_pcr_mode pcr_mode;      // use PCRs for timing information?
  int prime_size;    // initial priming size for buffered output
  int prime_speedup; // percentage of normal speed to prime with
  tswrite_pkt_hdr_type_t pkt_hdr_type;
  double pcr_scale;       // multiplier for PCRs -- see buffered_TS_output
};  
typedef struct TS_context *TS_context_p;

// Arguments processed by tswrite_process_args are set to:
#define TSWRITE_PROCESSED "<processed>"

#endif // _tswrite_defns

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

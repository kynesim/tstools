/*
 * Support for printing out to stdout/stderr/elsewhere -- functions to use
 * instead of printf, etc.
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
#ifndef _printing_fns
#define _printing_fns

#include "printing_defns.h"

// ============================================================
// Functions for printing
// ============================================================
/*
 * Prints the given string, as a normal message.
 */
extern void print_msg(const char *text);
/*
 * Prints the given string, as an error message.
 */
extern void print_err(const char *text);
/*
 * Prints the given format text, as a normal message.
 */
extern void fprint_msg(const char *format, ...);
/*
 * Prints the given formatted text, as an error message.
 */
extern void fprint_err(const char *format, ...);
/*
 * Prints the given formatted text, as a normal or error message.
 * If `is_msg`, then as a normal message, else as an error
 */
extern void fprint_msg_or_err(int is_msg, const char *format, ...);
/*
 * Flush the message output
 */
extern void flush_msg(void);

// ============================================================
// Choosing what the printing functions do
// ============================================================
/*
 * Calling this causes errors to go to stderr, and all other output
 * to go to stdout. This is the "traditional" mechanism used by
 * Unices.
 */
extern void redirect_output_stderr(void);
/*
 * Calling this causes all output to go to stdout. This is simpler,
 * and is likely to be more use to most users.
 *
 * This is the default state.
 */
extern void redirect_output_stdout(void);
/*
 * This allows the user to specify a set of functions to use for
 * formatted printing and non-formatted printing of errors and
 * other messages.
 *
 * It is up to the caller to ensure that all of the functions
 * make sense. All four functions must be specified.
 *
 * * `new_print_message_fn` takes a string and prints it out to the "normal"
 *    output stream.
 * * `new_print_error_fn` takes a string and prints it out to the error output
 *    stream.
 * * `new_fprint_message_fn` takes a printf-style format string and the
 *    appropriate arguments, and writes the result out to the "normal" output.
 * * `new_fprint_error_fn` takes a printf-style format string and the
 *    appropriate arguments, and writes the result out to the "error" output.
 * * `new_flush_msg_fn` flushes the "normal" message output.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int redirect_output( void (*new_print_message_fn) (const char *message),
                            void (*new_print_error_fn) (const char *message),
                            void (*new_fprint_message_fn) (const char *format, va_list arg_ptr),
                            void (*new_fprint_error_fn) (const char *format, va_list arg_ptr),
                            void (*new_flush_msg_fn) (void)
                          );

// Just for the moment
extern void test_C_printing(void);
#endif // _printing_fns

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

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

#include <stdio.h>
#include <stdarg.h>

#include "compat.h"
#include "printing_fns.h"

// ============================================================
// Default printing functions
// ============================================================

static int print_message_to_stdout(const char *message)
{
  return (fputs(message,stdout) == 0 ? 0:1);
}
static int print_message_to_stderr(const char *message)
{
  return (fputs(message,stderr) == 0 ? 0:1);
}
static int fprint_message_to_stdout(const char *format, va_list arg_ptr)
{
  return (vfprintf(stdout, format, arg_ptr) < 0 ? 1:0);
}
static int fprint_message_to_stderr(const char *format, va_list arg_ptr)
{
  return (vfprintf(stderr, format, arg_ptr) < 0 ? 1:0);
}

// ============================================================
// Print redirection
// ============================================================

static int (*print_message_fn) (const char *message) = print_message_to_stdout;
static int (*print_error_fn) (const char *message)   = print_message_to_stdout;

static int (*fprint_message_fn) (const char *format, va_list arg_ptr) = fprint_message_to_stdout;
static int (*fprint_error_fn) (const char *format, va_list arg_ptr)   = fprint_message_to_stdout;


// ============================================================
// Functions for printing
// ============================================================
/*
 * Prints the given string, as a normal message.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int print_msg(const char *text)
{
  return print_message_fn(text);
}


/*
 * Prints the given string, as an error message.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int print_err(const char *text)
{
  return print_error_fn(text);
}


/*
 * Prints the given format text, as a normal message.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int fprint_msg(const char *format, ...)
{
  int retval;
  va_list va_arg;
  va_start(va_arg, format); 
  retval = fprint_message_fn(format, va_arg);
  va_end(va_arg);
  return retval;
}


/*
 * Prints the given formatted text, as an error message.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int fprint_err(const char *format, ...)
{
  int retval;
  va_list va_arg;
  va_start(va_arg, format); 
  retval = fprint_error_fn(format, va_arg);
  va_end(va_arg);
  return retval;
}

// ============================================================
// Choosing what the printing functions do
// ============================================================
/*
 * Calling this causes errors to go to stderr, and all other output
 * to go to stdout. This is the "traditional" mechanism used by
 * Unices.
 */
extern void redirect_output_traditional(void)
{
  print_message_fn  = &print_message_to_stdout;
  print_error_fn    = &print_message_to_stderr;
  fprint_message_fn = &fprint_message_to_stdout;
  fprint_error_fn   = &fprint_message_to_stderr;
}


/*
 * Calling this causes all output to go to stdout. This is simpler,
 * and is likely to be more use to most users.
 *
 * This is the default state.
 */
extern void redirect_output_stdout(void)
{
  print_message_fn  = &print_message_to_stdout;
  print_error_fn    = &print_message_to_stdout;
  fprint_message_fn = &fprint_message_to_stdout;
  fprint_error_fn   = &fprint_message_to_stdout;
}


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
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int redirect_output( int (*new_print_message_fn) (const char *message),
                            int (*new_print_error_fn) (const char *message),
                            int (*new_fprint_message_fn) (const char *format, va_list arg_ptr),
                            int (*new_fprint_error_fn) (const char *format, va_list arg_ptr)
                          )
{
  if (new_print_message_fn == NULL || new_print_error_fn == NULL ||
      new_fprint_message_fn == NULL || new_fprint_error_fn == NULL)
    return 1;

  print_message_fn  = new_print_message_fn;
  print_error_fn    = new_print_error_fn;
  fprint_message_fn = new_fprint_message_fn;
  fprint_error_fn   = new_fprint_error_fn;

  return 0;
}


// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

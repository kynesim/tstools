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

#define DEBUG 0

// ============================================================
// Default printing functions
// ============================================================

static void print_message_to_stdout(const char *message)
{
#if DEBUG
  fputs("1>>",stdout);
#endif
  (void) fputs(message,stdout);
}
static void print_message_to_stderr(const char *message)
{
#if DEBUG
  fputs("2>>",stderr);
#endif
  (void) fputs(message,stderr);
}
static void fprint_message_to_stdout(const char *format, va_list arg_ptr)
{
#if DEBUG
  fputs("3>>",stdout);
#endif
  (void) vfprintf(stdout, format, arg_ptr);
}
static void fprint_message_to_stderr(const char *format, va_list arg_ptr)
{
#if DEBUG
  fputs("4>>",stderr);
#endif
  (void) vfprintf(stderr, format, arg_ptr);
}
static void flush_stdout(void)
{
  (void) fflush(stdout);
}

// ============================================================
// Print redirection defaults to all output going to stdout
// ============================================================

struct print_fns
{
  void (*print_message_fn) (const char *message);
  void (*print_error_fn) (const char *message);

  void (*fprint_message_fn) (const char *format, va_list arg_ptr);
  void (*fprint_error_fn) (const char *format, va_list arg_ptr);

  void (*flush_message_fn) (void);
};

static struct print_fns fns = { print_message_to_stdout,
                                print_message_to_stdout,
                                fprint_message_to_stdout,
                                fprint_message_to_stdout,
                                flush_stdout };

#if DEBUG
static void report_fns(const char *why)
{
  printf("Printing bound to (%s) m:%p, e:%p, fm:%p, fe:%p\n",why,
         fns.print_message_fn,
         fns.print_error_fn,
         fns.fprint_message_fn,
         fns.fprint_error_fn);
}
#endif


// ============================================================
// Functions for printing
// ============================================================
/*
 * Prints the given string, as a normal message.
 */
extern void print_msg(const char *text)
{
#if DEBUG
  printf("m:%p %s",fns.print_message_fn,text);
  report_fns("m");
#endif
  fns.print_message_fn(text);
}


/*
 * Prints the given string, as an error message.
 */
extern void print_err(const char *text)
{
#if DEBUG
  printf("e:%p %s",fns.print_error_fn,text);
  report_fns("e");
#endif
  fns.print_error_fn(text);
}


/*
 * Prints the given format text, as a normal message.
 */
extern void fprint_msg(const char *format, ...)
{
  va_list va_arg;
  va_start(va_arg, format); 
#if DEBUG
  printf("fm:%p %s",fns.fprint_message_fn,format);
  report_fns("fm");
#endif
  fns.fprint_message_fn(format, va_arg);
  va_end(va_arg);
}


/*
 * Prints the given formatted text, as an error message.
 */
extern void fprint_err(const char *format, ...)
{
  va_list va_arg;
  va_start(va_arg, format); 
#if DEBUG
  printf("fe:%p %s",fns.fprint_error_fn,format);
  report_fns("fe");
#endif
  fns.fprint_error_fn(format, va_arg);
  va_end(va_arg);
}


/*
 * Prints the given formatted text, as a normal or error message.
 * If `is_msg`, then as a normal message, else as an error
 */
extern void fprint_msg_or_err(int is_msg, const char *format, ...)
{
  va_list va_arg;
  va_start(va_arg, format); 
  if (is_msg)
  {
#if DEBUG
    printf("?m:%p %s",fns.fprint_message_fn,format);
    report_fns("?m");
#endif
    fns.fprint_message_fn(format, va_arg);
  }
  else
  {
#if DEBUG
    printf("?e:%p %s",fns.fprint_error_fn,format);
    report_fns("?e");
#endif
    fns.fprint_error_fn(format, va_arg);
  }
  va_end(va_arg);
}
/*
 * Prints the given string, as a normal message.
 */
extern void flush_msg(void)
{
  fns.flush_message_fn();
}

// ============================================================
// Choosing what the printing functions do
// ============================================================

/*
 * Calling this causes errors to go to stderr, and all other output
 * to go to stdout. This is the "traditional" mechanism used by
 * Unices.
 */
extern void redirect_output_stderr(void)
{
  fns.print_message_fn  = &print_message_to_stdout;
  fns.print_error_fn    = &print_message_to_stderr;
  fns.fprint_message_fn = &fprint_message_to_stdout;
  fns.fprint_error_fn   = &fprint_message_to_stderr;
  fns.flush_message_fn  = &flush_stdout;

#if DEBUG
  report_fns("traditional");
#endif
}


/*
 * Calling this causes all output to go to stdout. This is simpler,
 * and is likely to be more use to most users.
 *
 * This is the default state.
 */
extern void redirect_output_stdout(void)
{
  fns.print_message_fn  = &print_message_to_stdout;
  fns.print_error_fn    = &print_message_to_stdout;
  fns.fprint_message_fn = &fprint_message_to_stdout;
  fns.fprint_error_fn   = &fprint_message_to_stdout;
  fns.flush_message_fn  = &flush_stdout;

#if DEBUG
  report_fns("stdout");
#endif
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
 * * `new_flush_msg_fn` flushes the "normal" message output.
 *
 * Returns 0 if all goes well, 1 if something goes wrong.
 */
extern int redirect_output( void (*new_print_message_fn) (const char *message),
                            void (*new_print_error_fn) (const char *message),
                            void (*new_fprint_message_fn) (const char *format, va_list arg_ptr),
                            void (*new_fprint_error_fn) (const char *format, va_list arg_ptr),
                            void (*new_flush_msg_fn) (void)
                          )
{
  if (new_print_message_fn == NULL || new_print_error_fn == NULL ||
      new_fprint_message_fn == NULL || new_fprint_error_fn == NULL ||
      new_flush_msg_fn == NULL)
    return 1;

  fns.print_message_fn  = new_print_message_fn;
  fns.print_error_fn    = new_print_error_fn;
  fns.fprint_message_fn = new_fprint_message_fn;
  fns.fprint_error_fn   = new_fprint_error_fn;
  fns.flush_message_fn  = new_flush_msg_fn;

#if DEBUG
  report_fns("specific");
#endif

  return 0;
}

extern void test_C_printing(void)
{
  print_msg("C Message\n");
  print_err("C Error\n");
  fprint_msg("C Message %s\n","Fred");
  fprint_err("C Error %s\n","Fred");
}


// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

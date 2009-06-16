/*
 * Test the print redirection facilities
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

#include "printing_fns.h"
#include "version.h"

// Some example redirection routines

static void print_message_to_stdout(const char *message)
{
  (void) printf("<<<MSG>>> %s",message);
}
static void print_message_to_stderr(const char *message)
{
  (void) printf("<<<ERR>>> %s",message);
}
static void fprint_message_to_stdout(const char *format, va_list arg_ptr)
{
  printf("<<<MSG>>> ");
  (void) vfprintf(stdout, format, arg_ptr);
}
static void fprint_message_to_stderr(const char *format, va_list arg_ptr)
{
  printf("<<<ERR>>> ");
  (void) vfprintf(stdout, format, arg_ptr);
}

static void print_usage()
{
  printf(
    "Usage: test_printing\n"
    "\n"
    );
  REPORT_VERSION("test_printing");
  printf(
    "\n"
    "  Test the print redirection facilities.\n"
    );
}

int main(int argc, char **argv)
{
  int err;

  if (argc > 1)
  {
    print_usage();
    return 1;
  }

  printf("A fairly crude set of tests, mainly to check that nothing falls over.\n");
  printf("For each set of tests, you should see 4 messages, all very similar.\n");

  printf("------------------------------------\n");
  printf("Testing the default output functions\n");
  printf("------------------------------------\n");
  print_msg("1. Printing a normal message\n");
  print_err("2. Printing an error message\n");
  fprint_msg("3. Printing a formatted '%s'\n","message");
  fprint_err("4. Printing a formatted '%s'\n","error");

  printf("-------------------------------------------\n");
  printf("Choosing 'traditional' output and repeating\n");
  printf("-------------------------------------------\n");
  redirect_output_stderr();
  print_msg("1. Printing a normal message\n");
  print_err("2. Printing an error message\n");
  fprint_msg("3. Printing a formatted '%s'\n","message");
  fprint_err("4. Printing a formatted '%s'\n","error");

  printf("---------------------------------------------\n");
  printf("Choosing 'all output to stdout' and repeating\n");
  printf("---------------------------------------------\n");
  redirect_output_stdout();
  print_msg("1. Printing a normal message\n");
  print_err("2. Printing an error message\n");
  fprint_msg("3. Printing a formatted '%s'\n","message");
  fprint_err("4. Printing a formatted '%s'\n","error");

  printf("-----------------------------------------\n");
  printf("Choosing 'custom functions' and repeating\n");
  printf("-----------------------------------------\n");
  err = redirect_output(print_message_to_stdout,
                        print_message_to_stderr,
                        fprint_message_to_stdout,
                        fprint_message_to_stderr);
  if (err)
  {
    printf("Oops -- that went wrong: %d\n",err);
    return 1;
  }
  print_msg("1. Printing a normal message\n");
  print_err("2. Printing an error message\n");
  fprint_msg("3. Printing a formatted '%s'\n","message");
  fprint_err("4. Printing a formatted '%s'\n","error");

  printf("---------------------------------------------\n");
  printf("Trying to choose only *some* custom functions\n");
  printf("---------------------------------------------\n");
  err = redirect_output(print_message_to_stdout,
                        print_message_to_stderr,
                        NULL,
                        fprint_message_to_stderr);
  if (err == 0)
  {
    printf("Oh dear, that appeared to work: %d\n",err);
    printf("So what happens if we try our tests again?\n");
    print_msg("1. Printing a normal message\n");
    print_err("2. Printing an error message\n");
    fprint_msg("3. Printing a formatted '%s'\n","message");
    fprint_err("4. Printing a formatted '%s'\n","error");
    return 1;
  }
  printf("Which failed - good\n");

  printf("------------------------------------------------------------------\n");
  printf("After that (expected) failure, all four messages should still work\n");
  printf("------------------------------------------------------------------\n");
  print_msg("1. Printing a normal message\n");
  print_err("2. Printing an error message\n");
  fprint_msg("3. Printing a formatted '%s'\n","message");
  fprint_err("4. Printing a formatted '%s'\n","error");
  
  return 0;
}

// Local Variables:
// tab-width: 8
// indent-tabs-mode: nil
// c-basic-offset: 2
// End:
// vim: set tabstop=8 shiftwidth=2 expandtab:

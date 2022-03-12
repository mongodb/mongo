/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

Copyright (c) 2003 Hewlett-Packard Co.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "compiler.h"

#include <libunwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#define panic(args...)				\
	{ fprintf (stderr, args); exit (-1); }

void * stack_start;

#define PAGE_SIZE 4096

void do_backtrace (void)
{
  /*
    We make the assumption that we are able to rewind far enough
    (steps > 5) before touching the forbidden region in the stack,
    at which point the unwinding should stop gracefully.
  */
  mprotect((void*)((uintptr_t)stack_start & ~(PAGE_SIZE - 1)),
           PAGE_SIZE, PROT_NONE);

  unw_cursor_t cursor;
  unw_word_t ip, sp;
  unw_context_t uc;
  int ret;
  int steps = 0;

  unw_getcontext (&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    panic ("unw_init_local failed!\n");

  do
    {
      unw_get_reg (&cursor, UNW_REG_IP, &ip);
      unw_get_reg (&cursor, UNW_REG_SP, &sp);

      ret = unw_step (&cursor);
	  printf("ip=%lx, sp=%lx -> %d\n", ip, sp, ret);
      if (ret < 0)
	{
	  unw_get_reg (&cursor, UNW_REG_IP, &ip);
	}
      steps ++;
    }
  while (ret > 0);

  if (steps < 5)
    {
      printf("not enough steps: %d, need 5\n", steps);
      exit(-1);
    }
  printf("success, steps: %d\n", steps);

  mprotect((void*)((uintptr_t)stack_start & ~(PAGE_SIZE - 1)),
           PAGE_SIZE, PROT_READ|PROT_WRITE);
}

void NOINLINE consume_and_run (int depth)
{
  unw_cursor_t cursor;
  unw_context_t uc;
  char string[1024];

  sprintf (string, "hello %p %p\n", &cursor, &uc);
  if (depth == 0) {
    do_backtrace();
  } else {
    consume_and_run(depth - 1);
  }
}

int
main (int argc, char **argv UNUSED)
{
  int start;
  unw_context_t uc;
  unw_cursor_t cursor;

  stack_start = &start;

  /*
    We need to make the frame at least the size protected by
    the mprotect call so we are not forbidding access to
    unrelated regions.
  */
  char string[PAGE_SIZE];
  sprintf (string, "hello\n");

  // Initialize pipe mem validate check, opens file descriptors
  unw_getcontext(&uc);
  if (unw_init_local (&cursor, &uc) < 0)
    panic ("unw_init_local failed!\n");

  int i;
  for (i = 3; i < 10; i++)
    {

      pid_t childpid = fork();
      if (!childpid)
        {
          /* Close fds and make sure we still work */
          close(i);
        }

      int status;
      if (childpid)
        {
          wait(&status);
          if (WIFEXITED(status))
              return WEXITSTATUS(status);
          else
            return -1;
        }
      else
        {
          consume_and_run (10);

          return 0;
        }
    }

  return 0;
}

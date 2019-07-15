/* libunwind - a platform-independent unwind library
   Copyright (C) 2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

int
main (int argc, char **argv, char **envp)
{
  char *program, **child_argv;
  struct timeval start, stop;
  double secs;
  int status, i;
  long count;
  pid_t pid;

  count = atol (argv[1]);
  program = argv[2];

  child_argv = alloca ((argc - 1) * sizeof (char *));
  for (i = 0; i < argc - 2; ++i)
    child_argv[i] = argv[2 + i];
  child_argv[i] = NULL;

  gettimeofday (&start, NULL);
  for (i = 0; i < count; ++i)
    {
      pid = fork ();
      if (pid == 0)
        {
          execve (program, child_argv, envp);
          _exit (-1);
        }
      else
        {
          waitpid (pid, &status, 0);
          if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
            {
              fprintf (stderr, "%s: child failed\n", argv[0]);
              exit (-1);
            }
        }
    }
  gettimeofday (&stop, NULL);

  secs = ((stop.tv_sec + 1e-6 * stop.tv_usec)
	  - (start.tv_sec + 1e-6 * start.tv_usec));
  printf ("%lu nsec/execution\n",
	  (unsigned long) (1e9 * secs / (double) count));
  return 0;
}

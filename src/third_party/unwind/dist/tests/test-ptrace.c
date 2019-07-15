/* libunwind - a platform-independent unwind library
   Copyright (C) 2003-2004 Hewlett-Packard Co
	Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

This file is part of libunwind.

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

#include <config.h>

#ifdef HAVE_TTRACE

int
main (void)
{
  printf ("FAILURE: ttrace() not supported yet\n");
  return -1;
}

#else /* !HAVE_TTRACE */

#include <errno.h>
#include <fcntl.h>
#include <libunwind-ptrace.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ptrace.h>
#include <sys/wait.h>

extern char **environ;

static const int nerrors_max = 100;

int nerrors;
int verbose;
int print_names = 1;

enum
  {
    INSTRUCTION,
    SYSCALL,
    TRIGGER
  }
trace_mode = SYSCALL;

#define panic(args...)						\
	do { fprintf (stderr, args); ++nerrors; } while (0)

static unw_addr_space_t as;
static struct UPT_info *ui;

static int killed;

void
do_backtrace (void)
{
  unw_word_t ip, sp, start_ip = 0, off;
  int n = 0, ret;
  unw_proc_info_t pi;
  unw_cursor_t c;
  char buf[512];
  size_t len;

  ret = unw_init_remote (&c, as, ui);
  if (ret < 0)
    panic ("unw_init_remote() failed: ret=%d\n", ret);

  do
    {
      if ((ret = unw_get_reg (&c, UNW_REG_IP, &ip)) < 0
	  || (ret = unw_get_reg (&c, UNW_REG_SP, &sp)) < 0)
	panic ("unw_get_reg/unw_get_proc_name() failed: ret=%d\n", ret);

      if (n == 0)
	start_ip = ip;

      buf[0] = '\0';
      if (print_names)
	unw_get_proc_name (&c, buf, sizeof (buf), &off);

      if (verbose)
	{
	  if (off)
	    {
	      len = strlen (buf);
	      if (len >= sizeof (buf) - 32)
		len = sizeof (buf) - 32;
	      sprintf (buf + len, "+0x%lx", (unsigned long) off);
	    }
	  printf ("%016lx %-32s (sp=%016lx)\n", (long) ip, buf, (long) sp);
	}

      if ((ret = unw_get_proc_info (&c, &pi)) < 0)
	panic ("unw_get_proc_info(ip=0x%lx) failed: ret=%d\n", (long) ip, ret);
      else if (verbose)
	printf ("\tproc=%016lx-%016lx\n\thandler=%lx lsda=%lx",
		(long) pi.start_ip, (long) pi.end_ip,
		(long) pi.handler, (long) pi.lsda);

#if UNW_TARGET_IA64
      {
	unw_word_t bsp;

	if ((ret = unw_get_reg (&c, UNW_IA64_BSP, &bsp)) < 0)
	  panic ("unw_get_reg() failed: ret=%d\n", ret);
	else if (verbose)
	  printf (" bsp=%lx", bsp);
      }
#endif
      if (verbose)
	printf ("\n");

      ret = unw_step (&c);
      if (ret < 0)
	{
	  unw_get_reg (&c, UNW_REG_IP, &ip);
	  panic ("FAILURE: unw_step() returned %d for ip=%lx (start ip=%lx)\n",
		 ret, (long) ip, (long) start_ip);
	}

      if (++n > 64)
	{
	  /* guard against bad unwind info in old libraries... */
	  panic ("too deeply nested---assuming bogus unwind (start ip=%lx)\n",
		 (long) start_ip);
	  break;
	}
      if (nerrors > nerrors_max)
        {
	  panic ("Too many errors (%d)!\n", nerrors);
	  break;
	}
    }
  while (ret > 0);

  if (ret < 0)
    panic ("unwind failed with ret=%d\n", ret);

  if (verbose)
    printf ("================\n\n");
}

static pid_t target_pid;
static void target_pid_kill (void)
{
  kill (target_pid, SIGKILL);
}

int
main (int argc, char **argv)
{
  int status, pid, pending_sig, optind = 1, state = 1;

  as = unw_create_addr_space (&_UPT_accessors, 0);
  if (!as)
    panic ("unw_create_addr_space() failed");

  if (argc == 1)
    {
      static char *args[] = { "self", "/bin/ls", "/usr", NULL };

      /* automated test case */
      argv = args;

      /* Unless the args array is 'walked' the child
         process is unable to access it and dies with a segfault */
      fprintf(stderr, "Automated test (%s,%s,%s,%s)\n",
              args[0],args[1],args[2],args[3]);
    }
  else if (argc > 1)
    while (argv[optind][0] == '-')
      {
	if (strcmp (argv[optind], "-v") == 0)
	  ++optind, verbose = 1;
	else if (strcmp (argv[optind], "-i") == 0)
	  ++optind, trace_mode = INSTRUCTION;	/* backtrace at each insn */
	else if (strcmp (argv[optind], "-s") == 0)
	  ++optind, trace_mode = SYSCALL;	/* backtrace at each syscall */
	else if (strcmp (argv[optind], "-t") == 0)
	  /* Execute until raise(SIGUSR1), then backtrace at each insn
	     until raise(SIGUSR2).  */
	  ++optind, trace_mode = TRIGGER;
	else if (strcmp (argv[optind], "-c") == 0)
	  /* Enable caching of unwind-info.  */
	  ++optind, unw_set_caching_policy (as, UNW_CACHE_GLOBAL);
	else if (strcmp (argv[optind], "-n") == 0)
	  /* Don't look-up and print symbol names.  */
	  ++optind, print_names = 0;
	else
	  fprintf(stderr, "unrecognized option: %s\n", argv[optind++]);
        if (optind >= argc)
          break;
      }

  target_pid = fork ();
  if (!target_pid)
    {
      /* child */

      if (!verbose)
	dup2 (open ("/dev/null", O_WRONLY), 1);

#if HAVE_DECL_PTRACE_TRACEME
      ptrace (PTRACE_TRACEME, 0, 0, 0);
#elif HAVE_DECL_PT_TRACE_ME
      ptrace (PT_TRACE_ME, 0, 0, 0);
#else
#error Trace me
#endif

      if ((argc > 1) && (optind == argc)) {
        fprintf(stderr, "Need to specify a command line for the child\n");
        exit (-1);
      }
      execve (argv[optind], argv + optind, environ);
      _exit (-1);
    }
  atexit (target_pid_kill);

  ui = _UPT_create (target_pid);

  while (nerrors <= nerrors_max)
    {
      pid = wait4 (-1, &status, 0, NULL);
      if (pid == -1)
	{
	  if (errno == EINTR)
	    continue;

	  panic ("wait4() failed (errno=%d)\n", errno);
	}
      pending_sig = 0;
      if (WIFSIGNALED (status) || WIFEXITED (status)
	  || (WIFSTOPPED (status) && WSTOPSIG (status) != SIGTRAP))
	{
	  if (WIFEXITED (status))
	    {
	      if (WEXITSTATUS (status) != 0)
		panic ("child's exit status %d\n", WEXITSTATUS (status));
	      break;
	    }
	  else if (WIFSIGNALED (status))
	    {
	      if (!killed)
		panic ("child terminated by signal %d\n", WTERMSIG (status));
	      break;
	    }
	  else
	    {
	      pending_sig = WSTOPSIG (status);
	      /* Avoid deadlock:  */
	      if (WSTOPSIG (status) == SIGKILL)
	        break;
	      if (trace_mode == TRIGGER)
		{
		  if (WSTOPSIG (status) == SIGUSR1)
		    state = 0;
		  else if  (WSTOPSIG (status) == SIGUSR2)
		    state = 1;
		}
	      if (WSTOPSIG (status) != SIGUSR1 && WSTOPSIG (status) != SIGUSR2)
	        {
		  static int count = 0;

		  if (count++ > 100)
		    {
		      panic ("Too many child unexpected signals (now %d)\n",
			     WSTOPSIG (status));
			killed = 1;
		    }
	        }
	    }
	}

      switch (trace_mode)
	{
	case TRIGGER:
	  if (state)
#if HAVE_DECL_PTRACE_CONT
	    ptrace (PTRACE_CONT, target_pid, 0, 0);
#elif HAVE_DECL_PT_CONTINUE
	    ptrace (PT_CONTINUE, target_pid, (caddr_t)1, 0);
#else
#error Port me
#endif
	  else
	    {
	      do_backtrace ();
#if HAVE_DECL_PTRACE_SINGLESTEP
	      if (ptrace (PTRACE_SINGLESTEP, target_pid, 0, pending_sig) < 0)
          {
            panic ("ptrace(PTRACE_SINGLESTEP) failed (errno=%d)\n", errno);
            killed = 1;
          }
#elif HAVE_DECL_PT_STEP
	      if (ptrace (PT_STEP, target_pid, (caddr_t)1, pending_sig) < 0)
          {
            panic ("ptrace(PT_STEP) failed (errno=%d)\n", errno);
            killed = 1;
          }
#else
#error Singlestep me
#endif
	    }
	  break;

	case SYSCALL:
	  if (!state)
	    do_backtrace ();
	  state ^= 1;
#if HAVE_DECL_PTRACE_SYSCALL
	  ptrace (PTRACE_SYSCALL, target_pid, 0, pending_sig);
#elif HAVE_DECL_PT_SYSCALL
	  ptrace (PT_SYSCALL, target_pid, (caddr_t)1, pending_sig);
#else
#error Syscall me
#endif
	  break;

	case INSTRUCTION:
	  do_backtrace ();
#if HAVE_DECL_PTRACE_SINGLESTEP
	      ptrace (PTRACE_SINGLESTEP, target_pid, 0, pending_sig);
#elif HAVE_DECL_PT_STEP
	      ptrace (PT_STEP, target_pid, (caddr_t)1, pending_sig);
#else
#error Singlestep me
#endif
	  break;
	}
      if (killed)
        kill (target_pid, SIGKILL);
    }

  _UPT_destroy (ui);
  unw_destroy_addr_space (as);

  if (nerrors)
    {
      printf ("FAILURE: detected %d errors\n", nerrors);
      exit (-1);
    }
  if (verbose)
    printf ("SUCCESS\n");

  return 0;
}

#endif /* !HAVE_TTRACE */

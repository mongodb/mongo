#include "flush-cache.h"

#include <assert.h>
#include <libunwind.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>

int verbose;

#ifdef __ia64__
# define GET_ENTRY(fdesc)	(((uintptr_t *) (fdesc))[0])
# define GET_GP(fdesc)		(((uintptr_t *) (fdesc))[0])
# define EXTRA			16
#else
# define GET_ENTRY(fdesc)	((uintptr_t ) (fdesc))
# define GET_GP(fdesc)		(0)
# define EXTRA			0
#endif

int
make_executable (void *addr, size_t len)
{
  if (mprotect ((void *) (((long) addr) & -getpagesize ()), len,
		PROT_READ | PROT_WRITE | PROT_EXEC) < 0)
    {
      perror ("mprotect");
      return -1;
    }
  flush_cache (addr, len);
  return 0;
}

void *
create_func (unw_dyn_info_t *di, const char *name, long (*func) (),
	     void *end, unw_dyn_region_info_t *region)
{
  void *mem, *memend, *addr, *fptr;
  unw_word_t gp = 0;
  size_t len;

  len = (uintptr_t) end - GET_ENTRY (func) + EXTRA;
  mem = malloc (len);
  if (verbose)
    printf ("%s: cloning %s at %p (%zu bytes)\n",
	    __FUNCTION__, name, mem, len);
  memend = (char *) mem + len;

#ifdef __ia64__
  addr = (void *) GET_ENTRY (func);

  /* build function descriptor: */
  ((long *) mem)[0] = (long) mem + 16;		/* entry point */
  ((long *) mem)[1] = GET_GP (func);		/* global-pointer */
  fptr = mem;
  mem = (void *) ((long) mem + 16);
#else
  fptr = mem;
#endif

  len = (char *) memend - (char *) mem;
  memcpy (mem, addr, len);

  if (make_executable (mem, len) < 0)
    return NULL;

  if (di)
    {
      memset (di, 0, sizeof (*di));
      di->start_ip = (unw_word_t) mem;
      di->end_ip = (unw_word_t) memend;
      di->gp = gp;
      di->format = UNW_INFO_FORMAT_DYNAMIC;
      di->u.pi.name_ptr = (unw_word_t) name;
      di->u.pi.regions = region;
    }
  return fptr;
}

int
main (int argc, char **argv)
{
  extern long func_add1 (long);
  extern char func_add1_end[];
  extern long func_add3 (long, long (*[])());
  extern char func_add3_end[];
  extern long func_vframe (long);
  extern char func_vframe_end[];
  unw_dyn_region_info_t *r_pro, *r_epi, *r, *rtmp;
  unw_dyn_info_t di0, di1, di2, di3;
  long (*add1) (long);
  long (*add3_0) (long);
  long (*add3_1) (long, void *[]);
  long (*vframe) (long);
  void *flist[2];
  long ret;
  int i;

  signal (SIGUSR1, SIG_IGN);
  signal (SIGUSR2, SIG_IGN);

  if (argc != 1)
    verbose = 1;

  add1 = (long (*)(long))
	  create_func (&di0, "func_add1", func_add1, func_add1_end, NULL);

  /* Describe the epilogue of func_add3: */
  i = 0;
  r_epi = alloca (_U_dyn_region_info_size (5));
  r_epi->op_count = 5;
  r_epi->next = NULL;
  r_epi->insn_count = -9;
  _U_dyn_op_pop_frames (&r_epi->op[i++],
			_U_QP_TRUE, /* when=*/ 5, /* num_frames=*/ 1);
  _U_dyn_op_stop (&r_epi->op[i++]);
  assert ((unsigned) i <= r_epi->op_count);

  /* Describe the prologue of func_add3: */
  i = 0;
  r_pro = alloca (_U_dyn_region_info_size (4));
  r_pro->op_count = 4;
  r_pro->next = r_epi;
  r_pro->insn_count = 8;
  _U_dyn_op_save_reg (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 0,
		      /* reg=*/ UNW_IA64_AR_PFS, /* dst=*/ UNW_IA64_GR + 34);
  _U_dyn_op_add (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 2,
		 /* reg= */ UNW_IA64_SP, /* val=*/ -16);
  _U_dyn_op_save_reg (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 4,
		      /* reg=*/ UNW_IA64_RP, /* dst=*/ UNW_IA64_GR + 3);
  _U_dyn_op_spill_sp_rel (&r_pro->op[i++], _U_QP_TRUE, /* when=*/ 7,
		      /* reg=*/ UNW_IA64_RP, /* off=*/ 16);
  assert ((unsigned) i <= r_pro->op_count);

  /* Create regions for func_vframe: */
  i = 0;
  r = alloca (_U_dyn_region_info_size (16));
  r->op_count = 16;
  r->next = NULL;
  r->insn_count = 4;
  _U_dyn_op_label_state (&r->op[i++], /* label=*/ 100402);
  _U_dyn_op_pop_frames (&r->op[i++], _U_QP_TRUE, /* when=*/ 3, /* frames=*/ 1);
  _U_dyn_op_stop (&r->op[i++]);
  assert ((unsigned) i <= r->op_count);

  i = 0;
  rtmp = r;
  r = alloca (_U_dyn_region_info_size (16));
  r->op_count = 16;
  r->next = rtmp;
  r->insn_count = 16;
  _U_dyn_op_save_reg (&r->op[i++], _U_QP_TRUE, /* when=*/ 8,
		      /* reg=*/ UNW_IA64_RP, /* dst=*/ UNW_IA64_GR + 3);
  _U_dyn_op_pop_frames (&r->op[i++], _U_QP_TRUE, /* when=*/ 10,
			/* num_frames=*/ 1);
  _U_dyn_op_stop (&r->op[i++]);
  assert ((unsigned) i <= r->op_count);

  i = 0;
  rtmp = r;
  r = alloca (_U_dyn_region_info_size (16));
  r->op_count = 16;
  r->next = rtmp;
  r->insn_count = 5;
  _U_dyn_op_save_reg (&r->op[i++], _U_QP_TRUE, /* when=*/ 1,
			  /* reg=*/ UNW_IA64_RP, /* dst=*/ UNW_IA64_GR + 33);
  _U_dyn_op_save_reg (&r->op[i++], _U_QP_TRUE, /* when=*/ 2,
			  /* reg=*/ UNW_IA64_SP, /* dst=*/ UNW_IA64_GR + 34);
  _U_dyn_op_spill_fp_rel (&r->op[i++], _U_QP_TRUE, /* when=*/ 4,
			  /* reg=*/ UNW_IA64_AR_PFS, /* off=*/ 16);
  _U_dyn_op_label_state (&r->op[i++], /* label=*/ 100402);
  _U_dyn_op_stop (&r->op[i++]);
  assert ((unsigned) i <= r->op_count);

  /* Create two functions which can share the region-list:  */
  add3_0 = (long (*) (long))
	  create_func (&di1, "func_add3/0", func_add3, func_add3_end, r_pro);
  add3_1 = (long (*) (long, void *[]))
	  create_func (&di2, "func_add3/1", func_add3, func_add3_end, r_pro);
  vframe = (long (*) (long))
	  create_func (&di3, "func_vframe", func_vframe, func_vframe_end, r);

  _U_dyn_register (&di1);
  _U_dyn_register (&di2);
  _U_dyn_register (&di3);
  _U_dyn_register (&di0);

  flist[0] = add3_0;
  flist[1] = add1;

  kill (getpid (), SIGUSR1);	/* do something ptmon can latch onto */
  ret = (*add3_1) (13, flist);
  if (ret != 18)
    {
      fprintf (stderr, "FAILURE: (*add3_1)(13) returned %ld\n", ret);
      exit (-1);
    }

  ret = (*vframe) (48);
  if (ret != 4)
    {
      fprintf (stderr, "FAILURE: (*vframe)(16) returned %ld\n", ret);
      exit (-1);
    }
  ret = (*vframe) (64);
  if (ret != 10)
    {
      fprintf (stderr, "FAILURE: (*vframe)(32) returned %ld\n", ret);
      exit (-1);
    }
  kill (getpid (), SIGUSR2);	/* do something ptmon can latch onto */

  _U_dyn_cancel (&di0);
  _U_dyn_cancel (&di1);
  _U_dyn_cancel (&di3);
  _U_dyn_cancel (&di2);

  return 0;
}

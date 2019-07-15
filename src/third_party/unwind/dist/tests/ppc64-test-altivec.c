

#include <libunwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <altivec.h>

#include <sys/resource.h>

#define panic(args...)	{ fprintf (stderr, args);  abort(); }

extern vector signed int vec_init ();
extern void vec_print (vector signed int v);

vector signed int vec_stack (int count);

int
main ()
{
  printf ("&vec_stack = %016lx\n", (unsigned long) vec_stack);
  vec_stack (3);
  return 0;
}


vector signed int
vec_stack (int count)
{
  register vector signed int v1;
  register vector signed int v2;
  register vector signed int v3;
  register vector signed int v4;
  register vector signed int v5;
  register vector signed int v6;
  register vector signed int v7;
  register vector signed int v8;
  register vector signed int v9;

  unw_fpreg_t vr;

  unw_cursor_t cursor;
  unw_word_t ip, sp;
  unw_context_t uc;
  int ret;
  int verbose = 1;

  /* if (count == 0) return vec_init(); */

  if (count == 0)
    {
      unw_getcontext (&uc);
      if (unw_init_local (&cursor, &uc) < 0)
	{
	  panic ("unw_init_local failed!\n");
	}
      else
	{
	  do
	    {
	      if ((ret = unw_get_reg (&cursor, UNW_REG_IP, &ip)) < 0)
		{
		  panic ("FAILURE: unw_get_reg returned %d for UNW_REG_IP\n",
			 ret);
		}
	      if ((ret = unw_get_reg (&cursor, UNW_REG_SP, &sp)) < 0)
		{
		  panic ("FAILURE: unw_get_reg returned %d for UNW_REG_SP\n",
			 ret);
		}
	      if ((ret = unw_get_fpreg (&cursor, UNW_PPC64_V30, &vr)) < 0)
		{
		  panic
		    ("FAILURE: unw_get_vreg returned %d for UNW_PPC64_V30\n",
		     ret);
		}


	      if (verbose)
		{
		  const char *regname = unw_regname (UNW_PPC64_V30);
		  char proc_name_buffer[256];
		  unw_word_t offset;
                  unsigned int * vec_half1, * vec_half2;
                  vec_half1 = (unsigned int *)&vr;
                  vec_half2 = vec_half1 + 1;
		  printf ("ip = %016lx, sp=%016lx\n", (long) ip, (long) sp);
		  printf ("vr30 = %08x %08x %08x %08x\n",
			  (unsigned int) (*vec_half1 >> 16),
			  (unsigned int) (*vec_half1 & 0xffffffff),
			  (unsigned int) (*vec_half2 >> 16),
			  (unsigned int) (*vec_half2 & 0xffffffff));
		  ret =
		    unw_get_proc_name (&cursor, proc_name_buffer,
				       sizeof (proc_name_buffer), &offset);
		  if (ret == 0)
		    {
		      printf ("proc name = %s, offset = %lx\n",
			      proc_name_buffer, offset);
		    }
		  else
		    {
		      panic ("unw_get_proc_name returned %d\n", ret);
		    }
		  printf ("unw_regname(UNW_PPC_V30) = %s\n\n", regname);
		}

	      ret = unw_step (&cursor);
	      if (ret < 0)
		{
		  unw_get_reg (&cursor, UNW_REG_IP, &ip);
		  panic ("FAILURE: unw_step() returned %d for ip=%lx\n", ret,
			 (long) ip);
		}
	    }
	  while (ret > 0);
	}
    }

  v1 = vec_init ();
  v2 = vec_init ();
  v3 = vec_init ();
  v4 = vec_init ();
  v5 = vec_init ();
  v6 = vec_init ();

  /* make use of all of the registers in some calculation */
  v7 =
    vec_nor (v1, vec_add (v2, vec_sub (v3, vec_and (v4, vec_or (v5, v6)))));

  /*
   * "force" the registers to be non-volatile by making a call and also
   * using the registers after the call.
   */
  v8 = vec_stack (count - 1);

  /*
   * Use the result from the previous call, plus all of the non-volatile
   * registers in another calculation.
   */
  v9 =
    vec_nor (v1,
	     vec_add (v2,
		      vec_sub (v3,
			       vec_and (v4, vec_or (v5, vec_xor (v6, v8))))));

  printf ("v1 - ");
  vec_print (v1);
  printf ("\n");
  printf ("v2 - ");
  vec_print (v2);
  printf ("\n");
  printf ("v3 - ");
  vec_print (v3);
  printf ("\n");
  printf ("v4 - ");
  vec_print (v4);
  printf ("\n");
  printf ("v5 - ");
  vec_print (v5);
  printf ("\n");
  printf ("v6 - ");
  vec_print (v6);
  printf ("\n");
  printf ("v7 - ");
  vec_print (v7);
  printf ("\n");
  printf ("v8 - ");
  vec_print (v8);
  printf ("\n");
  printf ("v9 - ");
  vec_print (v9);
  printf ("\n");

  return v9;
}

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <libunwind.h>

static int verbose;
static int nerrors;

#define panic(args...)							  \
	do { printf (args); ++nerrors; } while (0)

// Assembly routine which sets up the stack for the test then calls another one
// which clobbers the stack, and which in turn calls recover_register below
extern int64_t DW_CFA_expression_testcase(int64_t regnum, int64_t height);

// recover_register is called by the assembly routines. It returns the value of
// a register at a specified height from the inner-most frame. The return value
// is propagated back through the assembly routines to the testcase.
extern int64_t recover_register(int64_t regnum, int64_t height)
{
  // Initialize cursor to current frame
  int rc, i;
  unw_cursor_t cursor;
  unw_context_t context;
  unw_getcontext(&context);
  unw_init_local(&cursor, &context);
  // Unwind frames until required height from inner-most frame (i.e. this one)
  for (i = 0; i < height; ++i)
    {
      rc = unw_step(&cursor);
      if (rc < 0)
        panic("%s: unw_step failed on step %d with return code %d", __FUNCTION__, i, rc);
      else if (rc == 0)
        panic("%s: unw_step failed to reach the end of the stack", __FUNCTION__);
      unw_word_t pc;
      rc = unw_get_reg(&cursor, UNW_REG_IP, &pc);
      if (rc < 0 || pc == 0)
        panic("%s: unw_get_reg failed to locate the program counter", __FUNCTION__);
    }
  // We're now at the required height, extract register
  uint64_t value;
  if ((rc = unw_get_reg(&cursor, (unw_regnum_t) regnum, &value)) != 0)
    panic("%s: unw_get_reg failed to retrieve register %lu", __FUNCTION__, regnum);
  return value;
}

int
main (int argc, char **argv)
{
  if (argc > 1)
    verbose = 1;

  if (DW_CFA_expression_testcase(12, 1) != 0)
    panic("r12 should be clobbered at height 1 (DW_CFA_expression_inner)");
  if (DW_CFA_expression_testcase(12, 2) != 111222333)
    panic("r12 should be restored at height 2 (DW_CFA_expression_testcase)");

  if (nerrors > 0)
    {
      fprintf (stderr, "FAILURE: detected %d errors\n", nerrors);
      exit (-1);
    }

  if (verbose)
    printf ("SUCCESS.\n");
  return 0;
}

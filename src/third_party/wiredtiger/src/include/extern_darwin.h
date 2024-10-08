#pragma once

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wt_futex_wait(volatile WT_FUTEX_WORD *addr, WT_FUTEX_WORD expected, time_t usec,
  WT_FUTEX_WORD *wake_valp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_futex_wake(volatile WT_FUTEX_WORD *addr, WT_FUTEX_WAKE wake, WT_FUTEX_WORD wake_val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */

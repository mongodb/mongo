/**
 * @file mlib/test.h
 * @brief Testing utilities
 * @date 2025-01-30
 *
 * @copyright Copyright (c) 2025
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <mlib/cmp.h>
#include <mlib/config.h>
#include <mlib/intutil.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Place this macro at the head of a (compound) statement to assert that
 * executing that statement aborts the program with SIGABRT.
 *
 * Internally, this will fork the calling process and wait for the child process
 * to terminate. It asserts that the child exits abnormally with SIGABRT. This
 * test assertion is a no-op on Win32, since it does not have a suitable `fork`
 * API.
 *
 * Beware that the child process runs in a forked environment, so it is not
 * safe to use any non-fork-safe functionality, and any modifications to program
 * state will not be visible in the parent. Behavior of attempting to escape the
 * statement (goto/return) is undefined.
 *
 * If the child process does not abort, it will call `_Exit(71)` to indicate
 * to the parent that it did not terminate (the number 71 is chosen arbitrarily)
 *
 * If the token `debug` is passed as a macro argument, then the forking behavior
 * is suppressed, allowing for easier debugging of the statement.
 */
#define mlib_assert_aborts(...) MLIB_PASTE_3(_mlibAssertAbortsStmt, _, __VA_ARGS__)()

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#define _mlibAssertAbortsStmt_()                                                                 \
   for (int once = 1, other_pid = fork(); once; once = 0)                                        \
      for (; once; once = 0)                                                                     \
         if (other_pid != 0) {                                                                   \
            /* We are the parent */                                                              \
            int wstatus;                                                                         \
            waitpid(other_pid, &wstatus, 0);                                                     \
            if (WIFEXITED(wstatus)) {                                                            \
               /* Normal exit! */                                                                \
               _mlib_stmt_did_not_abort(__FILE__, MLIB_FUNC, __LINE__, WEXITSTATUS(wstatus));    \
            } else if (WIFSIGNALED(wstatus)) {                                                   \
               /* Signalled */                                                                   \
               if (WTERMSIG(wstatus) != SIGABRT) {                                               \
                  fprintf(stderr,                                                                \
                          "%s:%d: [%s]: Child process did not exit with SIGABRT! (Exited %d)\n", \
                          __FILE__,                                                              \
                          __LINE__,                                                              \
                          MLIB_FUNC,                                                             \
                          WTERMSIG(wstatus));                                                    \
                  fflush(stderr);                                                                \
                  abort();                                                                       \
               }                                                                                 \
            }                                                                                    \
         } else /* We are the child */                                                           \
            if ((fclose(stderr), 1))                                                             \
               for (;; _Exit(71))                                                                \
                  for (;; _Exit(71)) /* Double loop to prevent the block from `break`ing out */

#else
#define _mlibAssertAbortsStmt_() \
   if (1) {                      \
   } else
#endif

// Called when an assert-aborts statement does not terminate
static inline void
_mlib_stmt_did_not_abort(const char *file, const char *func, int line, int rc)
{
   /* Normal exit! */
   if (rc == 71) {
      fprintf(stderr, "%s:%d: [%s]: Test case did not abort. The statement completed normally.\n", file, line, func);
   } else {
      fprintf(stderr, "%s:%d: [%s]: Test case did not abort (Exited %d)\n", file, line, func, rc);
   }
   fflush(stderr);
   abort();
}

#define _mlibAssertAbortsStmt_debug()                                   \
   for (;; _mlib_stmt_did_not_abort(__FILE__, MLIB_FUNC, __LINE__, -1)) \
      for (;; _mlib_stmt_did_not_abort(__FILE__, MLIB_FUNC, __LINE__, -1))

/**
 * @brief Aggregate type that holds information about a source location
 */
typedef struct mlib_source_location {
   const char *file;
   int lineno;
   const char *func;
} mlib_source_location;

/**
 * @brief Expands to an `mlib_source_location` for the location in which the macro is expanded
 */
#define mlib_this_source_location() (mlib_init(mlib_source_location){(__FILE__), (__LINE__), (MLIB_FUNC)})
// ↑ The paren wrapping is required on VS2017 to prevent it from deleting the preceding comma (?!)

/**
 * @brief Evaluate a check, aborting with a diagnostic if that check fails
 *
 * Can be called with one argument to test a single boolean condition, or three
 * arguments for more useful diagnostics with an infix operator.
 */
#define mlib_check(...) MLIB_ARGC_PICK(_mlib_check, #__VA_ARGS__, __VA_ARGS__)
// One arg:
#define _mlib_check_argc_2(ArgString, Condition) \
   _mlibCheckConditionSimple(Condition, ArgString, NULL, mlib_this_source_location())
// Three args:
#define _mlib_check_argc_4(ArgString, A, Operator, B) \
   MLIB_NOTHING(#A, #B) MLIB_PASTE(_mlibCheckCondition_, Operator)(A, B, NULL)
// Five args:
#define _mlib_check_argc_6(ArgString, A, Operator, B, Infix, Reason) \
   MLIB_NOTHING(#A, #B) MLIB_PASTE(_mlib_check_with_suffix_, Infix)(A, Operator, B, Reason)
#define _mlib_check_with_suffix_because(A, Operator, B, Reason) \
   MLIB_NOTHING(#A, #B) MLIB_PASTE(_mlibCheckCondition_, Operator)(A, B, Reason)
// String-compare:
#define _mlibCheckCondition_str_eq(A, B, Reason) _mlibCheckStrEq(A, B, #A, #B, Reason, mlib_this_source_location())
// Pointer-compare:
#define _mlibCheckCondition_ptr_eq(A, B, Reason) _mlibCheckPtrEq(A, B, #A, #B, Reason, mlib_this_source_location())
// Integer-equal:
#define _mlibCheckCondition_eq(A, B, Reason) \
   _mlibCheckIntCmp(mlib_equal,              \
                    true,                    \
                    "==",                    \
                    mlib_upsize_integer(A),  \
                    mlib_upsize_integer(B),  \
                    #A,                      \
                    #B,                      \
                    Reason,                  \
                    mlib_this_source_location())
// Integer not-equal:
#define _mlibCheckCondition_neq(A, B, Reason) \
   _mlibCheckIntCmp(mlib_equal,               \
                    false,                    \
                    "!=",                     \
                    mlib_upsize_integer(A),   \
                    mlib_upsize_integer(B),   \
                    #A,                       \
                    #B,                       \
                    Reason,                   \
                    mlib_this_source_location())
// Integer comparisons:
#define _mlibCheckCondition_lt(A, B, Reason) \
   _mlibCheckIntCmp(mlib_less,               \
                    true,                    \
                    "<",                     \
                    mlib_upsize_integer(A),  \
                    mlib_upsize_integer(B),  \
                    #A,                      \
                    #B,                      \
                    Reason,                  \
                    mlib_this_source_location())
#define _mlibCheckCondition_lte(A, B, Reason) \
   _mlibCheckIntCmp(mlib_greater,             \
                    false,                    \
                    "≤",                      \
                    mlib_upsize_integer(A),   \
                    mlib_upsize_integer(B),   \
                    #A,                       \
                    #B,                       \
                    Reason,                   \
                    mlib_this_source_location())
#define _mlibCheckCondition_gt(A, B, Reason) \
   _mlibCheckIntCmp(mlib_greater,            \
                    true,                    \
                    ">",                     \
                    mlib_upsize_integer(A),  \
                    mlib_upsize_integer(B),  \
                    #A,                      \
                    #B,                      \
                    Reason,                  \
                    mlib_this_source_location())
#define _mlibCheckCondition_gte(A, B, Reason) \
   _mlibCheckIntCmp(mlib_less,                \
                    false,                    \
                    "≥",                      \
                    mlib_upsize_integer(A),   \
                    mlib_upsize_integer(B),   \
                    #A,                       \
                    #B,                       \
                    Reason,                   \
                    mlib_this_source_location())


// Simple assertion with an explanatory string
#define _mlibCheckCondition_because(Cond, Reason, _null) \
   _mlibCheckConditionSimple(Cond, #Cond, Reason, mlib_this_source_location())

/// Check evaluator when given a single boolean
static inline void
_mlibCheckConditionSimple(bool c, const char *expr, const char *reason, struct mlib_source_location here)
{
   if (!c) {
      fprintf(stderr, "%s:%d: in [%s]: Check condition ⟨%s⟩ failed", here.file, here.lineno, here.func, expr);
      if (reason) {
         fprintf(stderr, " (%s)", reason);
      }
      fprintf(stderr, "\n");
      fflush(stderr);
      abort();
   }
}

// Implement integer comparison checks
static inline void
_mlibCheckIntCmp(enum mlib_cmp_result cres, // The cmp result to check
                 bool cond,                 // Whether we expect the cmp result to match `cres`
                 const char *operator_str,
                 struct mlib_upsized_integer left,
                 struct mlib_upsized_integer right,
                 const char *left_expr,
                 const char *right_expr,
                 const char *reason,
                 struct mlib_source_location here)
{
   if (((mlib_cmp)(left, right, 0) == cres) != cond) {
      fprintf(stderr,
              "%s:%d: in [%s]: Check [⟨%s⟩ %s ⟨%s⟩] failed:\n",
              here.file,
              here.lineno,
              here.func,
              left_expr,
              operator_str,
              right_expr);
      fprintf(stderr, "    ");
      if (left.is_signed) {
         fprintf(stderr, "%lld", (long long)left.bits.as_signed);
      } else {
         fprintf(stderr, "%llu", (unsigned long long)left.bits.as_unsigned);
      }
      fprintf(stderr, " ⟨%s⟩\n", left_expr);
      fprintf(stderr, "    ");
      if (right.is_signed) {
         fprintf(stderr, "%lld", (long long)right.bits.as_signed);
      } else {
         fprintf(stderr, "%llu", (unsigned long long)right.bits.as_unsigned);
      }
      fprintf(stderr, " ⟨%s⟩\n", right_expr);
      if (reason) {
         fprintf(stderr, "Because: %s\n", reason);
      }
      fflush(stderr);
      abort();
   }
}

// Pointer-comparison
static inline void
_mlibCheckPtrEq(const void *left,
                const void *right,
                const char *left_expr,
                const char *right_expr,
                const char *reason,
                struct mlib_source_location here)
{
   if (left != right) {
      fprintf(stderr,
              "%s:%d: in [%s]: Check [⟨%s⟩ pointer-equal ⟨%s⟩] failed:\n",
              here.file,
              here.lineno,
              here.func,
              left_expr,
              right_expr);
      fprintf(stderr,
              "    %p ⟨%s⟩\n"
              "  ≠ %p ⟨%s⟩\n",
              left,
              left_expr,
              right,
              right_expr);
      if (reason) {
         fprintf(stderr, "Because: %s\n", reason);
      }
      fflush(stderr);
      abort();
   }
}

// String-comparison
static inline void
_mlibCheckStrEq(const char *left,
                const char *right,
                const char *left_expr,
                const char *right_expr,
                const char *reason,
                struct mlib_source_location here)
{
   if (strcmp(left, right)) {
      fprintf(stderr,
              "%s:%d: in [%s]: Check [⟨%s⟩ str-equal ⟨%s⟩] failed:\n",
              here.file,
              here.lineno,
              here.func,
              left_expr,
              right_expr);
      fprintf(stderr,
              "    “%s” ⟨%s⟩\n"
              "  ≠ “%s” ⟨%s⟩\n",
              left,
              left_expr,
              right,
              right_expr);
      if (reason) {
         fprintf(stderr, "Because: %s\n", reason);
      }
      fflush(stderr);
      abort();
   }
}

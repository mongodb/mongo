/**
 * @file mlib/loop.h
 * @brief Looping utility macros
 * @date 2025-01-29
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
#ifndef MLIB_LOOP_H_INCLUDED
#define MLIB_LOOP_H_INCLUDED

#include <mlib/config.h>

#include <stdbool.h>
#include <stdint.h> // u/intmax_t

/**
 * @brief Begin a loop over a range of integer values. Supports:
 *
 * - `mlib_foreach_{u,i}range(Var, Stop)`
 * - `mlib_foreach_{u,i}range(Var, Start, Stop)`
 *
 * If omitted, starts at zero. The loop does not include the `Stop` value. The `Var`
 * variable cannot be modified within the loop. The loop variable is declared as the maximum
 * precision type for the requested signedness.
 */
#define mlib_foreach_urange(...) MLIB_ARGC_PICK(_mlib_foreach_urange, __VA_ARGS__)
#define mlib_foreach_irange(...) MLIB_ARGC_PICK(_mlib_foreach_irange, __VA_ARGS__)
#define _mlib_foreach_urange_argc_2(VarName, Stop) _mlib_foreach_urange_argc_3(VarName, 0, Stop)
#define _mlib_foreach_urange_argc_3(VarName, Start, Stop) \
   _mlibForeachRange(uintmax_t,                           \
                     VarName,                             \
                     Start,                               \
                     Stop,                                \
                     MLIB_PASTE(VarName, _start),         \
                     MLIB_PASTE(VarName, _stop),          \
                     MLIB_PASTE(VarName, _counter))
#define _mlib_foreach_irange_argc_2(VarName, Stop) _mlib_foreach_irange_argc_3(VarName, 0, Stop)
#define _mlib_foreach_irange_argc_3(VarName, Start, Stop) \
   _mlibForeachRange(intmax_t,                            \
                     VarName,                             \
                     Start,                               \
                     Stop,                                \
                     MLIB_PASTE(VarName, _start),         \
                     MLIB_PASTE(VarName, _stop),          \
                     MLIB_PASTE(VarName, _counter))

/**
 * @brief Loop over a pointed-to array
 *
 * @param T The type of the array elements
 * @param Var Identifier to declare as the pointer to the current element
 * @param ArrayPtr A pointer to the beginning of the array
 * @param Count The number of elements in the array
 */
#define mlib_foreach(T, Var, ArrayPtr, Count) \
   _mlibForeach(T, Var, ArrayPtr, Count, MLIB_PASTE(Var, _start), MLIB_PASTE(Var, _stop), MLIB_PASTE(Var, _iter))
/**
 * @brief Loop over the elements of a C array
 *
 * @param T the type of the array elements
 * @param Var Identifier to declare as the pointer to the current element
 * @param Array An expression of array type (not a pointer)
 */
#define mlib_foreach_arr(T, Var, Array) mlib_foreach (T, Var, Array, (sizeof Array / sizeof Array[0]))

// clang-format off
#define _mlibForeachRange(VarType, VarName, StartValue, StopValue, StartVar, StopVar, Counter) \
    _mlibLoopMagicBegin() \
    /* Capture the starting and stopping value first */ \
    for (VarType StartVar = (StartValue), StopVar = (StopValue); !_mlibLoopIsDone;) \
    _mlibLoopMagicEnd( \
        /* Init counter to the start value */ \
        VarType Counter = StartVar, \
        /* Stop when the counter is not less than the stop value */ \
        (_mlibLoopState.first = Counter == StartVar, \
         _mlibLoopState.last = Counter + 1 == StopVar, \
         Counter < StopVar), \
        /* Increment the counter at loop end */ \
        ++Counter, \
        /* Declare the loop variable as const at the start of each iteraiton */ \
        const VarType VarName = Counter)

#define _mlibForeach(T, VarName, ArrayPtr, Count, StartVar, StopVar, Iter) \
    _mlibLoopMagicBegin() \
    /* Capture the starting and stopping position so we only evaluate them once */ \
    for (T* const StartVar = (ArrayPtr) + 0; !_mlibLoopIsDone;) \
    for (T* const StopVar = StartVar + (Count); !_mlibLoopIsDone;) \
    _mlibLoopMagicEnd( \
        /* Init the iteration pointer to the array start */ \
        T* Iter = StartVar, \
        /* Stop when the iterator points to the stop position */ \
        (_mlibLoopState.first = Iter == StartVar, \
         _mlibLoopState.last = Iter + 1 == StopVar, \
         Iter != StopVar), \
        /* Advance the iterator on each loop */ \
        ++Iter, \
        /* Declare a constant pointer to the current element at the top of the loop */ \
        T* const VarName = Iter)


#define _mlibLoopDidBreak MLIB_PASTE(_mlibLoopDidBreak_lno_, __LINE__)
#define _mlibLoopOnce MLIB_PASTE(_mlibLoopOnce_lno_, __LINE__)
#define _mlibLoopIsDone MLIB_PASTE(_mlibLoopIsDone_lno_, __LINE__)
#define _mlibLoopIsState MLIB_PASTE(_mlibLoopIsState_lno_, __LINE__)
#define _mlibLoopMagicBegin() \
    /* Loop stop condition */ \
    for (int _mlibLoopIsDone = 0; !_mlibLoopIsDone;) \
    /* Track if the user broke out of the inner loop */ \
    for (int _mlibLoopDidBreak = 0; !_mlibLoopIsDone;) \
    /* Loop variables */ \
    for (struct mlib_loop_state _mlibLoopState = {0, 0, 0}; !_mlibLoopIsDone;)

/**
 * @brief Struct type declared within the scope of an `mlib_foreach` loop, which
 * contains information about the running loop.
 */
struct mlib_loop_state {
    // The current zero-based index of the loop
    size_t index;
    // Whether the current iteration is the first in the loop
    bool first;
    // Whether the current iteration will be the last in the loop
    bool last;
};

/// InitStmt: Statement that executes once at the top of the loop
/// ContinueCond: Condition at which the loop will stop
/// StepExpr: Expression for the loop step
/// HeadStmt: A statement that appears at the head of the loop, executed once on each iteration
#define _mlibLoopMagicEnd(InitStmt, ContinueCond, StepExpr, HeadStmt) \
    for (\
        /* Run the init statement */ \
        InitStmt; \
        /* Test the loop condition, unless we `break` out of the loop */ \
        !(_mlibLoopIsDone = _mlibLoopIsDone || !(ContinueCond)); \
        /* Run the step expression, unless we `break` from the loop */ \
        (void)(_mlibLoopIsDone || ((void)(StepExpr), 1))) \
    /* `break` detection: */ \
    for (int _mlibLoopOnce = 0; !_mlibLoopOnce; _mlibLoopOnce = 1, _mlibLoopIsDone = _mlibLoopDidBreak) \
    /* Loop state information */ \
    for (const struct mlib_loop_state loop = _mlibLoopState; \
         !_mlibLoopOnce; \
         ((void)loop, _mlibLoopOnce = 1, ++_mlibLoopState.index)) \
    for (HeadStmt; \
        /* Set `_mlibLoopDidBreak` to true at the start of the loop: */ \
        !_mlibLoopOnce && (_mlibLoopDidBreak = 1); \
        /* If loop exits normally, set `_mlibLoopDidBreak` to false */ \
        _mlibLoopDidBreak = 0, _mlibLoopOnce = 1)


// clang-format on

#endif // MLIB_LOOP_H_INCLUDED

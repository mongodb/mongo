/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Bytecode-level profiling support based on program counter offset within
 * functions and overall scripts.
 *
 * SpiderMonkey compiles functions and scripts into bytecode representation.
 * During execution, SpiderMonkey can keep a counter of how often each bytecode
 * (specifically, bytecodes that are the targets of jumps) is reached, to track
 * basic information about how many times any particular bytecode in a script
 * or function is executed.  These functions allow this counting to be started
 * and stopped, and the results of such counting to be examined.
 *
 * Accumulated counting data is tracked per "script" (where "script" is either
 * a JavaScript function from source text or a top-level script or module).
 * Accumulated PCCount data thus prevents the particular scripts that were
 * executed from being GC'd.  Once PCCount profiling has been stopped, you
 * should examine and then purge the accumulated data as soon as possible.
 *
 * Much of the data tracked here is pretty internal and may only have meaning if
 * you're familiar with bytecode and Ion details.  Hence why these APIs are
 * "experimental".
 */

#ifndef js_experimental_PCCountProfiling_h
#define js_experimental_PCCountProfiling_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSString;

namespace JS {

/**
 * Start PCCount-based profiling if it hasn't already been started.
 *
 * This discards previously-accumulated PCCount profiling data from a prior
 * PCCount profiling session.  It also discards all active JIT code (as such
 * code was compiled to *not* perform PCCount-based profiling), so a brief
 * performance hit to code execution can be expected after calling this.
 */
extern JS_PUBLIC_API void StartPCCountProfiling(JSContext* cx);

/**
 * Stop PCCount-based profiling if it has been started.
 *
 * Internally, this accumulates PCCount profiling results into an array of
 * (script, script count info) for future consumption.  If accumulation fails,
 * this array will just be empty.
 *
 * This also discard all active JIT code (as such code was compiled to perform
 * PCCount-based profiling), so a brief performance hit to code execution can be
 * expected after calling this.
 */
extern JS_PUBLIC_API void StopPCCountProfiling(JSContext* cx);

/**
 * Get a count of all scripts for which PCCount profiling data was accumulated,
 * after PCCount profiling has been stopped.
 *
 * As noted above, if an internal error occurred when profiling was stopped,
 * this function will return 0.
 */
extern JS_PUBLIC_API size_t GetPCCountScriptCount(JSContext* cx);

/**
 * Get a JSON string summarizing PCCount data for the given script, by index
 * within the internal array computed when PCCount profiling was stopped, of
 * length indicated by |JS::GetPCCountScriptCount|.
 *
 * The JSON string will encode an object of this form:
 *
 *   {
 *     "file": filename for the script,
 *     "line": line number within the script,
 *
 *     // OPTIONAL: only if this script is a function
 *     "name": function name
 *
 *     "totals":
 *       {
 *         "interp": sum of execution counts at all bytecode locations,
 *         "ion": sum of Ion activity counts,
 *       }
 *   }
 *
 * There is no inherent ordering to the (script, script count info) pairs within
 * the array.  Callers generally should expect to call this function in a loop
 * to get summaries for all executed scripts.
 */
extern JS_PUBLIC_API JSString* GetPCCountScriptSummary(JSContext* cx,
                                                       size_t script);

/**
 * Get a JSON string encoding detailed PCCount data for the given script, by
 * index within the internal array computed when PCCount profiling was stopped,
 * of length indicated by |JS::GetPCCountScriptCount|.
 *
 * The JSON string will encode an object of this form:
 *
 *   {
 *     "text": a string containg (possibly decompiled) source of the script,
 *     "line": line number within the script,
 *
 *     "opcodes":
 *       [
 *         // array elements of this form, corresponding to the script's
 *         // bytecodes
 *         {
 *           "id": offset of bytecode within script
 *           "line": line number for bytecode,
 *           "name": the name of the bytecode in question,
 *           "text": text of the expression being evaluated,
 *           "counts":
 *             {
 *               // OPTIONAL: only if the count is nonzero
 *               "interp": count of times executed,
 *             },
 *       ],
 *
 *     // OPTIONAL: only if the script is executed under Ion
 *     "ion":
 *       [
 *         // array of elements of this form, corresponding to Ion basic blocks
 *         {
 *           "id": Ion block id,
 *           "offset": Ion block offset,
 *           "successors":
 *             [
 *               // list of Ion block ids from which execution may proceed after
 *               // this one
 *             ],
 *           "hits": count of times this block was hit during execution,
 *           "code": a string containing the code in this Ion basic block,
 *         }
 *       ],
 *   }
 *
 * There is no inherent ordering to the (script, script count info) pairs within
 * the array.  Callers generally should expect to call this function in a loop
 * to get summaries for all executed scripts.
 */
extern JS_PUBLIC_API JSString* GetPCCountScriptContents(JSContext* cx,
                                                        size_t script);

/**
 * Purge all accumulated PCCount profiling data, particularly the internal array
 * that |JS::GetPCCountScript{Count,Summary,Contents}| exposes -- and all that
 * array's references to previously-executed scripts.
 */
extern JS_PUBLIC_API void PurgePCCounts(JSContext* cx);

}  // namespace JS

#endif  // js_experimental_PCCountProfiling_h

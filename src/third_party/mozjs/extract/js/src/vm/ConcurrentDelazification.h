/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ConcurrentDelazification_h
#define vm_ConcurrentDelazification_h

#include "mozilla/MemoryReporting.h"  // mozilla::MallocSizeOf

#include <stddef.h>  // size_t
#include <utility>   // std::pair

#include "frontend/CompilationStencil.h"  // frontend::{InitialStencilAndDelazifications, CompilationStencil, ScriptStencilRef, CompilationStencilMerger}
#include "frontend/ScriptIndex.h"         // frontend::ScriptIndex
#include "js/AllocPolicy.h"               // SystemAllocPolicy
#include "js/CompileOptions.h"  // JS::PrefableCompileOptions, JS::ReadOnlyCompileOptions
#include "js/experimental/JSStencil.h"  // RefPtrTraits for InitialStencilAndDelazifications
#include "js/UniquePtr.h"               // UniquePtr
#include "js/Vector.h"                  // Vector

namespace js {

class FrontendContext;

// Base class for implementing the various strategies to iterate over the
// functions to be delazified, or to decide when to stop doing any
// delazification.
//
// When created, the `add` function should be called with the top-level
// ScriptIndex.
struct DelazifyStrategy {
  using ScriptIndex = frontend::ScriptIndex;
  virtual ~DelazifyStrategy() = default;

  // Returns true if no more functions should be delazified. Note, this does not
  // imply that every function got delazified.
  virtual bool done() const = 0;

  // Return a function identifier which represent the next function to be
  // delazified. If no more function should be delazified, then return 0.
  virtual ScriptIndex next() = 0;

  // Empty the list of functions to be processed next. done() should return true
  // after this call.
  virtual void clear() = 0;

  // Insert an index in the container of the delazification strategy. A strategy
  // can choose to ignore the insertion of an index in its queue of function to
  // delazify. Return false only in case of errors while inserting, and true
  // otherwise.
  [[nodiscard]] virtual bool insert(ScriptIndex index,
                                    frontend::ScriptStencilRef& ref) = 0;

  // Add the inner functions of a delazified function. This function should only
  // be called with a function which has some bytecode associated with it, and
  // register functions which parent are already delazified.
  //
  // This function is called with the script index of:
  //  - top-level script, when starting the off-thread delazification.
  //  - functions added by `add` and delazified by `DelazificationContext`.
  [[nodiscard]] bool add(FrontendContext* fc,
                         const frontend::CompilationStencil& stencil,
                         ScriptIndex index);
};

// Delazify all functions using a Depth First traversal of the function-tree
// ordered, where each functions is visited in source-order.
//
// When `add` is called with the top-level ScriptIndex. This will push all inner
// functions to a stack such that they are popped in source order. Each
// function, once delazified, would be used to schedule their inner functions
// the same way.
//
// Hypothesis: This strategy parses all functions in source order, with the
// expectation that calls will follow the same order, and that helper thread
// would always be ahead of the execution.
struct DepthFirstDelazification final : public DelazifyStrategy {
  Vector<ScriptIndex, 0, SystemAllocPolicy> stack;

  bool done() const override { return stack.empty(); }
  ScriptIndex next() override { return stack.popCopy(); }
  void clear() override { return stack.clear(); }
  bool insert(ScriptIndex index, frontend::ScriptStencilRef&) override {
    return stack.append(index);
  }
};

// Delazify all functions using a traversal which select the largest function
// first. The intent being that if the main thread races with the helper thread,
// then the main thread should only have to parse small functions instead of the
// large ones which would be prioritized by this delazification strategy.
struct LargeFirstDelazification final : public DelazifyStrategy {
  using SourceSize = uint32_t;
  Vector<std::pair<SourceSize, ScriptIndex>, 0, SystemAllocPolicy> heap;

  bool done() const override { return heap.empty(); }
  ScriptIndex next() override;
  void clear() override { return heap.clear(); }
  bool insert(ScriptIndex, frontend::ScriptStencilRef&) override;
};

class DelazificationContext {
  const JS::PrefableCompileOptions initialPrefableOptions_;

  // Queue of functions to be processed while delazifying.
  UniquePtr<DelazifyStrategy> strategy_;

  // Every delazified function is merged back to provide context for delazifying
  // even more functions.
  frontend::CompilationStencilMerger merger_;

  RefPtr<frontend::InitialStencilAndDelazifications> stencils_;

  // Record any errors happening while parsing or generating bytecode.
  FrontendContext fc_;

  size_t stackQuota_;

  bool isInterrupted_ = false;

 public:
  explicit DelazificationContext(
      const JS::PrefableCompileOptions& initialPrefableOptions,
      size_t stackQuota)
      : initialPrefableOptions_(initialPrefableOptions),
        stackQuota_(stackQuota) {}

  bool init(const JS::ReadOnlyCompileOptions& options,
            frontend::InitialStencilAndDelazifications* stencils);
  bool delazify();

  // This function is called by `delazify` function to know whether the
  // delazification should be interrupted.
  //
  // The `delazify` function holds on a thread until all functions iterated
  // over by the strategy. However, as a `delazify` function iterates over
  // multiple functions, it can easily be interrupted at function boundaries.
  //
  // TODO: (Bug 1773683) Plug this with the mozilla::Task::RequestInterrupt
  // function which is wrapping HelperThreads tasks within Mozilla.
  bool isInterrupted() const { return isInterrupted_; }
  void interrupt() { isInterrupted_ = true; }

  bool done() const;

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

} /* namespace js */

#endif /* vm_ConcurrentDelazification_h */

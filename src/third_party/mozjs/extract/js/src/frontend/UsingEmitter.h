/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_UsingEmitter_h
#define frontend_UsingEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include "frontend/TryEmitter.h"
#include "vm/UsingHint.h"

namespace js::frontend {

struct BytecodeEmitter;
class EmitterScope;

// This enum simply refers to the kind of block we are operating in. The present
// use case of this is for disposal related code to special case the handling of
// disposals in different blocks.
enum class BlockKind : uint8_t {
  ForOf,

  // Other here refers to any generic block which doesnt require any
  // special handling.
  Other
};

// Class for emitting bytecode for disposal loops.
// https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposeresources
//
// Usage: (check for the return value is omitted for simplicity)
//
//   at the point where the disposal loop is needed
//     DisposalEmitter de(bce, hasAsyncDisposables, initialCompletion);
//     de.prepareForDisposeCapability();
//     emit_DisposeCapability();
//     de.emitEnd(es);
//
class MOZ_STACK_CLASS DisposalEmitter {
 private:
  BytecodeEmitter* bce_;
  bool hasAsyncDisposables_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+  prepareForDisposeCapability  +-----------------------------+
  // | Start |------------------------------>| prepareForDisposeCapability |--+
  // +-------+                               +-----------------------------+  |
  //                                                                          |
  //   +----------------------------------------------------------------------+
  //   |
  //   |  emitEnd  +-----+
  //   +---------->| End |
  //               +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForDisposeCapability.
    DisposeCapability,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

  [[nodiscard]] bool emitResourcePropertyAccess(TaggedParserAtomIndex prop,
                                                unsigned resourcesFromTop = 1);

 public:
  DisposalEmitter(BytecodeEmitter* bce, bool hasAsyncDisposables)
      : bce_(bce), hasAsyncDisposables_(hasAsyncDisposables) {}

  [[nodiscard]] bool prepareForDisposeCapability();

  [[nodiscard]] bool emitEnd(EmitterScope& es);
};

// Class for emitting bytecode for using declarations.
//
// Usage: (check for the return value is omitted for simplicity)
//
//  at the point of scope start
//    UsingEmitter ue(bce);
//    ue.prepareForDisposableScopeBody();
//
//  at the point of using decl assignment, e.g. `using x = y;`
//    ue.prepareForAssignment(UsingHint::Normal);
//    emit_Assignment();
//
//  at points requiring non-local jumps, like break, continue
//    ue.emitNonLocalJump(&currentScope);
//
//  at the point of scope end
//    ue.emitEnd();
class MOZ_STACK_CLASS UsingEmitter {
 private:
  mozilla::Maybe<TryEmitter> tryEmitter_;

  bool hasAwaitUsing_ = false;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+  prepareForDisposableScopeBody
  // | Start |---------------------------------+
  // +-------+                                 |
  //                                           |
  //   +---------------------------------------+
  //   |
  //   |       +---------------------+     emitEnd   +-----+
  //   +-->+-->| DisposableScopeBody |--+----------->| End |
  //       ^   +---------------------+  |            +-----+
  //       |                            |
  //       |    prepareForAssignment    |
  //       +<---------------------------+
  //       ^                            |
  //       |    emitNonLocalJump        |
  //       +----------------------------+
  //
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForDisposableScopeBody.
    DisposableScopeBody,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

  [[nodiscard]] bool emitGetDisposeMethod(UsingHint hint);

  [[nodiscard]] bool emitCreateDisposableResource(UsingHint hint);

  [[nodiscard]] bool emitTakeDisposeCapability();

 protected:
  BytecodeEmitter* bce_;

  [[nodiscard]] bool emitThrowIfException();

  [[nodiscard]] bool emitDisposeResourcesForEnvironment(EmitterScope& es);

 public:
  explicit UsingEmitter(BytecodeEmitter* bce);

  bool hasAwaitUsing() const { return hasAwaitUsing_; }

  void setHasAwaitUsing(bool hasAwaitUsing) { hasAwaitUsing_ = hasAwaitUsing; }

  [[nodiscard]] bool prepareForDisposableScopeBody(BlockKind blockKind);

  [[nodiscard]] bool prepareForAssignment(UsingHint hint);

  [[nodiscard]] bool emitEnd();
};

// This is a version of UsingEmitter specialized to help emit code for
// using declarations in for-of loop heads e.g.: `for (using x of y) {}`.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   at the point of the for-of loop head
//     ForOfDisposalEmitter disposeBeforeIter(bce, hasAwaitUsing);
//     disposeBeforeIter.prepareForForOfLoopIteration();
//     emit_Loop();
//
//   at the point of loop end
//     prepare_IteratorClose();
//     disposeBeforeIter.emitEnd();
//
class MOZ_STACK_CLASS ForOfDisposalEmitter : protected UsingEmitter {
 private:
#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+  prepareForForOfLoopIteration   +-----------+
  // | Start |-------------------------------->| Iteration |--+
  // +-------+                                 +-----------+  |
  //                                                          |
  //   +------------------------------------------------------+
  //   |
  //   |  emitEnd  +-----+
  //   +---------->| End |
  //               +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForForOfLoopIteration.
    Iteration,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif
 public:
  explicit ForOfDisposalEmitter(BytecodeEmitter* bce, bool hasAwaitUsing)
      : UsingEmitter(bce) {
    setHasAwaitUsing(hasAwaitUsing);
  }

  [[nodiscard]] bool prepareForForOfLoopIteration();

  [[nodiscard]] bool emitEnd();
};

// This is a version of UsingEmitter specialized to help emit code for
// non-local jumps in for-of loops for closing iterators.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   at the point of IteratorClose inside non-local jump
//     NonLocalIteratorCloseUsingEmitter disposeBeforeIterClose(bce);
//     disposeBeforeIterClose.prepareForIteratorClose(&currentScope);
//     emit_IteratorClose();
//     disposeBeforeIterClose.emitEnd(&currentScope);
//
class MOZ_STACK_CLASS NonLocalIteratorCloseUsingEmitter
    : protected UsingEmitter {
 private:
  mozilla::Maybe<TryEmitter> tryClosingIterator_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+  prepareForIteratorClose  +-------------------------+
  // | Start |-------------------------->| prepareForIteratorClose |--+
  // +-------+                           +-------------------------+  |
  //                                                                  |
  //   +--------------------------------------------------------------+
  //   |
  //   |  emitEnd  +-----+
  //   +---------->| End |
  //               +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForIteratorClose.
    IteratorClose,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 public:
  explicit NonLocalIteratorCloseUsingEmitter(BytecodeEmitter* bce)
      : UsingEmitter(bce) {}

  [[nodiscard]] bool prepareForIteratorClose(EmitterScope& es);

  [[nodiscard]] bool emitEnd();
};

}  // namespace js::frontend

#endif  // frontend_UsingEmitter_h

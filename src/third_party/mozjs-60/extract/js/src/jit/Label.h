/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Label_h
#define jit_Label_h

#include "jit/Ion.h"

namespace js {
namespace jit {

struct LabelBase
{
  private:
    // We use uint32_t instead of bool to ensure MSVC packs these fields
    // correctly.
    uint32_t bound_ : 1;

    // offset_ < INVALID_OFFSET means that the label is either bound or has
    // incoming uses and needs to be bound.
    uint32_t offset_ : 31;

    void operator=(const LabelBase& label) = delete;

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  public:
#endif
    static const uint32_t INVALID_OFFSET = 0x7fffffff; // UINT31_MAX.

  public:
    LabelBase() : bound_(false), offset_(INVALID_OFFSET)
    { }

    // If the label is bound, all incoming edges have been patched and any
    // future incoming edges will be immediately patched.
    bool bound() const {
        return bound_;
    }
    int32_t offset() const {
        MOZ_ASSERT(bound() || used());
        return offset_;
    }
    // Returns whether the label is not bound, but has incoming uses.
    bool used() const {
        return !bound() && offset_ < INVALID_OFFSET;
    }
    // Binds the label, fixing its final position in the code stream.
    void bind(int32_t offset) {
        MOZ_ASSERT(!bound());
        MOZ_ASSERT(offset >= 0);
        MOZ_ASSERT(uint32_t(offset) < INVALID_OFFSET);
        offset_ = offset;
        bound_ = true;
        MOZ_ASSERT(offset_ == offset, "offset fits in 31 bits");
    }
    // Marks the label as neither bound nor used.
    void reset() {
        offset_ = INVALID_OFFSET;
        bound_ = false;
    }
    // Sets the label's latest used position.
    void use(int32_t offset) {
        MOZ_ASSERT(!bound());
        MOZ_ASSERT(offset >= 0);
        MOZ_ASSERT(uint32_t(offset) < INVALID_OFFSET);
        offset_ = offset;
        MOZ_ASSERT(offset_ == offset, "offset fits in 31 bits");
    }
};

// A label represents a position in an assembly buffer that may or may not have
// already been generated. Labels can either be "bound" or "unbound", the
// former meaning that its position is known and the latter that its position
// is not yet known.
//
// A jump to an unbound label adds that jump to the label's incoming queue. A
// jump to a bound label automatically computes the jump distance. The process
// of binding a label automatically corrects all incoming jumps.
class Label : public LabelBase
{
  public:
    ~Label()
    {
#ifdef DEBUG
        // The assertion below doesn't hold if an error occurred.
        JitContext* context = MaybeGetJitContext();
        bool hadError = js::oom::HadSimulatedOOM() ||
                        (context && context->runtime && context->runtime->hadOutOfMemory());
        MOZ_ASSERT_IF(!hadError, !used());
#endif
    }
};

static_assert(sizeof(Label) == sizeof(uint32_t), "Label should have same size as uint32_t");

// Label's destructor asserts that if it has been used it has also been bound.
// In the case long-lived labels, however, failed compilation (e.g. OOM) will
// trigger this failure innocuously. This Label silences the assertion.
class NonAssertingLabel : public Label
{
  public:
    ~NonAssertingLabel()
    {
#ifdef DEBUG
        if (used())
            bind(0);
#endif
    }
};

} // namespace jit
} // namespace js

#endif // jit_Label_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Disassembler_shared_h
#define jit_shared_Disassembler_shared_h

#include "mozilla/Atomics.h"

#include "jit/Label.h"
#ifdef JS_DISASM_SUPPORTED
# include "jit/shared/IonAssemblerBuffer.h"
#endif

using js::jit::Label;
using js::Sprinter;

#if defined(JS_DISASM_ARM) || defined(JS_DISASM_ARM64)
#  define JS_DISASM_SUPPORTED
#endif

namespace js {
namespace jit {

// A wrapper around spew/disassembly functionality.  The disassembler is built
// on a per-instruction disassembler (as in our ARM, ARM64 back-ends) and
// formats labels with meaningful names and literals with meaningful values, if
// the assembler creates documentation (with provided helpers) at appropriate
// points.

class DisassemblerSpew
{
#ifdef JS_DISASM_SUPPORTED
    struct Node
    {
        const Label* key;       // Never dereferenced, only used for its value
        uint32_t value;         // The printable label value
        bool bound;             // If the label has been seen by spewBind()
        Node* next;
    };

    Node* lookup(const Label* key);
    Node* add(const Label* key, uint32_t value);
    bool remove(const Label* key);

    uint32_t probe(const Label* l);
    uint32_t define(const Label* l);
    uint32_t internalResolve(const Label* l);
#endif

    void spewVA(const char* fmt, va_list args) MOZ_FORMAT_PRINTF(2, 0);

  public:
    DisassemblerSpew();
    ~DisassemblerSpew();

#ifdef JS_DISASM_SUPPORTED
    // Set indentation strings.  The spewer retains a reference to s.
    void setLabelIndent(const char* s);
    void setTargetIndent(const char* s);
#endif

    // Set the spew printer, which will always be used if it is set, regardless
    // of whether the system spew channel is enabled or not.  The spewer retains
    // a reference to sp.
    void setPrinter(Sprinter* sp);

    // Return true if disassembly spew is disabled and no additional printer is
    // set.
    bool isDisabled();

    // Format and print text on the spew channel; output is suppressed if spew
    // is disabled.  The output is not indented, and is terminated by a newline.
    void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3);

    // Documentation for a label reference.
    struct LabelDoc
    {
#ifdef JS_DISASM_SUPPORTED
	LabelDoc() : doc(0), bound(false), valid(false) {}
	LabelDoc(uint32_t doc, bool bound) : doc(doc), bound(bound), valid(true) {}
	const uint32_t doc;
	const bool bound;
	const bool valid;
#else
	LabelDoc() {}
	LabelDoc(uint32_t, bool) {}
#endif
    };

    // Documentation for a literal load.
    struct LiteralDoc
    {
#ifdef JS_DISASM_SUPPORTED
        enum class Type { Patchable, I32, U32, I64, U64, F32, F64 };
        const Type type;
        union {
            int32_t  i32;
            uint32_t u32;
            int64_t  i64;
            uint64_t u64;
            float    f32;
            double   f64;
        } value;
        LiteralDoc() : type(Type::Patchable) {}
        explicit LiteralDoc(int32_t v) : type(Type::I32) { value.i32 = v; }
        explicit LiteralDoc(uint32_t v) : type(Type::U32) { value.u32 = v; }
        explicit LiteralDoc(int64_t v) : type(Type::I64) { value.i64 = v; }
        explicit LiteralDoc(uint64_t v) : type(Type::U64) { value.u64 = v; }
        explicit LiteralDoc(float v) : type(Type::F32) { value.f32 = v; }
        explicit LiteralDoc(double v) : type(Type::F64) { value.f64 = v; }
#else
        LiteralDoc() {}
        explicit LiteralDoc(int32_t) {}
        explicit LiteralDoc(uint32_t) {}
        explicit LiteralDoc(int64_t) {}
        explicit LiteralDoc(uint64_t) {}
        explicit LiteralDoc(float) {}
        explicit LiteralDoc(double) {}
#endif
    };

    // Reference a label, resolving it to a printable representation.
    //
    // NOTE: The printable representation depends on the state of the label, so
    // if we call resolve() when emitting & disassembling a branch instruction
    // then it should be called before the label becomes Used, if emitting the
    // branch can change the label's state.
    //
    // If the disassembler is not defined this returns a structure that is
    // marked not valid.
    LabelDoc refLabel(const Label* l);

#ifdef JS_DISASM_SUPPORTED
    // Spew the label information previously gathered by refLabel(), at a point
    // where the label is referenced.  The output is indented by targetIndent_
    // and terminated by a newline.
    void spewRef(const LabelDoc& target);

    // Spew the label at the point where the label is bound.  The output is
    // indented by labelIndent_ and terminated by a newline.
    void spewBind(const Label* label);

    // Spew a retarget directive at the point where the retarget is recorded.
    // The output is indented by labelIndent_ and terminated by a newline.
    void spewRetarget(const Label* label, const Label* target);

    // Format a literal value into the buffer.  The buffer is always
    // NUL-terminated even if this chops the formatted value.
    void formatLiteral(const LiteralDoc& doc, char* buffer, size_t bufsize);

    // Print any unbound labels, one per line, with normal label indent and with
    // a comment indicating the label is not defined.  Labels can be referenced
    // but unbound in some legitimate cases, normally for traps.  Printing them
    // reduces confusion.
    void spewOrphans();
#endif

  private:
    Sprinter* printer_;
#ifdef JS_DISASM_SUPPORTED
    const char* labelIndent_;
    const char* targetIndent_;
    uint32_t spewNext_;
    Node* nodes_;
    uint32_t tag_;

    // This global is used to disambiguate concurrently live assemblers, see
    // comments in Disassembler-shared.cpp for why this is desirable.
    //
    // The variable is atomic to avoid any kind of complaint from thread
    // sanitizers etc.  However, trying to look at disassembly without using
    // --no-threads is basically insane, so you can ignore the multi-threading
    // implications here.
    static mozilla::Atomic<uint32_t> counter_;
#endif
};

}
}

#endif // jit_shared_Disassembler_shared_h

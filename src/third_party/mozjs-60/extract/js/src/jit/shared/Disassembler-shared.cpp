/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/Disassembler-shared.h"

#include "jit/JitSpewer.h"

using namespace js::jit;

#ifdef JS_DISASM_SUPPORTED
// Concurrent assemblers are disambiguated by prefixing every disassembly with a
// tag that is quasi-unique, and certainly unique enough in realistic cases
// where we are debugging and looking at disassembler output.  The tag is a
// letter or digit between brackets prefixing the disassembly, eg, [X]. This
// wraps around every 62 assemblers.
//
// When running with --no-threads we can still have concurrent assemblers in the
// form of nested assemblers, as when an IC stub is created by one assembler
// while a JS compilation is going on and producing output in another assembler.
//
// We generate the tag for an assembler by incrementing a global mod-2^32
// counter every time a new disassembler is created.

mozilla::Atomic<uint32_t> DisassemblerSpew::counter_(0);
#endif

DisassemblerSpew::DisassemblerSpew()
  : printer_(nullptr)
#ifdef JS_DISASM_SUPPORTED
    ,
    labelIndent_(""),
    targetIndent_(""),
    spewNext_(1000),
    nodes_(nullptr),
    tag_(0)
#endif
{
#ifdef JS_DISASM_SUPPORTED
    tag_ = counter_++;
#endif
}

DisassemblerSpew::~DisassemblerSpew()
{
#ifdef JS_DISASM_SUPPORTED
    Node* p = nodes_;
    while (p) {
        Node* victim = p;
        p = p->next;
        js_free(victim);
    }
#endif
}

void
DisassemblerSpew::setPrinter(Sprinter* printer)
{
    printer_ = printer;
}

bool
DisassemblerSpew::isDisabled()
{
    return !(JitSpewEnabled(JitSpew_Codegen) || printer_);
}

void
DisassemblerSpew::spew(const char* fmt, ...)
{
#ifdef JS_DISASM_SUPPORTED
    static const char prefix_chars[] = "0123456789"
                                       "abcdefghijklmnopqrstuvwxyz"
                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char prefix_fmt[] = "[%c] ";

    char fmt2[1024];
    if (sizeof(fmt2) >= strlen(fmt) + sizeof(prefix_fmt)) {
        snprintf(fmt2, sizeof(prefix_fmt), prefix_fmt,
                 prefix_chars[tag_ % (sizeof(prefix_chars) - 1)]);
        strcat(fmt2, fmt);
        fmt = fmt2;
    }
#endif

    va_list args;
    va_start(args, fmt);
    spewVA(fmt, args);
    va_end(args);
}

void
DisassemblerSpew::spewVA(const char* fmt, va_list va)
{
    if (printer_) {
        printer_->vprintf(fmt, va);
        printer_->put("\n");
    }
    js::jit::JitSpewVA(js::jit::JitSpew_Codegen, fmt, va);
}

#ifdef JS_DISASM_SUPPORTED

void
DisassemblerSpew::setLabelIndent(const char* s)
{
    labelIndent_ = s;
}

void
DisassemblerSpew::setTargetIndent(const char* s)
{
    targetIndent_ = s;
}

DisassemblerSpew::LabelDoc
DisassemblerSpew::refLabel(const Label* l)
{
    return l ? LabelDoc(internalResolve(l), l->bound()) : LabelDoc();
}

void
DisassemblerSpew::spewRef(const LabelDoc& target)
{
    if (isDisabled())
        return;
    if (!target.valid)
        return;
    spew("%s-> %d%s", targetIndent_, target.doc, !target.bound ? "f" : "");
}

void
DisassemblerSpew::spewBind(const Label* label)
{
    if (isDisabled())
        return;
    uint32_t v = internalResolve(label);
    Node* probe = lookup(label);
    if (probe)
        probe->bound = true;
    spew("%s%d:", labelIndent_, v);
}

void
DisassemblerSpew::spewRetarget(const Label* label, const Label* target)
{
    if (isDisabled())
        return;
    LabelDoc labelDoc = LabelDoc(internalResolve(label), label->bound());
    LabelDoc targetDoc = LabelDoc(internalResolve(target), target->bound());
    Node* probe = lookup(label);
    if (probe)
        probe->bound = true;
    spew("%s%d: .retarget -> %d%s",
         labelIndent_, labelDoc.doc, targetDoc.doc, !targetDoc.bound ? "f" : "");
}

void
DisassemblerSpew::formatLiteral(const LiteralDoc& doc, char* buffer, size_t bufsize)
{
    switch (doc.type) {
      case LiteralDoc::Type::Patchable:
        snprintf(buffer, bufsize, "patchable");
        break;
      case LiteralDoc::Type::I32:
        snprintf(buffer, bufsize, "%d", doc.value.i32);
        break;
      case LiteralDoc::Type::U32:
        snprintf(buffer, bufsize, "%u", doc.value.u32);
        break;
      case LiteralDoc::Type::I64:
        snprintf(buffer, bufsize, "%" PRIi64, doc.value.i64);
        break;
      case LiteralDoc::Type::U64:
        snprintf(buffer, bufsize, "%" PRIu64, doc.value.u64);
        break;
      case LiteralDoc::Type::F32:
        snprintf(buffer, bufsize, "%g", doc.value.f32);
        break;
      case LiteralDoc::Type::F64:
        snprintf(buffer, bufsize, "%g", doc.value.f64);
        break;
      default:
        MOZ_CRASH();
    }
}

void
DisassemblerSpew::spewOrphans()
{
    for (Node* p = nodes_; p; p = p->next) {
        if (!p->bound)
            spew("%s%d:    ; .orphan", labelIndent_, p->value);
    }
}

uint32_t
DisassemblerSpew::internalResolve(const Label* l)
{
    // Note, internalResolve will sometimes return 0 when it is triggered by the
    // profiler and not by a full disassembly, since in that case a label can be
    // used or bound but not previously have been defined.  In that case,
    // internalResolve(l) will not necessarily create a binding for l!
    // Consequently a subsequent lookup(l) may still return null.
    return l->used() || l->bound() ? probe(l) : define(l);
}

uint32_t
DisassemblerSpew::probe(const Label* l)
{
    Node* n = lookup(l);
    return n ? n->value : 0;
}

uint32_t
DisassemblerSpew::define(const Label* l)
{
    remove(l);
    uint32_t value = spewNext_++;
    if (!add(l, value))
        return 0;
    return value;
}

DisassemblerSpew::Node*
DisassemblerSpew::lookup(const Label* key)
{
    Node* p;
    for (p = nodes_; p && p->key != key; p = p->next)
        ;
    return p;
}

DisassemblerSpew::Node*
DisassemblerSpew::add(const Label* key, uint32_t value)
{
    MOZ_ASSERT(!lookup(key));
    Node* node = (Node*)js_malloc(sizeof(Node));
    if (node) {
        node->key = key;
        node->value = value;
        node->bound = false;
        node->next = nodes_;
        nodes_ = node;
    }
    return node;
}

bool
DisassemblerSpew::remove(const Label* key)
{
    // We do not require that there is a node matching the key.
    for (Node* p = nodes_, *pp = nullptr; p; pp = p, p = p->next) {
        if (p->key == key) {
            if (pp)
                pp->next = p->next;
            else
                nodes_ = p->next;
            js_free(p);
            return true;
        }
    }
    return false;
}

#else

DisassemblerSpew::LabelDoc
DisassemblerSpew::refLabel(const Label* l)
{
    return LabelDoc();
}

#endif

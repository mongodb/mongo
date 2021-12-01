/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_CACHEIR_SPEW

#include "jit/CacheIRSpewer.h"

#include "mozilla/Sprintf.h"

#ifdef XP_WIN
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <stdarg.h>

#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"

#include "vm/JSCompartment-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::jit;

CacheIRSpewer CacheIRSpewer::cacheIRspewer;

CacheIRSpewer::CacheIRSpewer()
  : outputLock(mutexid::CacheIRSpewer)
{ }

CacheIRSpewer::~CacheIRSpewer()
{
    if (!enabled())
        return;

    json.ref().endList();
    output.flush();
    output.finish();
}

#ifndef JIT_SPEW_DIR
# if defined(_WIN32)
#  define JIT_SPEW_DIR "."
# elif defined(__ANDROID__)
#  define JIT_SPEW_DIR "/data/local/tmp"
# else
#  define JIT_SPEW_DIR "/tmp"
# endif
#endif

bool
CacheIRSpewer::init()
{
    if (enabled())
        return true;

    char name[256];
    uint32_t pid = getpid();
    SprintfLiteral(name, JIT_SPEW_DIR "/cacheir%" PRIu32 ".json", pid);

    if (!output.init(name))
        return false;
    output.put("[");

    json.emplace(output);
    return true;
}

void
CacheIRSpewer::beginCache(const IRGenerator& gen)
{
    MOZ_ASSERT(enabled());
    JSONPrinter& j = json.ref();
    const char* filename = gen.script_->filename();
    j.beginObject();
    j.property("name", CacheKindNames[uint8_t(gen.cacheKind_)]);
    j.property("file", filename ? filename : "null");
    j.property("mode", int(gen.mode_));
    if (jsbytecode* pc = gen.pc_) {
        unsigned column;
        j.property("line", PCToLineNumber(gen.script_, pc, &column));
        j.property("column", column);
        j.formatProperty("pc", "%p", pc);
    }
}

template <typename CharT>
static void
QuoteString(GenericPrinter& out, const CharT* s, size_t length)
{
    const CharT* end = s + length;
    for (const CharT* t = s; t < end; s = ++t) {
        // This quote implementation is probably correct,
        // but uses \u even when not strictly necessary.
        char16_t c = *t;
        if (c == '"' || c == '\\') {
            out.printf("\\");
            out.printf("%c", char(c));
        } else if (c < ' ' || c >= 127 || !isprint(c)) {
            out.printf("\\u%04x", c);
        } else {
            out.printf("%c", char(c));
        }
    }
}

static void
QuoteString(GenericPrinter& out, JSLinearString* str)
{
    JS::AutoCheckCannotGC nogc;
    if (str->hasLatin1Chars())
        QuoteString(out, str->latin1Chars(nogc), str->length());
    else
        QuoteString(out, str->twoByteChars(nogc), str->length());
}

void
CacheIRSpewer::valueProperty(const char* name, const Value& v)
{
    MOZ_ASSERT(enabled());
    JSONPrinter& j = json.ref();

    j.beginObjectProperty(name);

    const char* type = InformalValueTypeName(v);
    if (v.isInt32())
        type = "int32";
    j.property("type", type);

    if (v.isInt32()) {
        j.property("value", v.toInt32());
    } else if (v.isDouble()) {
        j.floatProperty("value", v.toDouble(), 3);
    } else if (v.isString() || v.isSymbol()) {
        JSString* str = v.isString() ? v.toString() : v.toSymbol()->description();
        if (str && str->isLinear()) {
            j.beginStringProperty("value");
            QuoteString(output, &str->asLinear());
            j.endStringProperty();
        }
    } else if (v.isObject()) {
        j.formatProperty("value", "%p (shape: %p)", &v.toObject(),
                         v.toObject().maybeShape());
    }

    j.endObject();
}

void
CacheIRSpewer::attached(const char* name)
{
    MOZ_ASSERT(enabled());
    json.ref().property("attached", name);
}

void
CacheIRSpewer::endCache()
{
    MOZ_ASSERT(enabled());
    json.ref().endObject();
}

#endif /* JS_CACHEIR_SPEW */

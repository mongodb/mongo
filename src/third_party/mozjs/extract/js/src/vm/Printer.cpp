/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Printer.h"

#include "mozilla/PodOperations.h"
#include "mozilla/Printf.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#include "jsutil.h"

#include "ds/LifoAlloc.h"
#include "util/Windows.h"
#include "vm/JSContext.h"

using mozilla::PodCopy;

namespace
{

class GenericPrinterPrintfTarget : public mozilla::PrintfTarget
{
public:

    explicit GenericPrinterPrintfTarget(js::GenericPrinter& p)
        : printer(p)
    {
    }

    bool append(const char* sp, size_t len) override {
        return printer.put(sp, len);
    }

private:

    js::GenericPrinter& printer;
};

}

namespace js {

GenericPrinter::GenericPrinter()
  : hadOOM_(false)
{
}

void
GenericPrinter::reportOutOfMemory()
{
    if (hadOOM_)
        return;
    hadOOM_ = true;
}

bool
GenericPrinter::hadOutOfMemory() const
{
    return hadOOM_;
}

bool
GenericPrinter::printf(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    bool r = vprintf(fmt, va);
    va_end(va);
    return r;
}

bool
GenericPrinter::vprintf(const char* fmt, va_list ap)
{
    // Simple shortcut to avoid allocating strings.
    if (strchr(fmt, '%') == nullptr)
        return put(fmt);

    GenericPrinterPrintfTarget printer(*this);
    if (!printer.vprint(fmt, ap)) {
        reportOutOfMemory();
        return false;
    }
    return true;
}

const size_t Sprinter::DefaultSize = 64;

bool
Sprinter::realloc_(size_t newSize)
{
    MOZ_ASSERT(newSize > (size_t) offset);
    char* newBuf = (char*) js_realloc(base, newSize);
    if (!newBuf) {
        reportOutOfMemory();
        return false;
    }
    base = newBuf;
    size = newSize;
    base[size - 1] = 0;
    return true;
}

Sprinter::Sprinter(JSContext* cx, bool shouldReportOOM)
  : context(cx),
#ifdef DEBUG
    initialized(false),
#endif
    shouldReportOOM(shouldReportOOM),
    base(nullptr), size(0), offset(0)
{ }

Sprinter::~Sprinter()
{
#ifdef DEBUG
    if (initialized)
        checkInvariants();
#endif
    js_free(base);
}

bool
Sprinter::init()
{
    MOZ_ASSERT(!initialized);
    base = (char*) js_malloc(DefaultSize);
    if (!base) {
        reportOutOfMemory();
        return false;
    }
#ifdef DEBUG
    initialized = true;
#endif
    *base = 0;
    size = DefaultSize;
    base[size - 1] = 0;
    return true;
}

void
Sprinter::checkInvariants() const
{
    MOZ_ASSERT(initialized);
    MOZ_ASSERT((size_t) offset < size);
    MOZ_ASSERT(base[size - 1] == 0);
}

char*
Sprinter::release()
{
    checkInvariants();
    if (hadOOM_)
        return nullptr;

    char* str = base;
    base = nullptr;
    offset = size = 0;
#ifdef DEBUG
    initialized = false;
#endif
    return str;
}

char*
Sprinter::stringAt(ptrdiff_t off) const
{
    MOZ_ASSERT(off >= 0 && (size_t) off < size);
    return base + off;
}

char&
Sprinter::operator[](size_t off)
{
    MOZ_ASSERT(off < size);
    return *(base + off);
}

char*
Sprinter::reserve(size_t len)
{
    InvariantChecker ic(this);

    while (len + 1 > size - offset) { /* Include trailing \0 */
        if (!realloc_(size * 2))
            return nullptr;
    }

    char* sb = base + offset;
    offset += len;
    return sb;
}

bool
Sprinter::put(const char* s, size_t len)
{
    InvariantChecker ic(this);

    const char* oldBase = base;
    const char* oldEnd = base + size;

    char* bp = reserve(len);
    if (!bp)
        return false;

    /* s is within the buffer already */
    if (s >= oldBase && s < oldEnd) {
        /* buffer was realloc'ed */
        if (base != oldBase)
            s = stringAt(s - oldBase);  /* this is where it lives now */
        memmove(bp, s, len);
    } else {
        js_memcpy(bp, s, len);
    }

    bp[len] = 0;
    return true;
}

bool
Sprinter::putString(JSString* s)
{
    InvariantChecker ic(this);

    size_t length = s->length();
    size_t size = length;

    char* buffer = reserve(size);
    if (!buffer)
        return false;

    JSLinearString* linear = s->ensureLinear(context);
    if (!linear)
        return false;

    JS::AutoCheckCannotGC nogc;
    if (linear->hasLatin1Chars())
        PodCopy(reinterpret_cast<Latin1Char*>(buffer), linear->latin1Chars(nogc), length);
    else
        DeflateStringToBuffer(nullptr, linear->twoByteChars(nogc), length, buffer, &size);

    buffer[size] = 0;
    return true;
}

ptrdiff_t
Sprinter::getOffset() const
{
    return offset;
}

void
Sprinter::reportOutOfMemory()
{
    if (hadOOM_)
        return;
    if (context && shouldReportOOM)
        ReportOutOfMemory(context);
    hadOOM_ = true;
}

bool
Sprinter::jsprintf(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);

    bool r = vprintf(format, ap);
    va_end(ap);

    return r;
}

const char js_EscapeMap[] = {
    '\b', 'b',
    '\f', 'f',
    '\n', 'n',
    '\r', 'r',
    '\t', 't',
    '\v', 'v',
    '"',  '"',
    '\'', '\'',
    '\\', '\\',
    '\0'
};

template <typename CharT>
static char*
QuoteString(Sprinter* sp, const mozilla::Range<const CharT> chars, char16_t quote)
{
    using CharPtr = mozilla::RangedPtr<const CharT>;

    /* Sample off first for later return value pointer computation. */
    ptrdiff_t offset = sp->getOffset();

    if (quote) {
        if (!sp->jsprintf("%c", char(quote)))
            return nullptr;
    }

    const CharPtr end = chars.end();

    /* Loop control variables: end points at end of string sentinel. */
    for (CharPtr t = chars.begin(); t < end; ++t) {
        /* Move t forward from s past un-quote-worthy characters. */
        const CharPtr s = t;
        char16_t c = *t;
        while (c < 127 && isprint(c) && c != quote && c != '\\' && c != '\t') {
            ++t;
            if (t == end)
                break;
            c = *t;
        }

        {
            ptrdiff_t len = t - s;
            ptrdiff_t base = sp->getOffset();
            if (!sp->reserve(len))
                return nullptr;

            for (ptrdiff_t i = 0; i < len; ++i)
                (*sp)[base + i] = char(s[i]);
            (*sp)[base + len] = 0;
        }

        if (t == end)
            break;

        /* Use js_EscapeMap, \u, or \x only if necessary. */
        const char* escape;
        if (!(c >> 8) && c != 0 && (escape = strchr(js_EscapeMap, int(c))) != nullptr) {
            if (!sp->jsprintf("\\%c", escape[1]))
                return nullptr;
        } else {
            /*
             * Use \x only if the high byte is 0 and we're in a quoted string,
             * because ECMA-262 allows only \u, not \x, in Unicode identifiers
             * (see bug 621814).
             */
            if (!sp->jsprintf((quote && !(c >> 8)) ? "\\x%02X" : "\\u%04X", c))
                return nullptr;
        }
    }

    /* Sprint the closing quote and return the quoted string. */
    if (quote) {
        if (!sp->jsprintf("%c", char(quote)))
            return nullptr;
    }

    /*
     * If we haven't Sprint'd anything yet, Sprint an empty string so that
     * the return below gives a valid result.
     */
    if (offset == sp->getOffset()) {
        if (!sp->put(""))
            return nullptr;
    }

    return sp->stringAt(offset);
}

char*
QuoteString(Sprinter* sp, JSString* str, char16_t quote)
{
    JSLinearString* linear = str->ensureLinear(sp->context);
    if (!linear)
        return nullptr;

    JS::AutoCheckCannotGC nogc;
    return linear->hasLatin1Chars()
           ? QuoteString(sp, linear->latin1Range(nogc), quote)
           : QuoteString(sp, linear->twoByteRange(nogc), quote);
}

JSString*
QuoteString(JSContext* cx, JSString* str, char16_t quote)
{
    Sprinter sprinter(cx);
    if (!sprinter.init())
        return nullptr;
    char* bytes = QuoteString(&sprinter, str, quote);
    if (!bytes)
        return nullptr;
    return NewStringCopyZ<CanGC>(cx, bytes);
}

Fprinter::Fprinter(FILE* fp)
  : file_(nullptr),
    init_(false)
{
    init(fp);
}

Fprinter::Fprinter()
  : file_(nullptr),
    init_(false)
{ }

Fprinter::~Fprinter()
{
    MOZ_ASSERT_IF(init_, !file_);
}

bool
Fprinter::init(const char* path)
{
    MOZ_ASSERT(!file_);
    file_ = fopen(path, "w");
    if (!file_)
        return false;
    init_ = true;
    return true;
}

void
Fprinter::init(FILE *fp)
{
    MOZ_ASSERT(!file_);
    file_ = fp;
    init_ = false;
}

void
Fprinter::flush()
{
    MOZ_ASSERT(file_);
    fflush(file_);
}

void
Fprinter::finish()
{
    MOZ_ASSERT(file_);
    if (init_)
        fclose(file_);
    file_ = nullptr;
}

bool
Fprinter::put(const char* s, size_t len)
{
    MOZ_ASSERT(file_);
    int i = fwrite(s, /*size=*/ 1, /*nitems=*/ len, file_);
    if (size_t(i) != len) {
        reportOutOfMemory();
        return false;
    }
#ifdef XP_WIN32
    if ((file_ == stderr) && (IsDebuggerPresent())) {
        UniqueChars buf(static_cast<char*>(js_malloc(len + 1)));
        if (!buf) {
            reportOutOfMemory();
            return false;
        }
        PodCopy(buf.get(), s, len);
        buf[len] = '\0';
        OutputDebugStringA(buf.get());
    }
#endif
    return true;
}

LSprinter::LSprinter(LifoAlloc* lifoAlloc)
  : alloc_(lifoAlloc),
    head_(nullptr),
    tail_(nullptr),
    unused_(0)
{ }

LSprinter::~LSprinter()
{
    // This LSprinter might be allocated as part of the same LifoAlloc, so we
    // should not expect the destructor to be called.
}

void
LSprinter::exportInto(GenericPrinter& out) const
{
    if (!head_)
        return;

    for (Chunk* it = head_; it != tail_; it = it->next)
        out.put(it->chars(), it->length);
    out.put(tail_->chars(), tail_->length - unused_);
}

void
LSprinter::clear()
{
    head_ = nullptr;
    tail_ = nullptr;
    unused_ = 0;
    hadOOM_ = false;
}

bool
LSprinter::put(const char* s, size_t len)
{
    // Compute how much data will fit in the current chunk.
    size_t existingSpaceWrite = 0;
    size_t overflow = len;
    if (unused_ > 0 && tail_) {
        existingSpaceWrite = std::min(unused_, len);
        overflow = len - existingSpaceWrite;
    }

    // If necessary, allocate a new chunk for overflow data.
    size_t allocLength = 0;
    Chunk* last = nullptr;
    if (overflow > 0) {
        allocLength = AlignBytes(sizeof(Chunk) + overflow, js::detail::LIFO_ALLOC_ALIGN);

        LifoAlloc::AutoFallibleScope fallibleAllocator(alloc_);
        last = reinterpret_cast<Chunk*>(alloc_->alloc(allocLength));
        if (!last) {
            reportOutOfMemory();
            return false;
        }
    }

    // All fallible operations complete: now fill up existing space, then
    // overflow space in any new chunk.
    MOZ_ASSERT(existingSpaceWrite + overflow == len);

    if (existingSpaceWrite > 0) {
        PodCopy(tail_->end() - unused_, s, existingSpaceWrite);
        unused_ -= existingSpaceWrite;
        s += existingSpaceWrite;
    }

    if (overflow > 0) {
        if (tail_ && reinterpret_cast<char*>(last) == tail_->end()) {
            // tail_ and last are consecutive in memory.  LifoAlloc has no
            // metadata and is just a bump allocator, so we can cheat by
            // appending the newly-allocated space to tail_.
            unused_ = allocLength;
            tail_->length += allocLength;
        } else {
            // Remove the size of the header from the allocated length.
            size_t availableSpace = allocLength - sizeof(Chunk);
            last->next = nullptr;
            last->length = availableSpace;

            unused_ = availableSpace;
            if (!head_)
                head_ = last;
            else
                tail_->next = last;

            tail_ = last;
        }

        PodCopy(tail_->end() - unused_, s, overflow);

        MOZ_ASSERT(unused_ >= overflow);
        unused_ -= overflow;
    }

    MOZ_ASSERT(len <= INT_MAX);
    return true;
}

} // namespace js

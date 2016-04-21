/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Printer.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#include "jscntxt.h"
#include "jsprf.h"
#include "jsutil.h"

#include "ds/LifoAlloc.h"

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

int
GenericPrinter::put(const char* s)
{
    return put(s, strlen(s));
}

int
GenericPrinter::printf(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int i = vprintf(fmt, va);
    va_end(va);
    return i;
}

int
GenericPrinter::vprintf(const char* fmt, va_list ap)
{
    // Simple shortcut to avoid allocating strings.
    if (strchr(fmt, '%') == nullptr)
        return put(fmt);

    char* bp;
    bp = JS_vsmprintf(fmt, ap);      /* XXX vsaprintf */
    if (!bp) {
        reportOutOfMemory();
        return -1;
    }
    int i = put(bp);
    js_free(bp);
    return i;
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

Sprinter::Sprinter(ExclusiveContext* cx, bool shouldReportOOM)
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

const char*
Sprinter::string() const
{
    return base;
}

const char*
Sprinter::stringEnd() const
{
    return base + offset;
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

int
Sprinter::put(const char* s, size_t len)
{
    InvariantChecker ic(this);

    const char* oldBase = base;
    const char* oldEnd = base + size;

    ptrdiff_t oldOffset = offset;
    char* bp = reserve(len);
    if (!bp)
        return -1;

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
    return oldOffset;
}

int
Sprinter::vprintf(const char* fmt, va_list ap)
{
    InvariantChecker ic(this);

    do {
        va_list aq;
        va_copy(aq, ap);
        int i = JS_vsnprintf(base + offset, size - offset, fmt, aq);
        va_end(aq);
        if (i > -1 && (size_t) i < size - offset) {
            offset += i;
            return i;
        }
    } while (realloc_(size * 2));

    return -1;
}

int
Sprinter::putString(JSString* s)
{
    InvariantChecker ic(this);

    size_t length = s->length();
    size_t size = length;

    ptrdiff_t oldOffset = offset;
    char* buffer = reserve(size);
    if (!buffer)
        return -1;

    JSLinearString* linear = s->ensureLinear(context);
    if (!linear)
        return -1;

    JS::AutoCheckCannotGC nogc;
    if (linear->hasLatin1Chars())
        mozilla::PodCopy(reinterpret_cast<Latin1Char*>(buffer), linear->latin1Chars(nogc), length);
    else
        DeflateStringToBuffer(nullptr, linear->twoByteChars(nogc), length, buffer, &size);

    buffer[size] = 0;
    return oldOffset;
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

ptrdiff_t
Sprint(Sprinter* sp, const char* format, ...)
{
    va_list ap;
    char* bp;
    ptrdiff_t offset;

    va_start(ap, format);
    bp = JS_vsmprintf(format, ap);      /* XXX vsaprintf */
    va_end(ap);
    if (!bp) {
        sp->reportOutOfMemory();
        return -1;
    }
    offset = sp->put(bp);
    js_free(bp);
    return offset;
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
QuoteString(Sprinter* sp, const CharT* s, size_t length, char16_t quote)
{
    /* Sample off first for later return value pointer computation. */
    ptrdiff_t offset = sp->getOffset();

    if (quote && Sprint(sp, "%c", char(quote)) < 0)
        return nullptr;

    const CharT* end = s + length;

    /* Loop control variables: end points at end of string sentinel. */
    for (const CharT* t = s; t < end; s = ++t) {
        /* Move t forward from s past un-quote-worthy characters. */
        char16_t c = *t;
        while (c < 127 && isprint(c) && c != quote && c != '\\' && c != '\t') {
            c = *++t;
            if (t == end)
                break;
        }

        {
            ptrdiff_t len = t - s;
            ptrdiff_t base = sp->getOffset();
            if (!sp->reserve(len))
                return nullptr;

            for (ptrdiff_t i = 0; i < len; ++i)
                (*sp)[base + i] = char(*s++);
            (*sp)[base + len] = 0;
        }

        if (t == end)
            break;

        /* Use js_EscapeMap, \u, or \x only if necessary. */
        const char* escape;
        if (!(c >> 8) && c != 0 && (escape = strchr(js_EscapeMap, int(c))) != nullptr) {
            if (Sprint(sp, "\\%c", escape[1]) < 0)
                return nullptr;
        } else {
            /*
             * Use \x only if the high byte is 0 and we're in a quoted string,
             * because ECMA-262 allows only \u, not \x, in Unicode identifiers
             * (see bug 621814).
             */
            if (Sprint(sp, (quote && !(c >> 8)) ? "\\x%02X" : "\\u%04X", c) < 0)
                return nullptr;
        }
    }

    /* Sprint the closing quote and return the quoted string. */
    if (quote && Sprint(sp, "%c", char(quote)) < 0)
        return nullptr;

    /*
     * If we haven't Sprint'd anything yet, Sprint an empty string so that
     * the return below gives a valid result.
     */
    if (offset == sp->getOffset() && Sprint(sp, "") < 0)
        return nullptr;

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
           ? QuoteString(sp, linear->latin1Chars(nogc), linear->length(), quote)
           : QuoteString(sp, linear->twoByteChars(nogc), linear->length(), quote);
}

JSString*
QuoteString(ExclusiveContext* cx, JSString* str, char16_t quote)
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

int
Fprinter::put(const char* s, size_t len)
{
    MOZ_ASSERT(file_);
    int i = fwrite(s, len, 1, file_);
    if (i == -1 || i != int(len))
        reportOutOfMemory();
    return i;
}

int
Fprinter::put(const char* s)
{
    MOZ_ASSERT(file_);
    int i = fputs(s, file_);
    if (i == -1)
        reportOutOfMemory();
    return i;
}

int
Fprinter::printf(const char* fmt, ...)
{
    MOZ_ASSERT(file_);
    va_list ap;
    va_start(ap, fmt);
    int i = vfprintf(file_, fmt, ap);
    if (i == -1)
        reportOutOfMemory();
    va_end(ap);
    return i;
}

int
Fprinter::vprintf(const char* fmt, va_list ap)
{
    MOZ_ASSERT(file_);
    int i = vfprintf(file_, fmt, ap);
    if (i == -1)
        reportOutOfMemory();
    return i;
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

int
LSprinter::put(const char* s, size_t len)
{
    size_t origLen = len;
    if (unused_ > 0 && tail_) {
        size_t minLen = unused_ < len ? unused_ : len;
        js_memcpy(tail_->end() - unused_, s, minLen);
        unused_ -= minLen;
        len -= minLen;
        s += minLen;
    }

    if (len == 0)
        return origLen;

    size_t allocLength = AlignBytes(sizeof(Chunk) + len, js::detail::LIFO_ALLOC_ALIGN);
    Chunk* last = reinterpret_cast<Chunk*>(alloc_->alloc(allocLength));
    if (!last) {
        reportOutOfMemory();
        return origLen - len;
    }

    if (tail_ && reinterpret_cast<char*>(last) == tail_->end()) {
        // tail_ and last are next to each others in memory, knowing that the
        // TempAlloctator has no meta data and is just a bump allocator, we
        // append the new allocated space to the tail_.
        unused_ = allocLength;
        tail_->length += allocLength;
    } else {
        // Remove the size of the header from the allocated length.
        allocLength -= sizeof(Chunk);
        last->next = nullptr;
        last->length = allocLength;
        unused_ = allocLength;
        if (!head_)
            head_ = last;
        else
            tail_->next = last;

        tail_ = last;
    }

    MOZ_ASSERT(tail_->length >= unused_);
    js_memcpy(tail_->end() - unused_, s, len);
    unused_ -= len;
    return origLen;
}

int
LSprinter::put(const char* s)
{
    return put(s, strlen(s));
}

int
LSprinter::printf(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int i = vprintf(fmt, va);
    va_end(va);
    return i;
}

int
LSprinter::vprintf(const char* fmt, va_list ap)
{
    // Simple shortcut to avoid allocating strings.
    if (strchr(fmt, '%') == nullptr)
        return put(fmt);

    char* bp;
    bp = JS_vsmprintf(fmt, ap);      /* XXX vsaprintf */
    if (!bp) {
        reportOutOfMemory();
        return -1;
    }
    int i = put(bp);
    js_free(bp);
    return i;
}

void
LSprinter::reportOutOfMemory()
{
    if (hadOOM_)
        return;
    hadOOM_ = true;
}

bool
LSprinter::hadOutOfMemory() const
{
    return hadOOM_;
}

} // namespace js

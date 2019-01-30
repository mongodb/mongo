/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Printer_h
#define vm_Printer_h

#include "mozilla/Attributes.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "js/TypeDecls.h"

namespace js {

class LifoAlloc;

// Generic printf interface, similar to an ostream in the standard library.
//
// This class is useful to make generic printers which can work either with a
// file backend, with a buffer allocated with an JSContext or a link-list
// of chunks allocated with a LifoAlloc.
class GenericPrinter
{
  protected:
    bool                  hadOOM_;     // whether reportOutOfMemory() has been called.

    GenericPrinter();

  public:
    // Puts |len| characters from |s| at the current position and
    // return true on success, false on failure.
    virtual bool put(const char* s, size_t len) = 0;
    virtual void flush() { /* Do nothing */ }

    inline bool put(const char* s) {
        return put(s, strlen(s));
    }
    inline bool putChar(const char c) {
        return put(&c, 1);
    }

    // Prints a formatted string into the buffer.
    bool printf(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3);
    bool vprintf(const char* fmt, va_list ap) MOZ_FORMAT_PRINTF(2, 0);

    // Report that a string operation failed to get the memory it requested.
    virtual void reportOutOfMemory();

    // Return true if this Sprinter ran out of memory.
    virtual bool hadOutOfMemory() const;
};

// Sprintf, but with unlimited and automatically allocated buffering.
class Sprinter final : public GenericPrinter
{
  public:
    struct InvariantChecker
    {
        const Sprinter* parent;

        explicit InvariantChecker(const Sprinter* p) : parent(p) {
            parent->checkInvariants();
        }

        ~InvariantChecker() {
            parent->checkInvariants();
        }
    };

    JSContext*            context;          // context executing the decompiler

  private:
    static const size_t   DefaultSize;
#ifdef DEBUG
    bool                  initialized;      // true if this is initialized, use for debug builds
#endif
    bool                  shouldReportOOM;  // whether to report OOM to the context
    char*                 base;             // malloc'd buffer address
    size_t                size;             // size of buffer allocated at base
    ptrdiff_t             offset;           // offset of next free char in buffer

    MOZ_MUST_USE bool realloc_(size_t newSize);

  public:
    explicit Sprinter(JSContext* cx, bool shouldReportOOM = true);
    ~Sprinter();

    // Initialize this sprinter, returns false on error.
    MOZ_MUST_USE bool init();

    void checkInvariants() const;

    const char* string() const { return base; }
    const char* stringEnd() const { return base + offset; }
    char* release();

    // Returns the string at offset |off|.
    char* stringAt(ptrdiff_t off) const;
    // Returns the char at offset |off|.
    char& operator[](size_t off);

    // Attempt to reserve len + 1 space (for a trailing nullptr byte). If the
    // attempt succeeds, return a pointer to the start of that space and adjust the
    // internal content. The caller *must* completely fill this space on success.
    char* reserve(size_t len);

    // Puts |len| characters from |s| at the current position and
    // return true on success, false on failure.
    virtual bool put(const char* s, size_t len) override;
    using GenericPrinter::put; // pick up |inline bool put(const char* s);|

    // Format the given format/arguments as if by JS_vsmprintf, then put it.
    // Return true on success, else return false and report an error (typically
    // OOM).
    MOZ_MUST_USE bool jsprintf(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3);

    bool putString(JSString* str);

    ptrdiff_t getOffset() const;

    // Report that a string operation failed to get the memory it requested. The
    // first call to this function calls JS_ReportOutOfMemory, and sets this
    // Sprinter's outOfMemory flag; subsequent calls do nothing.
    virtual void reportOutOfMemory() override;
};

// Fprinter, print a string directly into a file.
class Fprinter final : public GenericPrinter
{
  private:
    FILE*                   file_;
    bool                    init_;

  public:
    explicit Fprinter(FILE* fp);
    Fprinter();
    ~Fprinter();

    // Initialize this printer, returns false on error.
    MOZ_MUST_USE bool init(const char* path);
    void init(FILE* fp);
    bool isInitialized() const {
        return file_ != nullptr;
    }
    void flush() override;
    void finish();

    // Puts |len| characters from |s| at the current position and
    // return true on success, false on failure.
    virtual bool put(const char* s, size_t len) override;
    using GenericPrinter::put; // pick up |inline bool put(const char* s);|
};

// LSprinter, is similar to Sprinter except that instead of using an
// JSContext to allocate strings, it use a LifoAlloc as a backend for the
// allocation of the chunk of the string.
class LSprinter final : public GenericPrinter
{
  private:
    struct Chunk
    {
        Chunk* next;
        size_t length;

        char* chars() {
            return reinterpret_cast<char*>(this + 1);
        }
        char* end() {
            return chars() + length;
        }
    };

  private:
    LifoAlloc*              alloc_;          // LifoAlloc used as a backend of chunk allocations.
    Chunk*                  head_;
    Chunk*                  tail_;
    size_t                  unused_;

  public:
    explicit LSprinter(LifoAlloc* lifoAlloc);
    ~LSprinter();

    // Copy the content of the chunks into another printer, such that we can
    // flush the content of this printer to a file.
    void exportInto(GenericPrinter& out) const;

    // Drop the current string, and let them be free with the LifoAlloc.
    void clear();

    // Puts |len| characters from |s| at the current position and
    // return true on success, false on failure.
    virtual bool put(const char* s, size_t len) override;
    using GenericPrinter::put; // pick up |inline bool put(const char* s);|
};

// Map escaped code to the letter/symbol escaped with a backslash.
extern const char       js_EscapeMap[];

// Return a GC'ed string containing the chars in str, with any non-printing
// chars or quotes (' or " as specified by the quote argument) escaped, and
// with the quote character at the beginning and end of the result string.
extern JSString*
QuoteString(JSContext* cx, JSString* str, char16_t quote);

extern char*
QuoteString(Sprinter* sp, JSString* str, char16_t quote);


} // namespace js

#endif // vm_Printer_h

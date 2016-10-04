/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Printer_h
#define vm_Printer_h

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

class JSString;

namespace js {

class ExclusiveContext;
class LifoAlloc;

// Generic printf interface, similar to an ostream in the standard library.
//
// This class is useful to make generic printers which can work either with a
// file backend, with a buffer allocated with an ExclusiveContext or a link-list
// of chunks allocated with a LifoAlloc.
class GenericPrinter
{
  protected:
    bool                  hadOOM_;     // whether reportOutOfMemory() has been called.

    GenericPrinter();

  public:
    // Puts |len| characters from |s| at the current position and return an offset to
    // the beginning of this new data.
    virtual int put(const char* s, size_t len) = 0;
    virtual int put(const char* s);

    // Prints a formatted string into the buffer.
    virtual int printf(const char* fmt, ...);
    virtual int vprintf(const char* fmt, va_list ap);

    // Report that a string operation failed to get the memory it requested. The
    // first call to this function calls JS_ReportOutOfMemory, and sets this
    // Sprinter's outOfMemory flag; subsequent calls do nothing.
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

    ExclusiveContext*     context;          // context executing the decompiler

  private:
    static const size_t   DefaultSize;
#ifdef DEBUG
    bool                  initialized;      // true if this is initialized, use for debug builds
#endif
    bool                  shouldReportOOM;  // whether to report OOM to the context
    char*                 base;             // malloc'd buffer address
    size_t                size;             // size of buffer allocated at base
    ptrdiff_t             offset;           // offset of next free char in buffer

    bool realloc_(size_t newSize);

  public:
    explicit Sprinter(ExclusiveContext* cx, bool shouldReportOOM = true);
    ~Sprinter();

    // Initialize this sprinter, returns false on error.
    bool init();

    void checkInvariants() const;

    const char* string() const;
    const char* stringEnd() const;
    // Returns the string at offset |off|.
    char* stringAt(ptrdiff_t off) const;
    // Returns the char at offset |off|.
    char& operator[](size_t off);

    // Attempt to reserve len + 1 space (for a trailing nullptr byte). If the
    // attempt succeeds, return a pointer to the start of that space and adjust the
    // internal content. The caller *must* completely fill this space on success.
    char* reserve(size_t len);

    // Puts |len| characters from |s| at the current position and return an offset to
    // the beginning of this new data.
    using GenericPrinter::put;
    virtual int put(const char* s, size_t len) override;

    // Prints a formatted string into the buffer.
    virtual int vprintf(const char* fmt, va_list ap) override;

    int putString(JSString* str);

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
    bool init(const char* path);
    void init(FILE* fp);
    bool isInitialized() const {
        return file_ != nullptr;
    }
    void flush();
    void finish();

    // Puts |len| characters from |s| at the current position and return an
    // offset to the beginning of this new data.
    virtual int put(const char* s, size_t len) override;
    virtual int put(const char* s) override;

    // Prints a formatted string into the buffer.
    virtual int printf(const char* fmt, ...) override;
    virtual int vprintf(const char* fmt, va_list ap) override;
};

// LSprinter, is similar to Sprinter except that instead of using an
// ExclusiveContext to allocate strings, it use a LifoAlloc as a backend for the
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

    // Puts |len| characters from |s| at the current position and return an
    // offset to the beginning of this new data.
    virtual int put(const char* s, size_t len) override;
    virtual int put(const char* s) override;

    // Prints a formatted string into the buffer.
    virtual int printf(const char* fmt, ...) override;
    virtual int vprintf(const char* fmt, va_list ap) override;

    // Report that a string operation failed to get the memory it requested. The
    // first call to this function calls JS_ReportOutOfMemory, and sets this
    // Sprinter's outOfMemory flag; subsequent calls do nothing.
    virtual void reportOutOfMemory() override;

    // Return true if this Sprinter ran out of memory.
    virtual bool hadOutOfMemory() const override;
};

extern ptrdiff_t
Sprint(Sprinter* sp, const char* format, ...);

// Map escaped code to the letter/symbol escaped with a backslash.
extern const char       js_EscapeMap[];

// Return a GC'ed string containing the chars in str, with any non-printing
// chars or quotes (' or " as specified by the quote argument) escaped, and
// with the quote character at the beginning and end of the result string.
extern JSString*
QuoteString(ExclusiveContext* cx, JSString* str, char16_t quote);

extern char*
QuoteString(Sprinter* sp, JSString* str, char16_t quote);


} // namespace js

#endif // vm_Printer_h

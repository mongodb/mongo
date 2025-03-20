/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Printer_h
#define js_Printer_h

#include "mozilla/Attributes.h"
#include "mozilla/glue/Debug.h"
#include "mozilla/Range.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "js/TypeDecls.h"
#include "js/Utility.h"

// [SMDOC] *Printer, Sprinter, Fprinter, ...
//
// # Motivation
//
// In many places, we want to have functions which are capable of logging
// various data structures. Previously, we had logging functions for each
// storage, such as using `fwrite`, `printf` or `snprintf`. In additional cases,
// many of these logging options were using a string serializing logging
// function, only to discard the allocated string after it had been copied to a
// file.
//
// GenericPrinter is an answer to avoid excessive amount of temporary
// allocations which are used once, and a way to make logging functions work
// independently of the backend they are used with.
//
// # Design
//
// The GenericPrinter implements most of `put`, `printf`, `vprintf` and
// `putChar` functions, which are implemented using `put` and `putChar`
// functions in the derivative classes. Thus, one does not have to reimplement
// `putString` nor `printf` for each printer.
//
//   // Logging the value N to whatever printer is provided such as
//   // a file or a string.
//   void logN(GenericPrinter& out) {
//     out.printf("[Logging] %d\n", this->n);
//   }
//
// The printing functions are infallible, from the logging functions
// perspective. If an issue happens while printing, this would be recorded by
// the Printer, and this can be tested using `hadOutOfMemory` function by the
// owner of the Printer instance.
//
// Even in case of failure, printing functions should remain safe to use. Thus
// calling `put` twice in a row is safe even if no check for `hadOutOfMemory` is
// performed. This is necessary to simplify the control flow and avoid bubble up
// failures out of logging functions.
//
// Note, being safe to use does not imply correctness. In case of failure the
// correctness of the printed characters is no longer guarantee. One should use
// `hadOutOfMemory` function to know if any failure happened which might have
// caused incorrect content to be saved. In some cases, such as `Sprinter`,
// where the string buffer can be extracted, the returned value would account
// for checking `hadOutOfMemory`.
//
// # Implementations
//
// The GenericPrinter is a base class where the derivative classes are providing
// different implementations which have their own advantages and disadvantages:
//
//  - Fprinter: FILE* printer. Write the content directly to a file.
//
//  - Sprinter: System allocator C-string buffer. Write the content to a buffer
//    which is reallocated as more content is added. The buffer can then be
//    extracted into a C-string or a JSString, respectively using `release` and
//    `releaseJS`.
//
//  - LSprinter: LifoAlloc C-string rope. Write the content to a list of chunks
//    in a LifoAlloc buffer, no-reallocation occur but one should use
//    `exportInto` to serialize its content to a Sprinter or a Fprinter. This is
//    useful to avoid reallocation copies, while using an existing LifoAlloc.
//
//  - SEPrinter: Roughly the same as Fprinter for stderr, except it goes through
//    printf_stderr, which makes sure the output goes to a useful place: the
//    Android log or the Windows debug output.
//
//  - EscapePrinter: Wrapper around other printers, to escape characters when
//    necessary.
//
// # Print UTF-16
//
// The GenericPrinter only handle `char` inputs, which is good enough for ASCII
// and Latin1 character sets. However, to handle UTF-16, one should use an
// EscapePrinter as well as a policy for escaping characters.
//
// One might require different escaping policies based on the escape sequences
// and based on the set of accepted character for the content generated. For
// example, JSON does not specify \x<XX> escape sequences.
//
// Today the following escape policies exists:
//
//  - StringEscape: Produce C-like escape sequences: \<c>, \x<XX> and \u<XXXX>.
//  - JSONEscape: Produce JSON escape sequences: \<c> and \u<XXXX>.
//
// An escape policy is defined by 2 functions:
//
//   bool isSafeChar(char16_t c):
//     Returns whether a character can be printed without being escaped.
//
//   void convertInto(GenericPrinter& out, char16_t c):
//     Calls the printer with the escape sequence for the character given as
//     argument.
//
// To use an escape policy, the printer should be wrapped using an EscapePrinter
// as follows:
//
//   {
//     // The escaped string is surrounded by double-quotes, escape the double
//     // quotes as well.
//     StringEscape esc('"');
//
//     // Wrap our existing `GenericPrinter& out` using the `EscapePrinter`.
//     EscapePrinter ep(out, esc);
//
//     // Append a sequence of characters which might contain UTF-16 characters.
//     ep.put(chars);
//   }
//

namespace js {

class LifoAlloc;

// Generic printf interface, similar to an ostream in the standard library.
//
// This class is useful to make generic printers which can work either with a
// file backend, with a buffer allocated with an JSContext or a link-list
// of chunks allocated with a LifoAlloc.
class JS_PUBLIC_API GenericPrinter {
 protected:
  bool hadOOM_;  // whether reportOutOfMemory() has been called.

  constexpr GenericPrinter() : hadOOM_(false) {}

 public:
  // Puts |len| characters from |s| at the current position. This function might
  // silently fail and the error can be tested using `hadOutOfMemory()`. Calling
  // this function or any other printing functions after a failures is accepted,
  // but the outcome would still remain incorrect and `hadOutOfMemory()` would
  // still report any of the previous errors.
  virtual void put(const char* s, size_t len) = 0;
  inline void put(const char* s) { put(s, strlen(s)); }

  // Put a mozilla::Span / mozilla::Range of Latin1Char or char16_t characters
  // in the output.
  //
  // Note that the char16_t variant is expected to crash unless putChar is
  // overriden to handle properly the full set of WTF-16 character set.
  virtual void put(mozilla::Span<const JS::Latin1Char> str);
  virtual void put(mozilla::Span<const char16_t> str);

  // Same as the various put function but only appending a single character.
  //
  // Note that the char16_t variant is expected to crash unless putChar is
  // overriden to handle properly the full set of WTF-16 character set.
  virtual inline void putChar(const char c) { put(&c, 1); }
  virtual inline void putChar(const JS::Latin1Char c) { putChar(char(c)); }
  virtual inline void putChar(const char16_t c) {
    MOZ_CRASH("Use an EscapePrinter to handle all characters");
  }

  virtual void putString(JSContext* cx, JSString* str);

  // Prints a formatted string into the buffer.
  void printf(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3);
  void vprintf(const char* fmt, va_list ap) MOZ_FORMAT_PRINTF(2, 0);

  // In some cases, such as handling JSRopes in a less-quadratic worse-case,
  // it might be useful to copy content which has already been generated.
  //
  // If the buffer is back-readable, then this function should return `true`
  // and `putFromIndex` should be implemented to delegate to a `put` call at
  // the matching index and the corresponding length. To provide the index
  // argument of `putFromIndex`, the `index` method should also be implemented
  // to return the index within the inner buffer used by the printer.
  virtual bool canPutFromIndex() const { return false; }

  // Append to the current buffer, bytes which have previously been appended
  // before.
  virtual void putFromIndex(size_t index, size_t length) {
    MOZ_CRASH("Calls to putFromIndex should be guarded by canPutFromIndex.");
  }

  // When the printer has a seekable buffer and `canPutFromIndex` returns
  // `true`, this function can return the `index` of the next character to be
  // added to the buffer.
  //
  // This function is monotonic. Thus, if the printer encounter an
  // Out-Of-Memory issue, then the returned index should be the maximal value
  // ever returned.
  virtual size_t index() const { return 0; }

  // In some printers, this ensure that the content is fully written.
  virtual void flush() { /* Do nothing */
  }

  // Report that a string operation failed to get the memory it requested.
  virtual void reportOutOfMemory();

  // Return true if this Sprinter ran out of memory.
  virtual bool hadOutOfMemory() const { return hadOOM_; }
};

// Sprintf / JSSprintf, but with unlimited and automatically allocated
// buffering.
class JS_PUBLIC_API StringPrinter : public GenericPrinter {
 public:
  // Check that the invariant holds at the entry and exit of a scope.
  struct InvariantChecker {
    const StringPrinter* parent;

    explicit InvariantChecker(const StringPrinter* p) : parent(p) {
      parent->checkInvariants();
    }

    ~InvariantChecker() { parent->checkInvariants(); }
  };

  JSContext* maybeCx;

 private:
  static const size_t DefaultSize;
#ifdef DEBUG
  bool initialized;  // true if this is initialized, use for debug builds
#endif
  bool shouldReportOOM;  // whether to report OOM to the maybeCx
  char* base;            // malloc'd buffer address
  size_t size;           // size of buffer allocated at base
  ptrdiff_t offset;      // offset of next free char in buffer

  // The arena to be used by jemalloc to allocate the string into. This is
  // selected by the child classes when calling the constructor. JSStrings have
  // a different arena than strings which do not belong to the JS engine, and as
  // such when building a JSString with the intent of avoiding reallocation, the
  // destination arena has to be selected upfront.
  arena_id_t arena;

 private:
  [[nodiscard]] bool realloc_(size_t newSize);

 protected:
  // JSContext* parameter is optional and can be omitted if the following
  // are not used.
  //   * putString method with JSString
  //   * QuoteString function with JSString
  //   * JSONQuoteString function with JSString
  //
  // If JSContext* parameter is not provided, or shouldReportOOM is false,
  // the consumer should manually report OOM on any failure.
  explicit StringPrinter(arena_id_t arena, JSContext* maybeCx = nullptr,
                         bool shouldReportOOM = true);
  ~StringPrinter();

  JS::UniqueChars releaseChars();
  JSString* releaseJS(JSContext* cx);

 public:
  // Initialize this sprinter, returns false on error.
  [[nodiscard]] bool init();

  void checkInvariants() const;

  // Attempt to reserve len + 1 space (for a trailing nullptr byte). If the
  // attempt succeeds, return a pointer to the start of that space and adjust
  // the internal content. The caller *must* completely fill this space on
  // success.
  char* reserve(size_t len);

  // Puts |len| characters from |s| at the current position. May OOM, which must
  // be checked by testing the return value of releaseJS() at the end of
  // printing.
  virtual void put(const char* s, size_t len) final;
  using GenericPrinter::put;  // pick up |put(const char* s);|

  virtual bool canPutFromIndex() const final { return true; }
  virtual void putFromIndex(size_t index, size_t length) final {
    MOZ_ASSERT(index <= this->index());
    MOZ_ASSERT(index + length <= this->index());
    put(base + index, length);
  }
  virtual size_t index() const final { return length(); }

  virtual void putString(JSContext* cx, JSString* str) final;

  size_t length() const;

  // When an OOM has already been reported on the Sprinter, this function will
  // forward this error to the JSContext given in the Sprinter initialization.
  //
  // If no JSContext had been provided or the Sprinter is configured to not
  // report OOM, then nothing happens.
  void forwardOutOfMemory();
};

class JS_PUBLIC_API Sprinter : public StringPrinter {
 public:
  explicit Sprinter(JSContext* maybeCx = nullptr, bool shouldReportOOM = true)
      : StringPrinter(js::MallocArena, maybeCx, shouldReportOOM) {}
  ~Sprinter() {}

  JS::UniqueChars release() { return releaseChars(); }
};

class JS_PUBLIC_API JSSprinter : public StringPrinter {
 public:
  explicit JSSprinter(JSContext* cx)
      : StringPrinter(js::StringBufferArena, cx, true) {}
  ~JSSprinter() {}

  JSString* release(JSContext* cx) { return releaseJS(cx); }
};

// Fprinter, print a string directly into a file.
class JS_PUBLIC_API Fprinter final : public GenericPrinter {
 private:
  FILE* file_;
  bool init_;

 public:
  explicit Fprinter(FILE* fp);

  constexpr Fprinter() : file_(nullptr), init_(false) {}

#ifdef DEBUG
  ~Fprinter();
#endif

  // Initialize this printer, returns false on error.
  [[nodiscard]] bool init(const char* path);
  void init(FILE* fp);
  bool isInitialized() const { return file_ != nullptr; }
  void flush() override;
  void finish();

  // Puts |len| characters from |s| at the current position. Errors may be
  // detected with hadOutOfMemory() (which will be set for any fwrite() error,
  // not just OOM.)
  void put(const char* s, size_t len) override;
  using GenericPrinter::put;  // pick up |put(const char* s);|
};

// SEprinter, print using printf_stderr (goes to Android log, Windows debug,
// else just stderr).
class SEprinter final : public GenericPrinter {
 public:
  constexpr SEprinter() {}

  // Puts |len| characters from |s| at the current position. Ignores errors.
  virtual void put(const char* s, size_t len) override {
    printf_stderr("%.*s", int(len), s);
  }
  using GenericPrinter::put;  // pick up |put(const char* s);|
};

// LSprinter, is similar to Sprinter except that instead of using an
// JSContext to allocate strings, it use a LifoAlloc as a backend for the
// allocation of the chunk of the string.
class JS_PUBLIC_API LSprinter final : public GenericPrinter {
 private:
  struct Chunk {
    Chunk* next;
    size_t length;

    char* chars() { return reinterpret_cast<char*>(this + 1); }
    char* end() { return chars() + length; }
  };

 private:
  LifoAlloc* alloc_;  // LifoAlloc used as a backend of chunk allocations.
  Chunk* head_;
  Chunk* tail_;
  size_t unused_;

 public:
  explicit LSprinter(LifoAlloc* lifoAlloc);
  ~LSprinter();

  // Copy the content of the chunks into another printer, such that we can
  // flush the content of this printer to a file.
  void exportInto(GenericPrinter& out) const;

  // Drop the current string, and let them be free with the LifoAlloc.
  void clear();

  // Puts |len| characters from |s| at the current position.
  virtual void put(const char* s, size_t len) override;
  using GenericPrinter::put;  // pick up |put(const char* s);|
};

// Escaping printers work like any other printer except that any added character
// are checked for escaping sequences. This one would escape a string such that
// it can safely be embedded in a JS string.
template <typename Delegate, typename Escape>
class JS_PUBLIC_API EscapePrinter final : public GenericPrinter {
  size_t lengthOfSafeChars(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (!esc.isSafeChar(uint8_t(s[i]))) {
        return i;
      }
    }
    return len;
  }

 private:
  Delegate& out;
  Escape& esc;

 public:
  EscapePrinter(Delegate& out, Escape& esc) : out(out), esc(esc) {}
  ~EscapePrinter() {}

  using GenericPrinter::put;
  void put(const char* s, size_t len) override {
    const char* b = s;
    while (len) {
      size_t index = lengthOfSafeChars(b, len);
      if (index) {
        out.put(b, index);
        len -= index;
        b += index;
      }
      if (len) {
        esc.convertInto(out, char16_t(uint8_t(*b)));
        len -= 1;
        b += 1;
      }
    }
  }

  inline void putChar(const char c) override {
    if (esc.isSafeChar(char16_t(uint8_t(c)))) {
      out.putChar(char(c));
      return;
    }
    esc.convertInto(out, char16_t(uint8_t(c)));
  }

  inline void putChar(const JS::Latin1Char c) override {
    if (esc.isSafeChar(char16_t(c))) {
      out.putChar(char(c));
      return;
    }
    esc.convertInto(out, char16_t(c));
  }

  inline void putChar(const char16_t c) override {
    if (esc.isSafeChar(c)) {
      out.putChar(char(c));
      return;
    }
    esc.convertInto(out, c);
  }

  // Forward calls to delegated printer.
  bool canPutFromIndex() const override { return out.canPutFromIndex(); }
  void putFromIndex(size_t index, size_t length) final {
    out.putFromIndex(index, length);
  }
  size_t index() const final { return out.index(); }
  void flush() final { out.flush(); }
  void reportOutOfMemory() final { out.reportOutOfMemory(); }
  bool hadOutOfMemory() const final { return out.hadOutOfMemory(); }
};

class JS_PUBLIC_API JSONEscape {
 public:
  bool isSafeChar(char16_t c);
  void convertInto(GenericPrinter& out, char16_t c);
};

class JS_PUBLIC_API StringEscape {
 private:
  const char quote = '\0';

 public:
  explicit StringEscape(const char quote = '\0') : quote(quote) {}

  bool isSafeChar(char16_t c);
  void convertInto(GenericPrinter& out, char16_t c);
};

// A GenericPrinter that formats everything at a nested indentation level.
class JS_PUBLIC_API IndentedPrinter final : public GenericPrinter {
  GenericPrinter& out_;
  // The number of indents to insert at the beginning of each line.
  uint32_t indentLevel_;
  // The number of spaces to insert for each indent.
  uint32_t indentAmount_;
  // Whether we have seen a line ending and should insert an indent at the
  // next line fragment.
  bool pendingIndent_;

  // Put an indent to `out_`
  void putIndent();
  // Put `s` to `out_`, inserting an indent if we need to
  void putWithMaybeIndent(const char* s, size_t len);

 public:
  explicit IndentedPrinter(GenericPrinter& out, uint32_t indentLevel = 0,
                           uint32_t indentAmount = 2)
      : out_(out),
        indentLevel_(indentLevel),
        indentAmount_(indentAmount),
        pendingIndent_(false) {}

  // Automatically insert and remove and indent for a scope
  class AutoIndent {
    IndentedPrinter& printer_;

   public:
    explicit AutoIndent(IndentedPrinter& printer) : printer_(printer) {
      printer_.setIndentLevel(printer_.indentLevel() + 1);
    }
    ~AutoIndent() { printer_.setIndentLevel(printer_.indentLevel() - 1); }
  };

  uint32_t indentLevel() const { return indentLevel_; }
  void setIndentLevel(uint32_t indentLevel) { indentLevel_ = indentLevel; }

  virtual void put(const char* s, size_t len) override;
  using GenericPrinter::put;  // pick up |inline void put(const char* s);|
};

// Map escaped code to the letter/symbol escaped with a backslash.
extern const char js_EscapeMap[];

// Return a C-string containing the chars in str, with any non-printing chars
// escaped. If the optional quote parameter is present and is not '\0', quotes
// (as specified by the quote argument) are also escaped, and the quote
// character is appended at the beginning and end of the result string.
// The returned string is guaranteed to contain only ASCII characters.
extern JS_PUBLIC_API JS::UniqueChars QuoteString(JSContext* cx, JSString* str,
                                                 char quote = '\0');

// Appends the quoted string to the given Sprinter. Follows the same semantics
// as QuoteString from above.
extern JS_PUBLIC_API void QuoteString(Sprinter* sp, JSString* str,
                                      char quote = '\0');

// Appends the quoted string to the given Sprinter. Follows the same
// Appends the JSON quoted string to the given Sprinter.
extern JS_PUBLIC_API void JSONQuoteString(StringPrinter* sp, JSString* str);

// Internal implementation code for QuoteString methods above.
enum class QuoteTarget { String, JSON };

template <QuoteTarget target, typename CharT>
void JS_PUBLIC_API QuoteString(Sprinter* sp,
                               const mozilla::Range<const CharT>& chars,
                               char quote = '\0');

}  // namespace js

#endif  // js_Printer_h

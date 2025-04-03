/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SourceNotes_h
#define frontend_SourceNotes_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <algorithm>  // std::min
#include <stddef.h>   // ptrdiff_t, size_t
#include <stdint.h>   // int8_t, uint8_t, uint32_t

#include "jstypes.h"  // js::{Bit, BitMask}

namespace js {

/*
 * Source notes generated along with bytecode for decompiling and debugging.
 * A source note is a uint8_t with 4 bits of type and 4 of offset from the pc
 * of the previous note. If 4 bits of offset aren't enough, extended delta
 * notes (XDelta) consisting of 1 set high order bit followed by 7 offset
 * bits are emitted before the next note. Some notes have operand offsets
 * encoded immediately after them, in note bytes or byte-triples.
 *
 *                 Source Note               Extended Delta
 *              +7-6-5-4+3-2-1-0+           +7+6-5-4-3-2-1-0+
 *              | type  | delta |           |1| ext-delta   |
 *              +-------+-------+           +-+-------------+
 *
 * At most one "gettable" note (i.e., a note of type other than NewLine,
 * ColSpan, SetLine, and XDelta) applies to a given bytecode.
 *
 * NB: the js::SrcNote::specs_ array is indexed by this enum, so its
 * initializers need to match the order here.
 */

#define FOR_EACH_SRC_NOTE_TYPE(M)                                            \
  /* Terminates a note vector. */                                            \
  M(Null, "null", 0)                                                         \
  /* += or another assign-op follows. */                                     \
  M(AssignOp, "assignop", 0)                                                 \
  /* All notes above here are "gettable".  See SrcNote::isGettable below. */ \
  M(ColSpan, "colspan", int8_t(SrcNote::ColSpan::Operands::Count))           \
  /* Bytecode follows a source newline. */                                   \
  M(NewLine, "newline", 0)                                                   \
  M(SetLine, "setline", int8_t(SrcNote::SetLine::Operands::Count))           \
  /* Bytecode is a recommended breakpoint. */                                \
  M(Breakpoint, "breakpoint", 0)                                             \
  /* Bytecode is the first in a new steppable area. */                       \
  M(StepSep, "step-sep", 0)                                                  \
  M(Unused7, "unused", 0)                                                    \
  /* 8-15 (0b1xxx) are for extended delta notes. */                          \
  M(XDelta, "xdelta", 0)

// Note: need to add a new source note? If there's no Unused* note left,
// consider bumping SrcNoteType::XDelta to 12-15 and change
// SrcNote::XDeltaBits from 7 to 6.

enum class SrcNoteType : uint8_t {
#define DEFINE_SRC_NOTE_TYPE(sym, name, arity) sym,
  FOR_EACH_SRC_NOTE_TYPE(DEFINE_SRC_NOTE_TYPE)
#undef DEFINE_SRC_NOTE_TYPE

      Last,
  LastGettable = AssignOp
};

static_assert(uint8_t(SrcNoteType::XDelta) == 8, "XDelta should be 8");

class SrcNote {
  struct Spec {
    const char* name_;
    int8_t arity_;
  };

  static const Spec specs_[];

  static constexpr unsigned TypeBits = 4;
  static constexpr unsigned DeltaBits = 4;
  static constexpr unsigned XDeltaBits = 7;

  static constexpr uint8_t TypeMask = js::BitMask(TypeBits) << DeltaBits;
  static constexpr ptrdiff_t DeltaMask = js::BitMask(DeltaBits);
  static constexpr ptrdiff_t XDeltaMask = js::BitMask(XDeltaBits);

  static constexpr ptrdiff_t DeltaLimit = js::Bit(DeltaBits);
  static constexpr ptrdiff_t XDeltaLimit = js::Bit(XDeltaBits);

  static constexpr inline uint8_t toShiftedTypeBits(SrcNoteType type) {
    return (uint8_t(type) << DeltaBits);
  }

  static inline uint8_t noteValue(SrcNoteType type, ptrdiff_t delta) {
    MOZ_ASSERT((delta & DeltaMask) == delta);
    return noteValueUnchecked(type, delta);
  }

  static constexpr inline uint8_t noteValueUnchecked(SrcNoteType type,
                                                     ptrdiff_t delta) {
    return toShiftedTypeBits(type) | (delta & DeltaMask);
  }

  static inline uint8_t xDeltaValue(ptrdiff_t delta) {
    return toShiftedTypeBits(SrcNoteType::XDelta) | (delta & XDeltaMask);
  }

  uint8_t value_;

  constexpr explicit SrcNote(uint8_t value) : value_(value) {}

 public:
  constexpr SrcNote() : value_(noteValueUnchecked(SrcNoteType::Null, 0)){};

  SrcNote(const SrcNote& other) = default;
  SrcNote& operator=(const SrcNote& other) = default;

  SrcNote(SrcNote&& other) = default;
  SrcNote& operator=(SrcNote&& other) = default;

  static constexpr SrcNote terminator() { return SrcNote(); }

 private:
  inline uint8_t typeBits() const { return (value_ >> DeltaBits); }

  inline bool isXDelta() const {
    return typeBits() >= uint8_t(SrcNoteType::XDelta);
  }

  inline bool isFourBytesOperand() const {
    return value_ & FourBytesOperandFlag;
  }

  // number of operands
  inline unsigned arity() const {
    MOZ_ASSERT(uint8_t(type()) < uint8_t(SrcNoteType::Last));
    return specs_[uint8_t(type())].arity_;
  }

 public:
  inline SrcNoteType type() const {
    if (isXDelta()) {
      return SrcNoteType::XDelta;
    }
    return SrcNoteType(typeBits());
  }

  // name for disassembly/debugging output
  const char* name() const {
    MOZ_ASSERT(uint8_t(type()) < uint8_t(SrcNoteType::Last));
    return specs_[uint8_t(type())].name_;
  }

  inline bool isGettable() const {
    return uint8_t(type()) <= uint8_t(SrcNoteType::LastGettable);
  }

  inline bool isTerminator() const {
    return value_ == uint8_t(SrcNoteType::Null);
  }

  inline ptrdiff_t delta() const {
    if (isXDelta()) {
      return value_ & XDeltaMask;
    }
    return value_ & DeltaMask;
  }

 private:
  /*
   * Operand fields follow certain notes and are frequency-encoded: an operand
   * in [0,0x7f] consumes one byte, an operand in [0x80,0x7fffffff] takes four,
   * and the high bit of the first byte is set.
   */
  static constexpr unsigned FourBytesOperandFlag = 0x80;
  static constexpr unsigned FourBytesOperandMask = 0x7f;

  static constexpr unsigned OperandBits = 31;

 public:
  static constexpr size_t MaxOperand = (size_t(1) << OperandBits) - 1;

  static inline bool isRepresentableOperand(ptrdiff_t operand) {
    return 0 <= operand && size_t(operand) <= MaxOperand;
  }

  class ColSpan {
   public:
    enum class Operands {
      // The column span (the diff between the column corresponds to the
      // current op and last known column).
      Span,
      Count
    };

   private:
    /*
     * SrcNoteType::ColSpan values represent changes to the column number.
     * Colspans are signed: negative changes arise in describing constructs like
     * for(;;) loops, that generate code in non-source order. (Negative colspans
     * also have a history of indicating bugs in updating ParseNodes' source
     * locations.)
     *
     * We store colspans in operands. However, unlike normal operands, colspans
     * are signed, so we truncate colspans (toOperand) for storage as
     * operands, and sign-extend operands into colspans when we read them
     * (fromOperand).
     */
    static constexpr ptrdiff_t ColSpanSignBit = 1 << (OperandBits - 1);

    static inline ptrdiff_t fromOperand(ptrdiff_t operand) {
      // There should be no bits set outside the field we're going to
      // sign-extend.
      MOZ_ASSERT(!(operand & ~((1U << OperandBits) - 1)));

      // Sign-extend the least significant OperandBits bits.
      return (operand ^ ColSpanSignBit) - ColSpanSignBit;
    }

   public:
    static constexpr ptrdiff_t MinColSpan = -ColSpanSignBit;
    static constexpr ptrdiff_t MaxColSpan = ColSpanSignBit - 1;

    static inline ptrdiff_t toOperand(ptrdiff_t colspan) {
      // Truncate the two's complement colspan, for storage as an operand.
      ptrdiff_t operand = colspan & ((1U << OperandBits) - 1);

      // When we read this back, we'd better get the value we stored.
      MOZ_ASSERT(fromOperand(operand) == colspan);
      return operand;
    }

    static inline ptrdiff_t getSpan(const SrcNote* sn);
  };

  class SetLine {
   public:
    enum class Operands {
      // The file-absolute source line number of the current op.
      Line,
      Count
    };

   private:
    static inline size_t fromOperand(ptrdiff_t operand) {
      return size_t(operand);
    }

   public:
    static inline unsigned lengthFor(unsigned line, size_t initialLine) {
      unsigned operandSize = toOperand(line, initialLine) >
                                     ptrdiff_t(SrcNote::FourBytesOperandMask)
                                 ? 4
                                 : 1;
      return 1 /* SetLine */ + operandSize;
    }

    static inline ptrdiff_t toOperand(size_t line, size_t initialLine) {
      MOZ_ASSERT(line >= initialLine);
      return ptrdiff_t(line - initialLine);
    }

    static inline size_t getLine(const SrcNote* sn, size_t initialLine);
  };

  friend class SrcNoteWriter;
  friend class SrcNoteReader;
  friend class SrcNoteIterator;
};

class SrcNoteWriter {
 public:
  // Write a source note with given `type`, and `delta` from the last source
  // note. This writes the source note itself, and `XDelta`s if necessary.
  //
  // This doesn't write or allocate space for operands.
  // If the source note is not nullary, the caller is responsible for calling
  // `writeOperand` immediately after this.
  //
  // `allocator` is called with the number of bytes required to store the notes.
  // `allocator` can be called multiple times for each source note.
  // The last call corresponds to the source note for `type`.
  template <typename T>
  static bool writeNote(SrcNoteType type, ptrdiff_t delta, T allocator) {
    while (delta >= SrcNote::DeltaLimit) {
      ptrdiff_t xdelta = std::min(delta, SrcNote::XDeltaMask);
      SrcNote* sn = allocator(1);
      if (!sn) {
        return false;
      }
      sn->value_ = SrcNote::xDeltaValue(xdelta);
      delta -= xdelta;
    }

    SrcNote* sn = allocator(1);
    if (!sn) {
      return false;
    }
    sn->value_ = SrcNote::noteValue(type, delta);
    return true;
  }

  // Write source note operand.
  //
  // `allocator` is called with the number of bytes required to store the
  // operand.  `allocator` is called only once.
  template <typename T>
  static bool writeOperand(ptrdiff_t operand, T allocator) {
    if (operand > ptrdiff_t(SrcNote::FourBytesOperandMask)) {
      SrcNote* sn = allocator(4);
      if (!sn) {
        return false;
      }

      sn[0].value_ = (SrcNote::FourBytesOperandFlag | (operand >> 24));
      sn[1].value_ = operand >> 16;
      sn[2].value_ = operand >> 8;
      sn[3].value_ = operand;
    } else {
      SrcNote* sn = allocator(1);
      if (!sn) {
        return false;
      }

      sn[0].value_ = operand;
    }

    return true;
  }
};

class SrcNoteReader {
  template <typename T>
  static T getOperandHead(T sn, unsigned which) {
    MOZ_ASSERT(sn->type() != SrcNoteType::XDelta);
    MOZ_ASSERT(uint8_t(which) < sn->arity());

    T curr = sn + 1;
    for (; which; which--) {
      if (curr->isFourBytesOperand()) {
        curr += 4;
      } else {
        curr++;
      }
    }
    return curr;
  }

 public:
  // Return the operand of source note `sn`, specified by `which`.
  static ptrdiff_t getOperand(const SrcNote* sn, unsigned which) {
    const SrcNote* head = getOperandHead(sn, which);

    if (head->isFourBytesOperand()) {
      return ptrdiff_t(
          (uint32_t(head[0].value_ & SrcNote::FourBytesOperandMask) << 24) |
          (uint32_t(head[1].value_) << 16) | (uint32_t(head[2].value_) << 8) |
          uint32_t(head[3].value_));
    }

    return ptrdiff_t(head[0].value_);
  }
};

/* static */
inline ptrdiff_t SrcNote::ColSpan::getSpan(const SrcNote* sn) {
  return fromOperand(SrcNoteReader::getOperand(sn, unsigned(Operands::Span)));
}

/* static */
inline size_t SrcNote::SetLine::getLine(const SrcNote* sn, size_t initialLine) {
  return initialLine +
         fromOperand(SrcNoteReader::getOperand(sn, unsigned(Operands::Line)));
}

// Iterate over SrcNote array, until it hits terminator.
//
// Usage:
//   for (SrcNoteIterator iter(notes); !iter.atEnd(); ++iter) {
//     auto sn = *iter; // `sn` is `const SrcNote*` typed.
//     ...
//   }
class SrcNoteIterator {
  const SrcNote* current_;

  void next() {
    unsigned arity = current_->arity();
    current_++;

    for (; arity; arity--) {
      if (current_->isFourBytesOperand()) {
        current_ += 4;
      } else {
        current_++;
      }
    }
  }

 public:
  SrcNoteIterator() = delete;

  SrcNoteIterator(const SrcNoteIterator& other) = delete;
  SrcNoteIterator& operator=(const SrcNoteIterator& other) = delete;

  SrcNoteIterator(SrcNoteIterator&& other) = default;
  SrcNoteIterator& operator=(SrcNoteIterator&& other) = default;

  explicit SrcNoteIterator(const SrcNote* sn) : current_(sn) {}

  bool atEnd() const { return current_->isTerminator(); }

  const SrcNote* operator*() const { return current_; }

  // Pre-increment
  SrcNoteIterator& operator++() {
    next();
    return *this;
  }

  // Post-increment
  SrcNoteIterator operator++(int) = delete;
};

}  // namespace js

#endif /* frontend_SourceNotes_h */

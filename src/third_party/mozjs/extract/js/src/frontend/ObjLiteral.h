/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=0 ft=c:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ObjLiteral_h
#define frontend_ObjLiteral_h

#include "mozilla/BloomFilter.h"  // mozilla::BitBloomFilter
#include "mozilla/Span.h"

#include "frontend/ParserAtom.h"  // ParserAtomsTable, TaggedParserAtomIndex, ParserAtom
#include "js/AllocPolicy.h"
#include "js/Value.h"
#include "js/Vector.h"
#include "util/EnumFlags.h"
#include "vm/BytecodeUtil.h"
#include "vm/Opcodes.h"

/*
 * [SMDOC] ObjLiteral (Object Literal) Handling
 * ============================================
 *
 * The `ObjLiteral*` family of classes defines an infastructure to handle
 * object literals as they are encountered at parse time and translate them
 * into objects or shapes that are attached to the bytecode.
 *
 * The object-literal "instructions", whose opcodes are defined in
 * `ObjLiteralOpcode` below, each specify one key (atom property name, or
 * numeric index) and one value. An `ObjLiteralWriter` buffers a linear
 * sequence of such instructions, along with a side-table of atom references.
 * The writer stores a compact binary format that is then interpreted by the
 * `ObjLiteralReader` to construct an object or shape according to the
 * instructions.
 *
 * This may seem like an odd dance: create an intermediate data structure that
 * specifies key/value pairs, then later build the object/shape. Why not just do
 * so directly, as we parse? In fact, we used to do this. However, for several
 * good reasons, we want to avoid allocating or touching GC things at all
 * *during* the parse. We thus use a sequence of ObjLiteral instructions as an
 * intermediate data structure to carry object literal contents from parse to
 * the time at which we *can* allocate GC things.
 *
 * (The original intent was to allow for ObjLiteral instructions to actually be
 * invoked by a new JS opcode, JSOp::ObjLiteral, thus replacing the more
 * general opcode sequences sometimes generated to fill in objects and removing
 * the need to attach actual objects to JSOp::Object or JSOp::NewObject.
 * However, this was far too invasive and led to performance regressions, so
 * currently ObjLiteral only carries literals as far as the end of the parse
 * pipeline, when all GC things are allocated.)
 *
 * ObjLiteral data structures are used to represent object literals whenever
 * they are "compatible". See
 * BytecodeEmitter::isPropertyListObjLiteralCompatible for the precise
 * conditions; in brief, we can represent object literals with "primitive"
 * (numeric, boolean, string, null/undefined) values, and "normal"
 * (non-computed) object names. We can also represent arrays with the same
 * value restrictions. We cannot represent nested objects. We use ObjLiteral in
 * two different ways:
 *
 * - To build a template shape, when we can support the property keys but not
 *   the property values.
 * - To build the actual result object, when we support the property keys and
 *   the values and this is a JSOp::Object case (see below).
 *
 * Design and Performance Considerations
 * -------------------------------------
 *
 * As a brief overview, there are a number of opcodes that allocate objects:
 *
 * - JSOp::NewInit allocates a new empty `{}` object.
 *
 * - JSOp::NewObject, with a shape as an argument (held by the script data
 *   side-tables), allocates a new object with the given `shape` (property keys)
 *   and `undefined` property values.
 *
 * - JSOp::Object, with an object as argument, instructs the runtime to
 *   literally return the object argument as the result. This is thus only an
 *   "allocation" in the sense that the object was originally allocated when
 *   the script data / bytecode was created. It is only used when we know for
 *   sure that the script, and this program point within the script, will run
 *   *once*. (See the `treatAsRunOnce` flag on JSScript.)
 *
 * An operation occurs in a "singleton context", according to the parser, if it
 * will only ever execute once. In particular, this happens when (i) the script
 * is a "run-once" script, which is usually the case for e.g. top-level scripts
 * of web-pages (they run on page load, but no function or handle wraps or
 * refers to the script so it can't be invoked again), and (ii) the operation
 * itself is not within a loop or function in that run-once script.
 *
 * When we encounter an object literal, we decide which opcode to use, and we
 * construct the ObjLiteral and the bytecode using its result appropriately:
 *
 * - If in a singleton context, and if we support the values, we use
 *   JSOp::Object and we build the ObjLiteral instructions with values.
 * - Otherwise, if we support the keys but not the values, or if we are not
 *   in a singleton context, we use JSOp::NewObject. In this case, the initial
 *   opcode only creates an object with empty values, so BytecodeEmitter then
 *   generates bytecode to set the values appropriately.
 * - Otherwise, we generate JSOp::NewInit and bytecode to add properties one at
 *   a time. This will always work, but is the slowest and least
 *   memory-efficient option.
 */

namespace js {

class FrontendContext;
class JSONPrinter;
class LifoAlloc;

namespace frontend {
struct CompilationAtomCache;
struct CompilationStencil;
class StencilXDR;
}  // namespace frontend

// Object-literal instruction opcodes. An object literal is constructed by a
// straight-line sequence of these ops, each adding one property to the
// object.
enum class ObjLiteralOpcode : uint8_t {
  INVALID = 0,

  ConstValue = 1,  // numeric types only.
  ConstString = 2,
  Null = 3,
  Undefined = 4,
  True = 5,
  False = 6,

  MAX = False,
};

// The kind of GC thing constructed by the ObjLiteral framework and stored in
// the script data.
enum class ObjLiteralKind : uint8_t {
  // Construct an ArrayObject from a list of dense elements.
  Array,

  // Construct an ArrayObject (the call site object) for a tagged template call
  // from a list of dense elements for the cooked array followed by the dense
  // elements for the `.raw` array.
  CallSiteObj,

  // Construct a PlainObject from a list of property keys/values.
  Object,

  // Construct a PlainObject Shape from a list of property keys.
  Shape,

  // Invalid sentinel value. Must be the last enum value.
  Invalid
};

// Flags that are associated with a sequence of object-literal instructions.
// (These become bitflags by wrapping with EnumSet below.)
enum class ObjLiteralFlag : uint8_t {
  // If set, this object contains index property, or duplicate non-index
  // property.
  // This flag is valid only if the ObjLiteralKind is not Array.
  HasIndexOrDuplicatePropName = 1 << 0,

  // Note: at most 6 flags are currently supported. See ObjLiteralKindAndFlags.
};

using ObjLiteralFlags = EnumFlags<ObjLiteralFlag>;

// Helper class to encode ObjLiteralKind and ObjLiteralFlags in a single byte.
class ObjLiteralKindAndFlags {
  uint8_t bits_ = 0;

  static constexpr size_t KindBits = 3;
  static constexpr size_t KindMask = BitMask(KindBits);

  static_assert(size_t(ObjLiteralKind::Invalid) <= KindMask,
                "ObjLiteralKind needs more bits");

 public:
  ObjLiteralKindAndFlags() = default;

  ObjLiteralKindAndFlags(ObjLiteralKind kind, ObjLiteralFlags flags)
      : bits_(size_t(kind) | (flags.toRaw() << KindBits)) {
    MOZ_ASSERT(this->kind() == kind);
    MOZ_ASSERT(this->flags() == flags);
  }

  ObjLiteralKind kind() const { return ObjLiteralKind(bits_ & KindMask); }
  ObjLiteralFlags flags() const {
    ObjLiteralFlags res;
    res.setRaw(bits_ >> KindBits);
    return res;
  }

  uint8_t toRaw() const { return bits_; }
  void setRaw(uint8_t bits) { bits_ = bits; }
};

inline bool ObjLiteralOpcodeHasValueArg(ObjLiteralOpcode op) {
  return op == ObjLiteralOpcode::ConstValue;
}

inline bool ObjLiteralOpcodeHasAtomArg(ObjLiteralOpcode op) {
  return op == ObjLiteralOpcode::ConstString;
}

struct ObjLiteralReaderBase;

// Property name (as TaggedParserAtomIndex) or an integer index.  Only used for
// object-type literals; array literals do not require the index (the sequence
// is always dense, with no holes, so the index is implicit). For the latter
// case, we have a `None` placeholder.
struct ObjLiteralKey {
 private:
  uint32_t value_;

  enum ObjLiteralKeyType {
    None,
    AtomIndex,
    ArrayIndex,
  };

  ObjLiteralKeyType type_;

  ObjLiteralKey(uint32_t value, ObjLiteralKeyType ty)
      : value_(value), type_(ty) {}

 public:
  ObjLiteralKey() : ObjLiteralKey(0, None) {}
  ObjLiteralKey(uint32_t value, bool isArrayIndex)
      : ObjLiteralKey(value, isArrayIndex ? ArrayIndex : AtomIndex) {}
  ObjLiteralKey(const ObjLiteralKey& other) = default;

  static ObjLiteralKey fromPropName(frontend::TaggedParserAtomIndex atomIndex) {
    return ObjLiteralKey(atomIndex.rawData(), false);
  }
  static ObjLiteralKey fromArrayIndex(uint32_t index) {
    return ObjLiteralKey(index, true);
  }
  static ObjLiteralKey none() { return ObjLiteralKey(); }

  bool isNone() const { return type_ == None; }
  bool isAtomIndex() const { return type_ == AtomIndex; }
  bool isArrayIndex() const { return type_ == ArrayIndex; }

  frontend::TaggedParserAtomIndex getAtomIndex() const {
    MOZ_ASSERT(isAtomIndex());
    return frontend::TaggedParserAtomIndex::fromRaw(value_);
  }
  uint32_t getArrayIndex() const {
    MOZ_ASSERT(isArrayIndex());
    return value_;
  }

  uint32_t rawIndex() const { return value_; }
};

struct ObjLiteralWriterBase {
 protected:
  friend struct ObjLiteralReaderBase;  // for access to mask and shift.
  static const uint32_t ATOM_INDEX_MASK = 0x7fffffff;
  // If set, the atom index field is an array index, not an atom index.
  static const uint32_t INDEXED_PROP = 0x80000000;

 public:
  using CodeVector = Vector<uint8_t, 64, js::SystemAllocPolicy>;

 protected:
  CodeVector code_;

 public:
  ObjLiteralWriterBase() = default;

  uint32_t curOffset() const { return code_.length(); }

 private:
  [[nodiscard]] bool pushByte(FrontendContext* fc, uint8_t data) {
    if (!code_.append(data)) {
      js::ReportOutOfMemory(fc);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool prepareBytes(FrontendContext* fc, size_t len,
                                  uint8_t** p) {
    size_t offset = code_.length();
    if (!code_.growByUninitialized(len)) {
      js::ReportOutOfMemory(fc);
      return false;
    }
    *p = &code_[offset];
    return true;
  }

  template <typename T>
  [[nodiscard]] bool pushRawData(FrontendContext* fc, T data) {
    uint8_t* p = nullptr;
    if (!prepareBytes(fc, sizeof(T), &p)) {
      return false;
    }
    memcpy(p, &data, sizeof(T));
    return true;
  }

 protected:
  [[nodiscard]] bool pushOpAndName(FrontendContext* fc, ObjLiteralOpcode op,
                                   ObjLiteralKey key) {
    uint8_t opdata = static_cast<uint8_t>(op);
    uint32_t data = key.rawIndex() | (key.isArrayIndex() ? INDEXED_PROP : 0);
    return pushByte(fc, opdata) && pushRawData(fc, data);
  }

  [[nodiscard]] bool pushValueArg(FrontendContext* fc, const JS::Value& value) {
    MOZ_ASSERT(value.isNumber() || value.isNullOrUndefined() ||
               value.isBoolean());
    uint64_t data = value.asRawBits();
    return pushRawData(fc, data);
  }

  [[nodiscard]] bool pushAtomArg(FrontendContext* fc,
                                 frontend::TaggedParserAtomIndex atomIndex) {
    return pushRawData(fc, atomIndex.rawData());
  }
};

// An object-literal instruction writer. This class, held by the bytecode
// emitter, keeps a sequence of object-literal instructions emitted as object
// literal expressions are parsed. It allows the user to 'begin' and 'end'
// straight-line sequences, returning the offsets for this range of instructions
// within the writer.
struct ObjLiteralWriter : private ObjLiteralWriterBase {
 public:
  ObjLiteralWriter() = default;

  void clear() { code_.clear(); }

  using CodeVector = typename ObjLiteralWriterBase::CodeVector;

  bool checkForDuplicatedNames(FrontendContext* fc);
  mozilla::Span<const uint8_t> getCode() const { return code_; }
  ObjLiteralKind getKind() const { return kind_; }
  ObjLiteralFlags getFlags() const { return flags_; }
  uint32_t getPropertyCount() const { return propertyCount_; }

  void beginArray(JSOp op) {
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    MOZ_ASSERT(op == JSOp::Object);
    kind_ = ObjLiteralKind::Array;
  }
  void beginCallSiteObj(JSOp op) {
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    MOZ_ASSERT(op == JSOp::CallSiteObj);
    kind_ = ObjLiteralKind::CallSiteObj;
  }
  void beginObject(JSOp op) {
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    MOZ_ASSERT(op == JSOp::Object);
    kind_ = ObjLiteralKind::Object;
  }
  void beginShape(JSOp op) {
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_SHAPE);
    MOZ_ASSERT(op == JSOp::NewObject);
    kind_ = ObjLiteralKind::Shape;
  }

  bool setPropName(frontend::ParserAtomsTable& parserAtoms,
                   const frontend::TaggedParserAtomIndex propName) {
    // Only valid in object-mode.
    setPropNameNoDuplicateCheck(parserAtoms, propName);

    if (flags_.hasFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName)) {
      return true;
    }

    // OK to early return if we've already discovered a potential duplicate.
    if (mightContainDuplicatePropertyNames_) {
      return true;
    }

    // Check bloom filter for duplicate, and add if not already represented.
    if (propNamesFilter_.mightContain(propName.rawData())) {
      mightContainDuplicatePropertyNames_ = true;
    } else {
      propNamesFilter_.add(propName.rawData());
    }
    return true;
  }
  void setPropNameNoDuplicateCheck(
      frontend::ParserAtomsTable& parserAtoms,
      const frontend::TaggedParserAtomIndex propName) {
    MOZ_ASSERT(kind_ == ObjLiteralKind::Object ||
               kind_ == ObjLiteralKind::Shape);
    parserAtoms.markUsedByStencil(propName, frontend::ParserAtom::Atomize::Yes);
    nextKey_ = ObjLiteralKey::fromPropName(propName);
  }
  void setPropIndex(uint32_t propIndex) {
    MOZ_ASSERT(kind_ == ObjLiteralKind::Object);
    MOZ_ASSERT(propIndex <= ATOM_INDEX_MASK);
    nextKey_ = ObjLiteralKey::fromArrayIndex(propIndex);
    flags_.setFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName);
  }
  void beginDenseArrayElements() {
    MOZ_ASSERT(kind_ == ObjLiteralKind::Array ||
               kind_ == ObjLiteralKind::CallSiteObj);
    // Dense array element sequences do not use the keys; the indices are
    // implicit.
    nextKey_ = ObjLiteralKey::none();
  }

  [[nodiscard]] bool propWithConstNumericValue(FrontendContext* fc,
                                               const JS::Value& value) {
    MOZ_ASSERT(kind_ != ObjLiteralKind::Shape);
    propertyCount_++;
    MOZ_ASSERT(value.isNumber());
    return pushOpAndName(fc, ObjLiteralOpcode::ConstValue, nextKey_) &&
           pushValueArg(fc, value);
  }
  [[nodiscard]] bool propWithAtomValue(
      FrontendContext* fc, frontend::ParserAtomsTable& parserAtoms,
      const frontend::TaggedParserAtomIndex value) {
    MOZ_ASSERT(kind_ != ObjLiteralKind::Shape);
    propertyCount_++;
    parserAtoms.markUsedByStencil(value, frontend::ParserAtom::Atomize::No);
    return pushOpAndName(fc, ObjLiteralOpcode::ConstString, nextKey_) &&
           pushAtomArg(fc, value);
  }
  [[nodiscard]] bool propWithNullValue(FrontendContext* fc) {
    MOZ_ASSERT(kind_ != ObjLiteralKind::Shape);
    propertyCount_++;
    return pushOpAndName(fc, ObjLiteralOpcode::Null, nextKey_);
  }
  [[nodiscard]] bool propWithUndefinedValue(FrontendContext* fc) {
    propertyCount_++;
    return pushOpAndName(fc, ObjLiteralOpcode::Undefined, nextKey_);
  }
  [[nodiscard]] bool propWithTrueValue(FrontendContext* fc) {
    MOZ_ASSERT(kind_ != ObjLiteralKind::Shape);
    propertyCount_++;
    return pushOpAndName(fc, ObjLiteralOpcode::True, nextKey_);
  }
  [[nodiscard]] bool propWithFalseValue(FrontendContext* fc) {
    MOZ_ASSERT(kind_ != ObjLiteralKind::Shape);
    propertyCount_++;
    return pushOpAndName(fc, ObjLiteralOpcode::False, nextKey_);
  }

  static bool arrayIndexInRange(int32_t i) {
    return i >= 0 && static_cast<uint32_t>(i) <= ATOM_INDEX_MASK;
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(JSONPrinter& json,
            const frontend::CompilationStencil* stencil) const;
  void dumpFields(JSONPrinter& json,
                  const frontend::CompilationStencil* stencil) const;
#endif

 private:
  // Set to true if we've found possible duplicate names while building.
  // This field is placed next to `flags_` field, to reduce padding.
  bool mightContainDuplicatePropertyNames_ = false;

  ObjLiteralKind kind_ = ObjLiteralKind::Invalid;
  ObjLiteralFlags flags_;
  ObjLiteralKey nextKey_;
  uint32_t propertyCount_ = 0;

  // Duplicate property names detection is performed in the following way:
  //   * while emitting code, add each property names with
  //     `propNamesFilter_`
  //   * if possible duplicate property name is detected, set
  //     `mightContainDuplicatePropertyNames_` to true
  //   * in `checkForDuplicatedNames` method,
  //     if `mightContainDuplicatePropertyNames_` is true,
  //     check the duplicate property names with `HashSet`, and if it exists,
  //     set HasIndexOrDuplicatePropName flag.
  mozilla::BitBloomFilter<12, frontend::TaggedParserAtomIndex> propNamesFilter_;
};

struct ObjLiteralReaderBase {
 private:
  mozilla::Span<const uint8_t> data_;
  size_t cursor_;

  [[nodiscard]] bool readByte(uint8_t* b) {
    if (cursor_ + 1 > data_.Length()) {
      return false;
    }
    *b = *data_.From(cursor_).data();
    cursor_ += 1;
    return true;
  }

  [[nodiscard]] bool readBytes(size_t size, const uint8_t** p) {
    if (cursor_ + size > data_.Length()) {
      return false;
    }
    *p = data_.From(cursor_).data();
    cursor_ += size;
    return true;
  }

  template <typename T>
  [[nodiscard]] bool readRawData(T* data) {
    const uint8_t* p = nullptr;
    if (!readBytes(sizeof(T), &p)) {
      return false;
    }
    memcpy(data, p, sizeof(T));
    return true;
  }

 public:
  explicit ObjLiteralReaderBase(mozilla::Span<const uint8_t> data)
      : data_(data), cursor_(0) {}

  [[nodiscard]] bool readOpAndKey(ObjLiteralOpcode* op, ObjLiteralKey* key) {
    uint8_t opbyte;
    if (!readByte(&opbyte)) {
      return false;
    }
    if (MOZ_UNLIKELY(opbyte > static_cast<uint8_t>(ObjLiteralOpcode::MAX))) {
      return false;
    }
    *op = static_cast<ObjLiteralOpcode>(opbyte);

    uint32_t data;
    if (!readRawData(&data)) {
      return false;
    }
    bool isArray = data & ObjLiteralWriterBase::INDEXED_PROP;
    uint32_t rawIndex = data & ~ObjLiteralWriterBase::INDEXED_PROP;
    *key = ObjLiteralKey(rawIndex, isArray);
    return true;
  }

  [[nodiscard]] bool readValueArg(JS::Value* value) {
    uint64_t data;
    if (!readRawData(&data)) {
      return false;
    }
    *value = JS::Value::fromRawBits(data);
    return true;
  }

  [[nodiscard]] bool readAtomArg(frontend::TaggedParserAtomIndex* atomIndex) {
    return readRawData(atomIndex->rawDataRef());
  }

  size_t cursor() const { return cursor_; }
};

// A single object-literal instruction, creating one property on an object.
struct ObjLiteralInsn {
 private:
  ObjLiteralOpcode op_;
  ObjLiteralKey key_;
  union Arg {
    explicit Arg(uint64_t raw_) : raw(raw_) {}

    JS::Value constValue;
    frontend::TaggedParserAtomIndex atomIndex;
    uint64_t raw;
  } arg_;

 public:
  ObjLiteralInsn() : op_(ObjLiteralOpcode::INVALID), arg_(0) {}
  ObjLiteralInsn(ObjLiteralOpcode op, ObjLiteralKey key)
      : op_(op), key_(key), arg_(0) {
    MOZ_ASSERT(!hasConstValue());
    MOZ_ASSERT(!hasAtomIndex());
  }
  ObjLiteralInsn(ObjLiteralOpcode op, ObjLiteralKey key, const JS::Value& value)
      : op_(op), key_(key), arg_(0) {
    MOZ_ASSERT(hasConstValue());
    MOZ_ASSERT(!hasAtomIndex());
    arg_.constValue = value;
  }
  ObjLiteralInsn(ObjLiteralOpcode op, ObjLiteralKey key,
                 frontend::TaggedParserAtomIndex atomIndex)
      : op_(op), key_(key), arg_(0) {
    MOZ_ASSERT(!hasConstValue());
    MOZ_ASSERT(hasAtomIndex());
    arg_.atomIndex = atomIndex;
  }
  ObjLiteralInsn(const ObjLiteralInsn& other) : ObjLiteralInsn() {
    *this = other;
  }
  ObjLiteralInsn& operator=(const ObjLiteralInsn& other) {
    op_ = other.op_;
    key_ = other.key_;
    arg_.raw = other.arg_.raw;
    return *this;
  }

  bool isValid() const {
    return op_ > ObjLiteralOpcode::INVALID && op_ <= ObjLiteralOpcode::MAX;
  }

  ObjLiteralOpcode getOp() const {
    MOZ_ASSERT(isValid());
    return op_;
  }
  const ObjLiteralKey& getKey() const {
    MOZ_ASSERT(isValid());
    return key_;
  }

  bool hasConstValue() const {
    MOZ_ASSERT(isValid());
    return ObjLiteralOpcodeHasValueArg(op_);
  }
  bool hasAtomIndex() const {
    MOZ_ASSERT(isValid());
    return ObjLiteralOpcodeHasAtomArg(op_);
  }

  JS::Value getConstValue() const {
    MOZ_ASSERT(isValid());
    MOZ_ASSERT(hasConstValue());
    return arg_.constValue;
  }
  frontend::TaggedParserAtomIndex getAtomIndex() const {
    MOZ_ASSERT(isValid());
    MOZ_ASSERT(hasAtomIndex());
    return arg_.atomIndex;
  };
};

// A reader that parses a sequence of object-literal instructions out of the
// encoded form.
struct ObjLiteralReader : private ObjLiteralReaderBase {
 public:
  explicit ObjLiteralReader(mozilla::Span<const uint8_t> data)
      : ObjLiteralReaderBase(data) {}

  [[nodiscard]] bool readInsn(ObjLiteralInsn* insn) {
    ObjLiteralOpcode op;
    ObjLiteralKey key;
    if (!readOpAndKey(&op, &key)) {
      return false;
    }
    if (ObjLiteralOpcodeHasValueArg(op)) {
      JS::Value value;
      if (!readValueArg(&value)) {
        return false;
      }
      *insn = ObjLiteralInsn(op, key, value);
      return true;
    }
    if (ObjLiteralOpcodeHasAtomArg(op)) {
      frontend::TaggedParserAtomIndex atomIndex;
      if (!readAtomArg(&atomIndex)) {
        return false;
      }
      *insn = ObjLiteralInsn(op, key, atomIndex);
      return true;
    }
    *insn = ObjLiteralInsn(op, key);
    return true;
  }
};

// A class to modify the code, while keeping the structure.
struct ObjLiteralModifier : private ObjLiteralReaderBase {
  mozilla::Span<uint8_t> mutableData_;

 public:
  explicit ObjLiteralModifier(mozilla::Span<uint8_t> data)
      : ObjLiteralReaderBase(data), mutableData_(data) {}

 private:
  // Map `atom` with `map`, and write to `atomCursor` of `mutableData_`.
  template <typename MapT>
  void mapOneAtom(MapT map, frontend::TaggedParserAtomIndex atom,
                  size_t atomCursor) {
    auto atomIndex = map(atom);
    memcpy(mutableData_.data() + atomCursor, atomIndex.rawDataRef(),
           sizeof(frontend::TaggedParserAtomIndex));
  }

  // Map atoms in single instruction.
  // Return true if it successfully maps.
  // Return false if there's no more instruction.
  template <typename MapT>
  bool mapInsnAtom(MapT map) {
    ObjLiteralOpcode op;
    ObjLiteralKey key;

    size_t opCursor = cursor();
    if (!readOpAndKey(&op, &key)) {
      return false;
    }
    if (key.isAtomIndex()) {
      static constexpr size_t OpLength = 1;
      size_t atomCursor = opCursor + OpLength;
      mapOneAtom(map, key.getAtomIndex(), atomCursor);
    }

    if (ObjLiteralOpcodeHasValueArg(op)) {
      JS::Value value;
      if (!readValueArg(&value)) {
        return false;
      }
    } else if (ObjLiteralOpcodeHasAtomArg(op)) {
      size_t atomCursor = cursor();

      frontend::TaggedParserAtomIndex atomIndex;
      if (!readAtomArg(&atomIndex)) {
        return false;
      }

      mapOneAtom(map, atomIndex, atomCursor);
    }

    return true;
  }

 public:
  // Map TaggedParserAtomIndex inside the code in place, with given function.
  template <typename MapT>
  void mapAtom(MapT map) {
    while (mapInsnAtom(map)) {
    }
  }
};

class ObjLiteralStencil {
  friend class frontend::StencilXDR;

  // CompilationStencil::clone has to update the code pointer.
  friend struct frontend::CompilationStencil;

  mozilla::Span<uint8_t> code_;
  ObjLiteralKindAndFlags kindAndFlags_;
  uint32_t propertyCount_ = 0;

 public:
  ObjLiteralStencil() = default;

  ObjLiteralStencil(uint8_t* code, size_t length, ObjLiteralKind kind,
                    const ObjLiteralFlags& flags, uint32_t propertyCount)
      : code_(mozilla::Span(code, length)),
        kindAndFlags_(kind, flags),
        propertyCount_(propertyCount) {}

  JS::GCCellPtr create(JSContext* cx,
                       const frontend::CompilationAtomCache& atomCache) const;

  mozilla::Span<const uint8_t> code() const { return code_; }
  ObjLiteralKind kind() const { return kindAndFlags_.kind(); }
  ObjLiteralFlags flags() const { return kindAndFlags_.flags(); }
  uint32_t propertyCount() const { return propertyCount_; }

#ifdef DEBUG
  bool isContainedIn(const LifoAlloc& alloc) const;
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(JSONPrinter& json,
            const frontend::CompilationStencil* stencil) const;
  void dumpFields(JSONPrinter& json,
                  const frontend::CompilationStencil* stencil) const;

#endif
};

}  // namespace js
#endif  // frontend_ObjLiteral_h

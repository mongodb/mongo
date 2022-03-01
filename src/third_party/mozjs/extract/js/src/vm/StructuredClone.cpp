/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * This file implements the structured data algorithms of
 * https://html.spec.whatwg.org/multipage/structured-data.html
 *
 * The spec is in two parts:
 *
 * -   StructuredSerialize examines a JS value and produces a graph of Records.
 * -   StructuredDeserialize walks the Records and produces a new JS value.
 *
 * The differences between our implementation and the spec are minor:
 *
 * -   We call the two phases "write" and "read".
 * -   Our algorithms use an explicit work stack, rather than recursion.
 * -   Serialized data is a flat array of bytes, not a (possibly cyclic) graph
 *     of "Records".
 * -   As a consequence, we handle non-treelike object graphs differently.
 *     We serialize objects that appear in multiple places in the input as
 *     backreferences, using sequential integer indexes.
 *     See `JSStructuredCloneReader::allObjs`, our take on the "memory" map
 *     in the spec's StructuredDeserialize.
 */

#include "js/StructuredClone.h"

#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/ScopeExit.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "jsapi.h"
#include "jsdate.h"

#include "builtin/DataViewObject.h"
#include "builtin/MapObject.h"
#include "js/Array.h"        // JS::GetArrayLength, JS::IsArrayObject
#include "js/ArrayBuffer.h"  // JS::{ArrayBufferHasData,DetachArrayBuffer,IsArrayBufferObject,New{,Mapped}ArrayBufferWithContents,ReleaseMappedArrayBufferContents}
#include "js/Date.h"
#include "js/experimental/TypedData.h"  // JS_NewDataView, JS_New{{Ui,I}nt{8,16,32},Float{32,64},Uint8Clamped,Big{Ui,I}nt64}ArrayWithBuffer
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/GCHashTable.h"
#include "js/Object.h"             // JS::GetBuiltinClass
#include "js/RegExpFlags.h"        // JS::RegExpFlag, JS::RegExpFlags
#include "js/ScalarType.h"         // js::Scalar::Type
#include "js/SharedArrayBuffer.h"  // JS::IsSharedArrayBufferObject
#include "js/Wrapper.h"
#include "vm/BigIntType.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/RegExpObject.h"
#include "vm/SavedFrame.h"
#include "vm/SharedArrayObject.h"
#include "vm/TypedArrayObject.h"
#include "vm/WrapperObject.h"
#include "wasm/WasmJS.h"

#include "vm/InlineCharBuffer-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;

using JS::CanonicalizeNaN;
using JS::GetBuiltinClass;
using JS::RegExpFlag;
using JS::RegExpFlags;
using JS::RootedValueVector;
using mozilla::AssertedCast;
using mozilla::BitwiseCast;
using mozilla::NativeEndian;
using mozilla::NumbersAreIdentical;
using mozilla::RangedPtr;

// When you make updates here, make sure you consider whether you need to bump
// the value of JS_STRUCTURED_CLONE_VERSION in js/public/StructuredClone.h.  You
// will likely need to increment the version if anything at all changes in the
// serialization format.
//
// Note that SCTAG_END_OF_KEYS is written into the serialized form and should
// have a stable ID, it need not be at the end of the list and should not be
// used for sizing data structures.

enum StructuredDataType : uint32_t {
  // Structured data types provided by the engine
  SCTAG_FLOAT_MAX = 0xFFF00000,
  SCTAG_HEADER = 0xFFF10000,
  SCTAG_NULL = 0xFFFF0000,
  SCTAG_UNDEFINED,
  SCTAG_BOOLEAN,
  SCTAG_INT32,
  SCTAG_STRING,
  SCTAG_DATE_OBJECT,
  SCTAG_REGEXP_OBJECT,
  SCTAG_ARRAY_OBJECT,
  SCTAG_OBJECT_OBJECT,
  SCTAG_ARRAY_BUFFER_OBJECT_V2,  // Old version, for backwards compatibility.
  SCTAG_BOOLEAN_OBJECT,
  SCTAG_STRING_OBJECT,
  SCTAG_NUMBER_OBJECT,
  SCTAG_BACK_REFERENCE_OBJECT,
  SCTAG_DO_NOT_USE_1,           // Required for backwards compatibility
  SCTAG_DO_NOT_USE_2,           // Required for backwards compatibility
  SCTAG_TYPED_ARRAY_OBJECT_V2,  // Old version, for backwards compatibility.
  SCTAG_MAP_OBJECT,
  SCTAG_SET_OBJECT,
  SCTAG_END_OF_KEYS,
  SCTAG_DO_NOT_USE_3,         // Required for backwards compatibility
  SCTAG_DATA_VIEW_OBJECT_V2,  // Old version, for backwards compatibility.
  SCTAG_SAVED_FRAME_OBJECT,

  // No new tags before principals.
  SCTAG_JSPRINCIPALS,
  SCTAG_NULL_JSPRINCIPALS,
  SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_SYSTEM,
  SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_NOT_SYSTEM,

  SCTAG_SHARED_ARRAY_BUFFER_OBJECT,
  SCTAG_SHARED_WASM_MEMORY_OBJECT,

  SCTAG_BIGINT,
  SCTAG_BIGINT_OBJECT,

  SCTAG_ARRAY_BUFFER_OBJECT,
  SCTAG_TYPED_ARRAY_OBJECT,
  SCTAG_DATA_VIEW_OBJECT,

  SCTAG_TYPED_ARRAY_V1_MIN = 0xFFFF0100,
  SCTAG_TYPED_ARRAY_V1_INT8 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Int8,
  SCTAG_TYPED_ARRAY_V1_UINT8 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Uint8,
  SCTAG_TYPED_ARRAY_V1_INT16 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Int16,
  SCTAG_TYPED_ARRAY_V1_UINT16 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Uint16,
  SCTAG_TYPED_ARRAY_V1_INT32 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Int32,
  SCTAG_TYPED_ARRAY_V1_UINT32 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Uint32,
  SCTAG_TYPED_ARRAY_V1_FLOAT32 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Float32,
  SCTAG_TYPED_ARRAY_V1_FLOAT64 = SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Float64,
  SCTAG_TYPED_ARRAY_V1_UINT8_CLAMPED =
      SCTAG_TYPED_ARRAY_V1_MIN + Scalar::Uint8Clamped,
  // BigInt64 and BigUint64 are not supported in the v1 format.
  SCTAG_TYPED_ARRAY_V1_MAX = SCTAG_TYPED_ARRAY_V1_UINT8_CLAMPED,

  // Define a separate range of numbers for Transferable-only tags, since
  // they are not used for persistent clone buffers and therefore do not
  // require bumping JS_STRUCTURED_CLONE_VERSION.
  SCTAG_TRANSFER_MAP_HEADER = 0xFFFF0200,
  SCTAG_TRANSFER_MAP_PENDING_ENTRY,
  SCTAG_TRANSFER_MAP_ARRAY_BUFFER,
  SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER,
  SCTAG_TRANSFER_MAP_END_OF_BUILTIN_TYPES,

  SCTAG_END_OF_BUILTIN_TYPES
};

/*
 * Format of transfer map:
 *   <SCTAG_TRANSFER_MAP_HEADER, TransferableMapHeader(UNREAD|TRANSFERRED)>
 *   numTransferables (64 bits)
 *   array of:
 *     <SCTAG_TRANSFER_MAP_*, TransferableOwnership>
 *     pointer (64 bits)
 *     extraData (64 bits), eg byte length for ArrayBuffers
 */

// Data associated with an SCTAG_TRANSFER_MAP_HEADER that tells whether the
// contents have been read out yet or not.
enum TransferableMapHeader { SCTAG_TM_UNREAD = 0, SCTAG_TM_TRANSFERRED };

static inline uint64_t PairToUInt64(uint32_t tag, uint32_t data) {
  return uint64_t(data) | (uint64_t(tag) << 32);
}

namespace js {

template <typename T, typename AllocPolicy>
struct BufferIterator {
  using BufferList = mozilla::BufferList<AllocPolicy>;

  explicit BufferIterator(const BufferList& buffer)
      : mBuffer(buffer), mIter(buffer.Iter()) {
    static_assert(8 % sizeof(T) == 0);
  }

  explicit BufferIterator(const JSStructuredCloneData& data)
      : mBuffer(data.bufList_), mIter(data.Start()) {}

  BufferIterator& operator=(const BufferIterator& other) {
    MOZ_ASSERT(&mBuffer == &other.mBuffer);
    mIter = other.mIter;
    return *this;
  }

  [[nodiscard]] bool advance(size_t size = sizeof(T)) {
    return mIter.AdvanceAcrossSegments(mBuffer, size);
  }

  BufferIterator operator++(int) {
    BufferIterator ret = *this;
    if (!advance(sizeof(T))) {
      MOZ_ASSERT(false, "Failed to read StructuredCloneData. Data incomplete");
    }
    return ret;
  }

  BufferIterator& operator+=(size_t size) {
    if (!advance(size)) {
      MOZ_ASSERT(false, "Failed to read StructuredCloneData. Data incomplete");
    }
    return *this;
  }

  size_t operator-(const BufferIterator& other) const {
    MOZ_ASSERT(&mBuffer == &other.mBuffer);
    return mBuffer.RangeLength(other.mIter, mIter);
  }

  bool done() const { return mIter.Done(); }

  [[nodiscard]] bool readBytes(char* outData, size_t size) {
    return mBuffer.ReadBytes(mIter, outData, size);
  }

  void write(const T& data) {
    MOZ_ASSERT(mIter.HasRoomFor(sizeof(T)));
    *reinterpret_cast<T*>(mIter.Data()) = data;
  }

  T peek() const {
    MOZ_ASSERT(mIter.HasRoomFor(sizeof(T)));
    return *reinterpret_cast<T*>(mIter.Data());
  }

  bool canPeek() const { return mIter.HasRoomFor(sizeof(T)); }

  const BufferList& mBuffer;
  typename BufferList::IterImpl mIter;
};

SharedArrayRawBufferRefs& SharedArrayRawBufferRefs::operator=(
    SharedArrayRawBufferRefs&& other) {
  takeOwnership(std::move(other));
  return *this;
}

SharedArrayRawBufferRefs::~SharedArrayRawBufferRefs() { releaseAll(); }

bool SharedArrayRawBufferRefs::acquire(JSContext* cx,
                                       SharedArrayRawBuffer* rawbuf) {
  if (!refs_.append(rawbuf)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!rawbuf->addReference()) {
    refs_.popBack();
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SC_SAB_REFCNT_OFLO);
    return false;
  }

  return true;
}

bool SharedArrayRawBufferRefs::acquireAll(
    JSContext* cx, const SharedArrayRawBufferRefs& that) {
  if (!refs_.reserve(refs_.length() + that.refs_.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (auto ref : that.refs_) {
    if (!ref->addReference()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_SC_SAB_REFCNT_OFLO);
      return false;
    }
    MOZ_ALWAYS_TRUE(refs_.append(ref));
  }

  return true;
}

void SharedArrayRawBufferRefs::takeOwnership(SharedArrayRawBufferRefs&& other) {
  MOZ_ASSERT(refs_.empty());
  refs_ = std::move(other.refs_);
}

void SharedArrayRawBufferRefs::releaseAll() {
  for (auto ref : refs_) {
    ref->dropReference();
  }
  refs_.clear();
}

// SCOutput provides an interface to write raw data -- eg uint64_ts, doubles,
// arrays of bytes -- into a structured clone data output stream. It also knows
// how to free any transferable data within that stream.
//
// Note that it contains a full JSStructuredCloneData object, which holds the
// callbacks necessary to read/write/transfer/free the data. For the purpose of
// this class, only the freeTransfer callback is relevant; the rest of the
// callbacks are used by the higher-level JSStructuredCloneWriter interface.
struct SCOutput {
 public:
  using Iter = BufferIterator<uint64_t, SystemAllocPolicy>;

  SCOutput(JSContext* cx, JS::StructuredCloneScope scope);

  JSContext* context() const { return cx; }
  JS::StructuredCloneScope scope() const { return buf.scope(); }
  void sameProcessScopeRequired() { buf.sameProcessScopeRequired(); }

  [[nodiscard]] bool write(uint64_t u);
  [[nodiscard]] bool writePair(uint32_t tag, uint32_t data);
  [[nodiscard]] bool writeDouble(double d);
  [[nodiscard]] bool writeBytes(const void* p, size_t nbytes);
  [[nodiscard]] bool writeChars(const Latin1Char* p, size_t nchars);
  [[nodiscard]] bool writeChars(const char16_t* p, size_t nchars);

  template <class T>
  [[nodiscard]] bool writeArray(const T* p, size_t nelems);

  void setCallbacks(const JSStructuredCloneCallbacks* callbacks, void* closure,
                    OwnTransferablePolicy policy) {
    buf.setCallbacks(callbacks, closure, policy);
  }
  void extractBuffer(JSStructuredCloneData* data) { *data = std::move(buf); }
  void discardTransferables();

  uint64_t tell() const { return buf.Size(); }
  uint64_t count() const { return buf.Size() / sizeof(uint64_t); }
  Iter iter() { return Iter(buf); }

  size_t offset(Iter dest) { return dest - iter(); }

  JSContext* cx;
  JSStructuredCloneData buf;
};

class SCInput {
  typedef js::BufferIterator<uint64_t, SystemAllocPolicy> BufferIterator;

 public:
  SCInput(JSContext* cx, const JSStructuredCloneData& data);

  JSContext* context() const { return cx; }

  static void getPtr(uint64_t data, void** ptr);
  static void getPair(uint64_t data, uint32_t* tagp, uint32_t* datap);

  [[nodiscard]] bool read(uint64_t* p);
  [[nodiscard]] bool readPair(uint32_t* tagp, uint32_t* datap);
  [[nodiscard]] bool readDouble(double* p);
  [[nodiscard]] bool readBytes(void* p, size_t nbytes);
  [[nodiscard]] bool readChars(Latin1Char* p, size_t nchars);
  [[nodiscard]] bool readChars(char16_t* p, size_t nchars);
  [[nodiscard]] bool readPtr(void**);

  [[nodiscard]] bool get(uint64_t* p);
  [[nodiscard]] bool getPair(uint32_t* tagp, uint32_t* datap);

  const BufferIterator& tell() const { return point; }
  void seekTo(const BufferIterator& pos) { point = pos; }
  [[nodiscard]] bool seekBy(size_t pos) {
    if (!point.advance(pos)) {
      reportTruncated();
      return false;
    }
    return true;
  }

  template <class T>
  [[nodiscard]] bool readArray(T* p, size_t nelems);

  bool reportTruncated() {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA, "truncated");
    return false;
  }

 private:
  void staticAssertions() {
    static_assert(sizeof(char16_t) == 2);
    static_assert(sizeof(uint32_t) == 4);
  }

  JSContext* cx;
  BufferIterator point;
};

}  // namespace js

struct JSStructuredCloneReader {
 public:
  explicit JSStructuredCloneReader(SCInput& in, JS::StructuredCloneScope scope,
                                   const JS::CloneDataPolicy& cloneDataPolicy,
                                   const JSStructuredCloneCallbacks* cb,
                                   void* cbClosure)
      : in(in),
        allowedScope(scope),
        cloneDataPolicy(cloneDataPolicy),
        objs(in.context()),
        allObjs(in.context()),
        numItemsRead(0),
        callbacks(cb),
        closure(cbClosure) {}

  SCInput& input() { return in; }
  bool read(MutableHandleValue vp, size_t nbytes);

 private:
  JSContext* context() { return in.context(); }

  bool readHeader();
  bool readTransferMap();

  template <typename CharT>
  JSString* readStringImpl(uint32_t nchars, gc::InitialHeap heap);
  JSString* readString(uint32_t data, gc::InitialHeap heap = gc::DefaultHeap);

  BigInt* readBigInt(uint32_t data);

  [[nodiscard]] bool readTypedArray(uint32_t arrayType, uint64_t nelems,
                                    MutableHandleValue vp, bool v1Read = false);
  [[nodiscard]] bool readDataView(uint64_t byteLength, MutableHandleValue vp);
  [[nodiscard]] bool readArrayBuffer(StructuredDataType type, uint32_t data,
                                     MutableHandleValue vp);
  [[nodiscard]] bool readSharedArrayBuffer(MutableHandleValue vp);
  [[nodiscard]] bool readSharedWasmMemory(uint32_t nbytes,
                                          MutableHandleValue vp);
  [[nodiscard]] bool readV1ArrayBuffer(uint32_t arrayType, uint32_t nelems,
                                       MutableHandleValue vp);
  JSObject* readSavedFrame(uint32_t principalsTag);
  [[nodiscard]] bool startRead(MutableHandleValue vp,
                               gc::InitialHeap strHeap = gc::DefaultHeap);

  SCInput& in;

  // The widest scope that the caller will accept, where
  // SameProcess is the widest (it can store anything it wants)
  // and DifferentProcess is the narrowest (it cannot contain pointers and must
  // be valid cross-process.)
  JS::StructuredCloneScope allowedScope;

  const JS::CloneDataPolicy cloneDataPolicy;

  // Stack of objects with properties remaining to be read.
  RootedValueVector objs;

  // Array of all objects read during this deserialization, for resolving
  // backreferences.
  //
  // For backreferences to work correctly, objects must be added to this
  // array in exactly the order expected by the version of the Writer that
  // created the serialized data, even across years and format versions. This
  // is usually no problem, since both algorithms do a single linear pass
  // over the serialized data. There is one hitch; see readTypedArray.
  //
  // The values in this vector are objects, except it can temporarily have
  // one `undefined` placeholder value (the readTypedArray hack).
  RootedValueVector allObjs;

  size_t numItemsRead;

  // The user defined callbacks that will be used for cloning.
  const JSStructuredCloneCallbacks* callbacks;

  // Any value passed to JS_ReadStructuredClone.
  void* closure;

  friend bool JS_ReadTypedArray(JSStructuredCloneReader* r,
                                MutableHandleValue vp);
};

struct JSStructuredCloneWriter {
 public:
  explicit JSStructuredCloneWriter(JSContext* cx,
                                   JS::StructuredCloneScope scope,
                                   const JS::CloneDataPolicy& cloneDataPolicy,
                                   const JSStructuredCloneCallbacks* cb,
                                   void* cbClosure, const Value& tVal)
      : out(cx, scope),
        callbacks(cb),
        closure(cbClosure),
        objs(out.context()),
        counts(out.context()),
        objectEntries(out.context()),
        otherEntries(out.context()),
        memory(out.context()),
        transferable(out.context(), tVal),
        transferableObjects(out.context(), TransferableObjectsSet(cx)),
        cloneDataPolicy(cloneDataPolicy) {
    out.setCallbacks(cb, cbClosure, OwnTransferablePolicy::NoTransferables);
  }

  ~JSStructuredCloneWriter();

  bool init() {
    return parseTransferable() && writeHeader() && writeTransferMap();
  }

  bool write(HandleValue v);

  SCOutput& output() { return out; }

  void extractBuffer(JSStructuredCloneData* newData) {
    out.extractBuffer(newData);
  }

 private:
  JSStructuredCloneWriter() = delete;
  JSStructuredCloneWriter(const JSStructuredCloneWriter&) = delete;

  JSContext* context() { return out.context(); }

  bool writeHeader();
  bool writeTransferMap();

  bool writeString(uint32_t tag, JSString* str);
  bool writeBigInt(uint32_t tag, BigInt* bi);
  bool writeArrayBuffer(HandleObject obj);
  bool writeTypedArray(HandleObject obj);
  bool writeDataView(HandleObject obj);
  bool writeSharedArrayBuffer(HandleObject obj);
  bool writeSharedWasmMemory(HandleObject obj);
  bool startObject(HandleObject obj, bool* backref);
  bool startWrite(HandleValue v);
  bool traverseObject(HandleObject obj, ESClass cls);
  bool traverseMap(HandleObject obj);
  bool traverseSet(HandleObject obj);
  bool traverseSavedFrame(HandleObject obj);

  template <typename... Args>
  bool reportDataCloneError(uint32_t errorId, Args&&... aArgs);

  bool parseTransferable();
  bool transferOwnership();

  inline void checkStack();

  SCOutput out;

  // The user defined callbacks that will be used to signal cloning, in some
  // cases.
  const JSStructuredCloneCallbacks* callbacks;

  // Any value passed to the callbacks.
  void* closure;

  // Vector of objects with properties remaining to be written.
  //
  // NB: These can span multiple compartments, so the compartment must be
  // entered before any manipulation is performed.
  RootedValueVector objs;

  // counts[i] is the number of entries of objs[i] remaining to be written.
  // counts.length() == objs.length() and sum(counts) == entries.length().
  Vector<size_t> counts;

  // For JSObject: Property IDs as value
  RootedIdVector objectEntries;

  // For Map: Key followed by value
  // For Set: Key
  // For SavedFrame: parent SavedFrame
  RootedValueVector otherEntries;

  // The "memory" list described in the HTML5 internal structured cloning
  // algorithm.  memory is a superset of objs; items are never removed from
  // Memory until a serialization operation is finished
  using CloneMemory =
      GCHashMap<JSObject*, uint32_t, MovableCellHasher<JSObject*>,
                SystemAllocPolicy>;
  Rooted<CloneMemory> memory;

  struct TransferableObjectsHasher : public DefaultHasher<JSObject*> {
    static inline HashNumber hash(const Lookup& l) {
      return DefaultHasher<JSObject*>::hash(l);
    }
  };

  // Set of transferable objects
  RootedValue transferable;
  typedef GCHashSet<JSObject*, TransferableObjectsHasher>
      TransferableObjectsSet;
  Rooted<TransferableObjectsSet> transferableObjects;

  const JS::CloneDataPolicy cloneDataPolicy;

  friend bool JS_WriteString(JSStructuredCloneWriter* w, HandleString str);
  friend bool JS_WriteTypedArray(JSStructuredCloneWriter* w, HandleValue v);
  friend bool JS_ObjectNotWritten(JSStructuredCloneWriter* w, HandleObject obj);
};

JS_PUBLIC_API uint64_t js::GetSCOffset(JSStructuredCloneWriter* writer) {
  MOZ_ASSERT(writer);
  return writer->output().count() * sizeof(uint64_t);
}

static_assert(SCTAG_END_OF_BUILTIN_TYPES <= JS_SCTAG_USER_MIN);
static_assert(JS_SCTAG_USER_MIN <= JS_SCTAG_USER_MAX);
static_assert(Scalar::Int8 == 0);

template <typename... Args>
static void ReportDataCloneError(JSContext* cx,
                                 const JSStructuredCloneCallbacks* callbacks,
                                 uint32_t errorId, void* closure,
                                 Args&&... aArgs) {
  unsigned errorNumber;
  switch (errorId) {
    case JS_SCERR_DUP_TRANSFERABLE:
      errorNumber = JSMSG_SC_DUP_TRANSFERABLE;
      break;

    case JS_SCERR_TRANSFERABLE:
      errorNumber = JSMSG_SC_NOT_TRANSFERABLE;
      break;

    case JS_SCERR_UNSUPPORTED_TYPE:
      errorNumber = JSMSG_SC_UNSUPPORTED_TYPE;
      break;

    case JS_SCERR_SHMEM_TRANSFERABLE:
      errorNumber = JSMSG_SC_SHMEM_TRANSFERABLE;
      break;

    case JS_SCERR_TYPED_ARRAY_DETACHED:
      errorNumber = JSMSG_TYPED_ARRAY_DETACHED;
      break;

    case JS_SCERR_WASM_NO_TRANSFER:
      errorNumber = JSMSG_WASM_NO_TRANSFER;
      break;

    case JS_SCERR_NOT_CLONABLE:
      errorNumber = JSMSG_SC_NOT_CLONABLE;
      break;

    case JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP:
      errorNumber = JSMSG_SC_NOT_CLONABLE_WITH_COOP_COEP;
      break;

    default:
      MOZ_CRASH("Unkown errorId");
      break;
  }

  if (callbacks && callbacks->reportError) {
    MOZ_RELEASE_ASSERT(!cx->isExceptionPending());

    JSErrorReport report;
    report.errorNumber = errorNumber;
    // Get js error message if it's possible and propagate it through callback.
    if (JS_ExpandErrorArgumentsASCII(cx, GetErrorMessage, errorNumber, &report,
                                     std::forward<Args>(aArgs)...) &&
        report.message()) {
      callbacks->reportError(cx, errorId, closure, report.message().c_str());
    } else {
      ReportOutOfMemory(cx);

      callbacks->reportError(cx, errorId, closure, "");
    }

    return;
  }

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNumber,
                            std::forward<Args>(aArgs)...);
}

bool WriteStructuredClone(JSContext* cx, HandleValue v,
                          JSStructuredCloneData* bufp,
                          JS::StructuredCloneScope scope,
                          const JS::CloneDataPolicy& cloneDataPolicy,
                          const JSStructuredCloneCallbacks* cb, void* cbClosure,
                          const Value& transferable) {
  JSStructuredCloneWriter w(cx, scope, cloneDataPolicy, cb, cbClosure,
                            transferable);
  if (!w.init()) {
    return false;
  }
  if (!w.write(v)) {
    return false;
  }
  w.extractBuffer(bufp);
  return true;
}

bool ReadStructuredClone(JSContext* cx, const JSStructuredCloneData& data,
                         JS::StructuredCloneScope scope, MutableHandleValue vp,
                         const JS::CloneDataPolicy& cloneDataPolicy,
                         const JSStructuredCloneCallbacks* cb,
                         void* cbClosure) {
  SCInput in(cx, data);
  JSStructuredCloneReader r(in, scope, cloneDataPolicy, cb, cbClosure);
  return r.read(vp, data.Size());
}

static bool StructuredCloneHasTransferObjects(
    const JSStructuredCloneData& data) {
  if (data.Size() < sizeof(uint64_t)) {
    return false;
  }

  uint64_t u;
  BufferIterator<uint64_t, SystemAllocPolicy> iter(data);
  MOZ_ALWAYS_TRUE(iter.readBytes(reinterpret_cast<char*>(&u), sizeof(u)));
  uint32_t tag = uint32_t(u >> 32);
  return (tag == SCTAG_TRANSFER_MAP_HEADER);
}

namespace js {

SCInput::SCInput(JSContext* cx, const JSStructuredCloneData& data)
    : cx(cx), point(data) {
  static_assert(JSStructuredCloneData::BufferList::kSegmentAlignment % 8 == 0,
                "structured clone buffer reads should be aligned");
  MOZ_ASSERT(data.Size() % 8 == 0);
}

bool SCInput::read(uint64_t* p) {
  if (!point.canPeek()) {
    *p = 0;  // initialize to shut GCC up
    return reportTruncated();
  }
  *p = NativeEndian::swapFromLittleEndian(point.peek());
  MOZ_ALWAYS_TRUE(point.advance());
  return true;
}

bool SCInput::readPair(uint32_t* tagp, uint32_t* datap) {
  uint64_t u;
  bool ok = read(&u);
  if (ok) {
    *tagp = uint32_t(u >> 32);
    *datap = uint32_t(u);
  }
  return ok;
}

bool SCInput::get(uint64_t* p) {
  if (!point.canPeek()) {
    return reportTruncated();
  }
  *p = NativeEndian::swapFromLittleEndian(point.peek());
  return true;
}

bool SCInput::getPair(uint32_t* tagp, uint32_t* datap) {
  uint64_t u = 0;
  if (!get(&u)) {
    return false;
  }

  *tagp = uint32_t(u >> 32);
  *datap = uint32_t(u);
  return true;
}

void SCInput::getPair(uint64_t data, uint32_t* tagp, uint32_t* datap) {
  uint64_t u = NativeEndian::swapFromLittleEndian(data);
  *tagp = uint32_t(u >> 32);
  *datap = uint32_t(u);
}

bool SCInput::readDouble(double* p) {
  uint64_t u;
  if (!read(&u)) {
    return false;
  }
  *p = CanonicalizeNaN(mozilla::BitwiseCast<double>(u));
  return true;
}

template <typename T>
static void swapFromLittleEndianInPlace(T* ptr, size_t nelems) {
  if (nelems > 0) {
    NativeEndian::swapFromLittleEndianInPlace(ptr, nelems);
  }
}

template <>
void swapFromLittleEndianInPlace(uint8_t* ptr, size_t nelems) {}

// Data is packed into an integral number of uint64_t words. Compute the
// padding required to finish off the final word.
static size_t ComputePadding(size_t nelems, size_t elemSize) {
  // We want total length mod 8, where total length is nelems * sizeof(T),
  // but that might overflow. So reduce nelems to nelems mod 8, since we are
  // going to be doing a mod 8 later anyway.
  size_t leftoverLength = (nelems % sizeof(uint64_t)) * elemSize;
  return (-leftoverLength) & (sizeof(uint64_t) - 1);
}

template <class T>
bool SCInput::readArray(T* p, size_t nelems) {
  if (!nelems) {
    return true;
  }

  static_assert(sizeof(uint64_t) % sizeof(T) == 0);

  // Fail if nelems is so huge that computing the full size will overflow.
  mozilla::CheckedInt<size_t> size =
      mozilla::CheckedInt<size_t>(nelems) * sizeof(T);
  if (!size.isValid()) {
    return reportTruncated();
  }

  if (!point.readBytes(reinterpret_cast<char*>(p), size.value())) {
    // To avoid any way in which uninitialized data could escape, zero the array
    // if filling it failed.
    std::uninitialized_fill_n(p, nelems, 0);
    return false;
  }

  swapFromLittleEndianInPlace(p, nelems);

  point += ComputePadding(nelems, sizeof(T));

  return true;
}

bool SCInput::readBytes(void* p, size_t nbytes) {
  return readArray((uint8_t*)p, nbytes);
}

bool SCInput::readChars(Latin1Char* p, size_t nchars) {
  static_assert(sizeof(Latin1Char) == sizeof(uint8_t),
                "Latin1Char must fit in 1 byte");
  return readBytes(p, nchars);
}

bool SCInput::readChars(char16_t* p, size_t nchars) {
  MOZ_ASSERT(sizeof(char16_t) == sizeof(uint16_t));
  return readArray((uint16_t*)p, nchars);
}

void SCInput::getPtr(uint64_t data, void** ptr) {
  *ptr = reinterpret_cast<void*>(NativeEndian::swapFromLittleEndian(data));
}

bool SCInput::readPtr(void** p) {
  uint64_t u;
  if (!read(&u)) {
    return false;
  }
  *p = reinterpret_cast<void*>(u);
  return true;
}

SCOutput::SCOutput(JSContext* cx, JS::StructuredCloneScope scope)
    : cx(cx), buf(scope) {}

bool SCOutput::write(uint64_t u) {
  uint64_t v = NativeEndian::swapToLittleEndian(u);
  if (!buf.AppendBytes(reinterpret_cast<char*>(&v), sizeof(u))) {
    ReportOutOfMemory(context());
    return false;
  }
  return true;
}

bool SCOutput::writePair(uint32_t tag, uint32_t data) {
  // As it happens, the tag word appears after the data word in the output.
  // This is because exponents occupy the last 2 bytes of doubles on the
  // little-endian platforms we care most about.
  //
  // For example, TrueValue() is written using writePair(SCTAG_BOOLEAN, 1).
  // PairToUInt64 produces the number 0xFFFF000200000001.
  // That is written out as the bytes 01 00 00 00 02 00 FF FF.
  return write(PairToUInt64(tag, data));
}

static inline double ReinterpretPairAsDouble(uint32_t tag, uint32_t data) {
  return BitwiseCast<double>(PairToUInt64(tag, data));
}

bool SCOutput::writeDouble(double d) {
  return write(BitwiseCast<uint64_t>(CanonicalizeNaN(d)));
}

template <class T>
bool SCOutput::writeArray(const T* p, size_t nelems) {
  static_assert(8 % sizeof(T) == 0);
  static_assert(sizeof(uint64_t) % sizeof(T) == 0);

  if (nelems == 0) {
    return true;
  }

  for (size_t i = 0; i < nelems; i++) {
    T value = NativeEndian::swapToLittleEndian(p[i]);
    if (!buf.AppendBytes(reinterpret_cast<char*>(&value), sizeof(value))) {
      return false;
    }
  }

  // Zero-pad to 8 bytes boundary.
  size_t padbytes = ComputePadding(nelems, sizeof(T));
  char zeroes[sizeof(uint64_t)] = {0};
  if (!buf.AppendBytes(zeroes, padbytes)) {
    return false;
  }

  return true;
}

template <>
bool SCOutput::writeArray<uint8_t>(const uint8_t* p, size_t nelems) {
  if (nelems == 0) {
    return true;
  }

  if (!buf.AppendBytes(reinterpret_cast<const char*>(p), nelems)) {
    return false;
  }

  // zero-pad to 8 bytes boundary
  size_t padbytes = ComputePadding(nelems, 1);
  char zeroes[sizeof(uint64_t)] = {0};
  if (!buf.AppendBytes(zeroes, padbytes)) {
    return false;
  }

  return true;
}

bool SCOutput::writeBytes(const void* p, size_t nbytes) {
  return writeArray((const uint8_t*)p, nbytes);
}

bool SCOutput::writeChars(const char16_t* p, size_t nchars) {
  static_assert(sizeof(char16_t) == sizeof(uint16_t),
                "required so that treating char16_t[] memory as uint16_t[] "
                "memory is permissible");
  return writeArray((const uint16_t*)p, nchars);
}

bool SCOutput::writeChars(const Latin1Char* p, size_t nchars) {
  static_assert(sizeof(Latin1Char) == sizeof(uint8_t),
                "Latin1Char must fit in 1 byte");
  return writeBytes(p, nchars);
}

void SCOutput::discardTransferables() { buf.discardTransferables(); }

}  // namespace js

JSStructuredCloneData::~JSStructuredCloneData() { discardTransferables(); }

// If the buffer contains Transferables, free them. Note that custom
// Transferables will use the JSStructuredCloneCallbacks::freeTransfer() to
// delete their transferables.
void JSStructuredCloneData::discardTransferables() {
  if (!Size()) {
    return;
  }

  if (ownTransferables_ != OwnTransferablePolicy::OwnsTransferablesIfAny) {
    return;
  }

  // DifferentProcess clones cannot contain pointers, so nothing needs to be
  // released.
  if (scope() == JS::StructuredCloneScope::DifferentProcess) {
    return;
  }

  FreeTransferStructuredCloneOp freeTransfer = nullptr;
  if (callbacks_) {
    freeTransfer = callbacks_->freeTransfer;
  }

  auto point = BufferIterator<uint64_t, SystemAllocPolicy>(*this);
  if (point.done()) {
    return;  // Empty buffer
  }

  uint32_t tag, data;
  MOZ_RELEASE_ASSERT(point.canPeek());
  SCInput::getPair(point.peek(), &tag, &data);
  MOZ_ALWAYS_TRUE(point.advance());

  if (tag == SCTAG_HEADER) {
    if (point.done()) {
      return;
    }

    MOZ_RELEASE_ASSERT(point.canPeek());
    SCInput::getPair(point.peek(), &tag, &data);
    MOZ_ALWAYS_TRUE(point.advance());
  }

  if (tag != SCTAG_TRANSFER_MAP_HEADER) {
    return;
  }

  if (TransferableMapHeader(data) == SCTAG_TM_TRANSFERRED) {
    return;
  }

  // freeTransfer should not GC
  JS::AutoSuppressGCAnalysis nogc;

  if (point.done()) {
    return;
  }

  MOZ_RELEASE_ASSERT(point.canPeek());
  uint64_t numTransferables = NativeEndian::swapFromLittleEndian(point.peek());
  MOZ_ALWAYS_TRUE(point.advance());
  while (numTransferables--) {
    if (!point.canPeek()) {
      return;
    }

    uint32_t ownership;
    SCInput::getPair(point.peek(), &tag, &ownership);
    MOZ_ALWAYS_TRUE(point.advance());
    MOZ_ASSERT(tag >= SCTAG_TRANSFER_MAP_PENDING_ENTRY);
    if (!point.canPeek()) {
      return;
    }

    void* content;
    SCInput::getPtr(point.peek(), &content);
    MOZ_ALWAYS_TRUE(point.advance());
    if (!point.canPeek()) {
      return;
    }

    uint64_t extraData = NativeEndian::swapFromLittleEndian(point.peek());
    MOZ_ALWAYS_TRUE(point.advance());

    if (ownership < JS::SCTAG_TMO_FIRST_OWNED) {
      continue;
    }

    if (ownership == JS::SCTAG_TMO_ALLOC_DATA) {
      js_free(content);
    } else if (ownership == JS::SCTAG_TMO_MAPPED_DATA) {
      JS::ReleaseMappedArrayBufferContents(content, extraData);
    } else if (freeTransfer) {
      freeTransfer(tag, JS::TransferableOwnership(ownership), content,
                   extraData, closure_);
    } else {
      MOZ_ASSERT(false, "unknown ownership");
    }
  }
}

static_assert(JSString::MAX_LENGTH < UINT32_MAX);

JSStructuredCloneWriter::~JSStructuredCloneWriter() {
  // Free any transferable data left lying around in the buffer
  if (out.count()) {
    out.discardTransferables();
  }
}

bool JSStructuredCloneWriter::parseTransferable() {
  // NOTE: The transferables set is tested for non-emptiness at various
  //       junctures in structured cloning, so this set must be initialized
  //       by this method in all non-error cases.
  MOZ_ASSERT(transferableObjects.empty(),
             "parseTransferable called with stale data");

  if (transferable.isNull() || transferable.isUndefined()) {
    return true;
  }

  if (!transferable.isObject()) {
    return reportDataCloneError(JS_SCERR_TRANSFERABLE);
  }

  JSContext* cx = context();
  RootedObject array(cx, &transferable.toObject());
  bool isArray;
  if (!JS::IsArrayObject(cx, array, &isArray)) {
    return false;
  }
  if (!isArray) {
    return reportDataCloneError(JS_SCERR_TRANSFERABLE);
  }

  uint32_t length;
  if (!JS::GetArrayLength(cx, array, &length)) {
    return false;
  }

  // Initialize the set for the provided array's length.
  if (!transferableObjects.reserve(length)) {
    return false;
  }

  if (length == 0) {
    return true;
  }

  RootedValue v(context());
  RootedObject tObj(context());

  for (uint32_t i = 0; i < length; ++i) {
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    if (!JS_GetElement(cx, array, i, &v)) {
      return false;
    }

    if (!v.isObject()) {
      return reportDataCloneError(JS_SCERR_TRANSFERABLE);
    }
    tObj = &v.toObject();

    RootedObject unwrappedObj(cx, CheckedUnwrapStatic(tObj));
    if (!unwrappedObj) {
      ReportAccessDenied(cx);
      return false;
    }

    // Shared memory cannot be transferred because it is not possible (nor
    // desirable) to detach the memory in agents that already hold a
    // reference to it.

    if (unwrappedObj->is<SharedArrayBufferObject>()) {
      return reportDataCloneError(JS_SCERR_SHMEM_TRANSFERABLE);
    }

    else if (unwrappedObj->is<WasmMemoryObject>()) {
      if (unwrappedObj->as<WasmMemoryObject>().isShared()) {
        return reportDataCloneError(JS_SCERR_SHMEM_TRANSFERABLE);
      }
    }

    // External array buffers may be able to be transferred in the future,
    // but that is not currently implemented.

    else if (unwrappedObj->is<ArrayBufferObject>()) {
      if (unwrappedObj->as<ArrayBufferObject>().isExternal()) {
        return reportDataCloneError(JS_SCERR_TRANSFERABLE);
      }
    }

    else {
      if (!out.buf.callbacks_ || !out.buf.callbacks_->canTransfer) {
        return reportDataCloneError(JS_SCERR_TRANSFERABLE);
      }

      JSAutoRealm ar(cx, unwrappedObj);
      bool sameProcessScopeRequired = false;
      if (!out.buf.callbacks_->canTransfer(
              cx, unwrappedObj, &sameProcessScopeRequired, out.buf.closure_)) {
        return false;
      }

      if (sameProcessScopeRequired) {
        output().sameProcessScopeRequired();
      }
    }

    // No duplicates allowed
    auto p = transferableObjects.lookupForAdd(tObj);
    if (p) {
      return reportDataCloneError(JS_SCERR_DUP_TRANSFERABLE);
    }

    if (!transferableObjects.add(p, tObj)) {
      return false;
    }
  }

  return true;
}

template <typename... Args>
bool JSStructuredCloneWriter::reportDataCloneError(uint32_t errorId,
                                                   Args&&... aArgs) {
  ReportDataCloneError(context(), out.buf.callbacks_, errorId, out.buf.closure_,
                       std::forward<Args>(aArgs)...);
  return false;
}

bool JSStructuredCloneWriter::writeString(uint32_t tag, JSString* str) {
  JSLinearString* linear = str->ensureLinear(context());
  if (!linear) {
    return false;
  }

  static_assert(JSString::MAX_LENGTH <= INT32_MAX,
                "String length must fit in 31 bits");

  uint32_t length = linear->length();
  uint32_t lengthAndEncoding =
      length | (uint32_t(linear->hasLatin1Chars()) << 31);
  if (!out.writePair(tag, lengthAndEncoding)) {
    return false;
  }

  JS::AutoCheckCannotGC nogc;
  return linear->hasLatin1Chars()
             ? out.writeChars(linear->latin1Chars(nogc), length)
             : out.writeChars(linear->twoByteChars(nogc), length);
}

bool JSStructuredCloneWriter::writeBigInt(uint32_t tag, BigInt* bi) {
  bool signBit = bi->isNegative();
  size_t length = bi->digitLength();
  // The length must fit in 31 bits to leave room for a sign bit.
  if (length > size_t(INT32_MAX)) {
    return false;
  }
  uint32_t lengthAndSign = length | (static_cast<uint32_t>(signBit) << 31);

  if (!out.writePair(tag, lengthAndSign)) {
    return false;
  }
  return out.writeArray(bi->digits().data(), length);
}

inline void JSStructuredCloneWriter::checkStack() {
#ifdef DEBUG
  // To avoid making serialization O(n^2), limit stack-checking at 10.
  const size_t MAX = 10;

  size_t limit = std::min(counts.length(), MAX);
  MOZ_ASSERT(objs.length() == counts.length());
  size_t total = 0;
  for (size_t i = 0; i < limit; i++) {
    MOZ_ASSERT(total + counts[i] >= total);
    total += counts[i];
  }
  if (counts.length() <= MAX) {
    MOZ_ASSERT(total == objectEntries.length() + otherEntries.length());
  } else {
    MOZ_ASSERT(total <= objectEntries.length() + otherEntries.length());
  }

  size_t j = objs.length();
  for (size_t i = 0; i < limit; i++) {
    --j;
    MOZ_ASSERT(memory.has(&objs[j].toObject()));
  }
#endif
}

/*
 * Write out a typed array. Note that post-v1 structured clone buffers do not
 * perform endianness conversion on stored data, so multibyte typed arrays
 * cannot be deserialized into a different endianness machine. Endianness
 * conversion would prevent sharing ArrayBuffers: if you have Int8Array and
 * Int16Array views of the same ArrayBuffer, should the data bytes be
 * byte-swapped when writing or not? The Int8Array requires them to not be
 * swapped; the Int16Array requires that they are.
 */
bool JSStructuredCloneWriter::writeTypedArray(HandleObject obj) {
  Rooted<TypedArrayObject*> tarr(context(),
                                 obj->maybeUnwrapAs<TypedArrayObject>());
  JSAutoRealm ar(context(), tarr);

  if (!TypedArrayObject::ensureHasBuffer(context(), tarr)) {
    return false;
  }

  if (!out.writePair(SCTAG_TYPED_ARRAY_OBJECT, uint32_t(tarr->type()))) {
    return false;
  }

  uint64_t nelems = tarr->length();
  if (!out.write(nelems)) {
    return false;
  }

  // Write out the ArrayBuffer tag and contents
  RootedValue val(context(), tarr->bufferValue());
  if (!startWrite(val)) {
    return false;
  }

  uint64_t byteOffset = tarr->byteOffset();
  return out.write(byteOffset);
}

bool JSStructuredCloneWriter::writeDataView(HandleObject obj) {
  Rooted<DataViewObject*> view(context(), obj->maybeUnwrapAs<DataViewObject>());
  JSAutoRealm ar(context(), view);

  if (!out.writePair(SCTAG_DATA_VIEW_OBJECT, 0)) {
    return false;
  }

  uint64_t byteLength = view->byteLength();
  if (!out.write(byteLength)) {
    return false;
  }

  // Write out the ArrayBuffer tag and contents
  RootedValue val(context(), view->bufferValue());
  if (!startWrite(val)) {
    return false;
  }

  uint64_t byteOffset = view->byteOffset();
  return out.write(byteOffset);
}

bool JSStructuredCloneWriter::writeArrayBuffer(HandleObject obj) {
  Rooted<ArrayBufferObject*> buffer(context(),
                                    obj->maybeUnwrapAs<ArrayBufferObject>());
  JSAutoRealm ar(context(), buffer);

  if (!out.writePair(SCTAG_ARRAY_BUFFER_OBJECT, 0)) {
    return false;
  }

  uint64_t byteLength = buffer->byteLength();
  if (!out.write(byteLength)) {
    return false;
  }

  return out.writeBytes(buffer->dataPointer(), byteLength);
}

bool JSStructuredCloneWriter::writeSharedArrayBuffer(HandleObject obj) {
  MOZ_ASSERT(obj->canUnwrapAs<SharedArrayBufferObject>());

  if (!cloneDataPolicy.areSharedMemoryObjectsAllowed()) {
    auto error = context()->realm()->creationOptions().getCoopAndCoepEnabled()
                     ? JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP
                     : JS_SCERR_NOT_CLONABLE;
    reportDataCloneError(error, "SharedArrayBuffer");
    return false;
  }

  output().sameProcessScopeRequired();

  // We must not transmit SAB pointers (including for WebAssembly.Memory)
  // cross-process.  The cloneDataPolicy should have guarded against this;
  // since it did not then throw, with a very explicit message.

  if (output().scope() > JS::StructuredCloneScope::SameProcess) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_SHMEM_POLICY);
    return false;
  }

  Rooted<SharedArrayBufferObject*> sharedArrayBuffer(
      context(), obj->maybeUnwrapAs<SharedArrayBufferObject>());
  SharedArrayRawBuffer* rawbuf = sharedArrayBuffer->rawBufferObject();

  if (!out.buf.refsHeld_.acquire(context(), rawbuf)) {
    return false;
  }

  // We must serialize the length so that the buffer object arrives in the
  // receiver with the same length, and not with the length read from the
  // rawbuf - that length can be different, and it can change at any time.

  intptr_t p = reinterpret_cast<intptr_t>(rawbuf);
  uint64_t byteLength = sharedArrayBuffer->byteLength();
  if (!(out.writePair(SCTAG_SHARED_ARRAY_BUFFER_OBJECT,
                      static_cast<uint32_t>(sizeof(p))) &&
        out.writeBytes(&byteLength, sizeof(byteLength)) &&
        out.writeBytes(&p, sizeof(p)))) {
    return false;
  }

  if (callbacks && callbacks->sabCloned &&
      !callbacks->sabCloned(context(), /*receiving=*/false, closure)) {
    return false;
  }

  return true;
}

bool JSStructuredCloneWriter::writeSharedWasmMemory(HandleObject obj) {
  MOZ_ASSERT(obj->canUnwrapAs<WasmMemoryObject>());

  // Check the policy here so that we can report a sane error.
  if (!cloneDataPolicy.areSharedMemoryObjectsAllowed()) {
    auto error = context()->realm()->creationOptions().getCoopAndCoepEnabled()
                     ? JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP
                     : JS_SCERR_NOT_CLONABLE;
    reportDataCloneError(error, "WebAssembly.Memory");
    return false;
  }

  // If this changes, might need to change what we write.
  MOZ_ASSERT(WasmMemoryObject::RESERVED_SLOTS == 2);

  Rooted<WasmMemoryObject*> memoryObj(context(),
                                      &obj->unwrapAs<WasmMemoryObject>());
  Rooted<SharedArrayBufferObject*> sab(
      context(), &memoryObj->buffer().as<SharedArrayBufferObject>());

  return out.writePair(SCTAG_SHARED_WASM_MEMORY_OBJECT, 0) &&
         writeSharedArrayBuffer(sab);
}

bool JSStructuredCloneWriter::startObject(HandleObject obj, bool* backref) {
  // Handle cycles in the object graph.
  CloneMemory::AddPtr p = memory.lookupForAdd(obj);
  if ((*backref = p.found())) {
    return out.writePair(SCTAG_BACK_REFERENCE_OBJECT, p->value());
  }
  if (!memory.add(p, obj, memory.count())) {
    ReportOutOfMemory(context());
    return false;
  }

  if (memory.count() == UINT32_MAX) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_NEED_DIET, "object graph to serialize");
    return false;
  }

  return true;
}

static bool TryAppendNativeProperties(JSContext* cx, HandleObject obj,
                                      MutableHandleIdVector entries,
                                      size_t* properties, bool* optimized) {
  *optimized = false;

  if (!obj->is<NativeObject>()) {
    return true;
  }

  HandleNativeObject nobj = obj.as<NativeObject>();
  if (nobj->isIndexed() || nobj->is<TypedArrayObject>() ||
      nobj->getClass()->getNewEnumerate() || nobj->getClass()->getEnumerate()) {
    return true;
  }

  *optimized = true;

  size_t count = 0;
  // We iterate from the last to the first property, so the property names
  // are already in reverse order.
  for (ShapePropertyIter<NoGC> iter(nobj->shape()); !iter.done(); iter++) {
    jsid id = iter->key();

    // Ignore symbols and non-enumerable properties.
    if (!iter->enumerable() || id.isSymbol()) {
      continue;
    }

    MOZ_ASSERT(JSID_IS_STRING(id));
    if (!entries.append(id)) {
      return false;
    }

    count++;
  }

  // Add dense element ids in reverse order.
  for (uint32_t i = nobj->getDenseInitializedLength(); i > 0; --i) {
    if (nobj->getDenseElement(i - 1).isMagic(JS_ELEMENTS_HOLE)) {
      continue;
    }

    if (!entries.append(INT_TO_JSID(i - 1))) {
      return false;
    }

    count++;
  }

  *properties = count;
  return true;
}

// Objects are written as a "preorder" traversal of the object graph: object
// "headers" (the class tag and any data needed for initial construction) are
// visited first, then the children are recursed through (where children are
// properties, Set or Map entries, etc.). So for example
//
//     obj1 = { key1: { key1.1: val1.1, key1.2: val1.2 }, key2: {} }
//
// would be stored as:
//
//     <Object tag for obj1>
//       <key1 data>
//       <Object tag for key1's value>
//         <key1.1 data>
//         <val1.1 data>
//         <key1.2 data>
//         <val1.2 data>
//       <end-of-children marker for key1's value>
//       <key2 data>
//       <Object tag for key2's value>
//       <end-of-children marker for key2's value>
//     <end-of-children marker for obj1>
//
// This nests nicely (ie, an entire recursive value starts with its tag and
// ends with its end-of-children marker) and so it can be presented indented.
// But see traverseMap below for how this looks different for Maps.
bool JSStructuredCloneWriter::traverseObject(HandleObject obj, ESClass cls) {
  size_t count;
  bool optimized = false;
  if (!TryAppendNativeProperties(context(), obj, &objectEntries, &count,
                                 &optimized)) {
    return false;
  }

  if (!optimized) {
    // Get enumerable property ids and put them in reverse order so that they
    // will come off the stack in forward order.
    RootedIdVector properties(context());
    if (!GetPropertyKeys(context(), obj, JSITER_OWNONLY, &properties)) {
      return false;
    }

    for (size_t i = properties.length(); i > 0; --i) {
      jsid id = properties[i - 1];

      MOZ_ASSERT(JSID_IS_STRING(id) || JSID_IS_INT(id));
      if (!objectEntries.append(id)) {
        return false;
      }
    }

    count = properties.length();
  }

  // Push obj and count to the stack.
  if (!objs.append(ObjectValue(*obj)) || !counts.append(count)) {
    return false;
  }

  checkStack();

#if DEBUG
  ESClass cls2;
  if (!GetBuiltinClass(context(), obj, &cls2)) {
    return false;
  }
  MOZ_ASSERT(cls2 == cls);
#endif

  // Write the header for obj.
  if (cls == ESClass::Array) {
    uint32_t length = 0;
    if (!JS::GetArrayLength(context(), obj, &length)) {
      return false;
    }

    return out.writePair(SCTAG_ARRAY_OBJECT,
                         NativeEndian::swapToLittleEndian(length));
  }

  return out.writePair(SCTAG_OBJECT_OBJECT, 0);
}

// Use the same basic setup as for traverseObject, but now keys can themselves
// be complex objects. Keys and values are visited first via startWrite(), then
// the key's children (if any) are handled, then the value's children.
//
//     m = new Map();
//     m.set(key1 = ..., value1 = ...)
//
// where key1 and value2 are both objects would be stored as
//
//     <Map tag>
//     <key1 class tag>
//     <value1 class tag>
//     ...key1 data...
//     <end-of-children marker for key1>
//     ...value1 data...
//     <end-of-children marker for value1>
//     <end-of-children marker for Map>
//
// Notice how the end-of-children marker for key1 is sandwiched between the
// value1 beginning and end.
bool JSStructuredCloneWriter::traverseMap(HandleObject obj) {
  Rooted<GCVector<Value>> newEntries(context(), GCVector<Value>(context()));
  {
    // If there is no wrapper, the compartment munging is a no-op.
    RootedObject unwrapped(context(), obj->maybeUnwrapAs<MapObject>());
    MOZ_ASSERT(unwrapped);
    JSAutoRealm ar(context(), unwrapped);
    if (!MapObject::getKeysAndValuesInterleaved(unwrapped, &newEntries)) {
      return false;
    }
  }
  if (!context()->compartment()->wrap(context(), &newEntries)) {
    return false;
  }

  for (size_t i = newEntries.length(); i > 0; --i) {
    if (!otherEntries.append(newEntries[i - 1])) {
      return false;
    }
  }

  // Push obj and count to the stack.
  if (!objs.append(ObjectValue(*obj)) || !counts.append(newEntries.length())) {
    return false;
  }

  checkStack();

  // Write the header for obj.
  return out.writePair(SCTAG_MAP_OBJECT, 0);
}

// Similar to traverseMap, only there is a single value instead of a key and
// value, and thus no interleaving is possible: a value will be fully emitted
// before the next value is begun.
bool JSStructuredCloneWriter::traverseSet(HandleObject obj) {
  Rooted<GCVector<Value>> keys(context(), GCVector<Value>(context()));
  {
    // If there is no wrapper, the compartment munging is a no-op.
    RootedObject unwrapped(context(), obj->maybeUnwrapAs<SetObject>());
    MOZ_ASSERT(unwrapped);
    JSAutoRealm ar(context(), unwrapped);
    if (!SetObject::keys(context(), unwrapped, &keys)) {
      return false;
    }
  }
  if (!context()->compartment()->wrap(context(), &keys)) {
    return false;
  }

  for (size_t i = keys.length(); i > 0; --i) {
    if (!otherEntries.append(keys[i - 1])) {
      return false;
    }
  }

  // Push obj and count to the stack.
  if (!objs.append(ObjectValue(*obj)) || !counts.append(keys.length())) {
    return false;
  }

  checkStack();

  // Write the header for obj.
  return out.writePair(SCTAG_SET_OBJECT, 0);
}

bool JSStructuredCloneWriter::traverseSavedFrame(HandleObject obj) {
  RootedSavedFrame savedFrame(context(), obj->maybeUnwrapAs<SavedFrame>());
  MOZ_ASSERT(savedFrame);

  RootedObject parent(context(), savedFrame->getParent());
  if (!context()->compartment()->wrap(context(), &parent)) {
    return false;
  }

  if (!objs.append(ObjectValue(*obj)) ||
      !otherEntries.append(parent ? ObjectValue(*parent) : NullValue()) ||
      !counts.append(1)) {
    return false;
  }

  checkStack();

  // Write the SavedFrame tag and the SavedFrame's principals.

  if (savedFrame->getPrincipals() ==
      &ReconstructedSavedFramePrincipals::IsSystem) {
    if (!out.writePair(SCTAG_SAVED_FRAME_OBJECT,
                       SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_SYSTEM)) {
      return false;
    };
  } else if (savedFrame->getPrincipals() ==
             &ReconstructedSavedFramePrincipals::IsNotSystem) {
    if (!out.writePair(
            SCTAG_SAVED_FRAME_OBJECT,
            SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_NOT_SYSTEM)) {
      return false;
    }
  } else {
    if (auto principals = savedFrame->getPrincipals()) {
      if (!out.writePair(SCTAG_SAVED_FRAME_OBJECT, SCTAG_JSPRINCIPALS) ||
          !principals->write(context(), this)) {
        return false;
      }
    } else {
      if (!out.writePair(SCTAG_SAVED_FRAME_OBJECT, SCTAG_NULL_JSPRINCIPALS)) {
        return false;
      }
    }
  }

  // Write the SavedFrame's reserved slots, except for the parent, which is
  // queued on objs for further traversal.

  RootedValue val(context());

  val = BooleanValue(savedFrame->getMutedErrors());
  if (!startWrite(val)) {
    return false;
  }

  context()->markAtom(savedFrame->getSource());
  val = StringValue(savedFrame->getSource());
  if (!startWrite(val)) {
    return false;
  }

  val = NumberValue(savedFrame->getLine());
  if (!startWrite(val)) {
    return false;
  }

  val = NumberValue(savedFrame->getColumn());
  if (!startWrite(val)) {
    return false;
  }

  auto name = savedFrame->getFunctionDisplayName();
  if (name) {
    context()->markAtom(name);
  }
  val = name ? StringValue(name) : NullValue();
  if (!startWrite(val)) {
    return false;
  }

  auto cause = savedFrame->getAsyncCause();
  if (cause) {
    context()->markAtom(cause);
  }
  val = cause ? StringValue(cause) : NullValue();
  if (!startWrite(val)) {
    return false;
  }

  return true;
}

bool JSStructuredCloneWriter::startWrite(HandleValue v) {
  context()->check(v);

  if (v.isString()) {
    return writeString(SCTAG_STRING, v.toString());
  } else if (v.isInt32()) {
    return out.writePair(SCTAG_INT32, v.toInt32());
  } else if (v.isDouble()) {
    return out.writeDouble(v.toDouble());
  } else if (v.isBoolean()) {
    return out.writePair(SCTAG_BOOLEAN, v.toBoolean());
  } else if (v.isNull()) {
    return out.writePair(SCTAG_NULL, 0);
  } else if (v.isUndefined()) {
    return out.writePair(SCTAG_UNDEFINED, 0);
  } else if (v.isBigInt()) {
    return writeBigInt(SCTAG_BIGINT, v.toBigInt());
  } else if (v.isObject()) {
    RootedObject obj(context(), &v.toObject());

    bool backref;
    if (!startObject(obj, &backref)) {
      return false;
    }
    if (backref) {
      return true;
    }

    ESClass cls;
    if (!GetBuiltinClass(context(), obj, &cls)) {
      return false;
    }

    switch (cls) {
      case ESClass::Object:
      case ESClass::Array:
        return traverseObject(obj, cls);
      case ESClass::Number: {
        RootedValue unboxed(context());
        if (!Unbox(context(), obj, &unboxed)) {
          return false;
        }
        return out.writePair(SCTAG_NUMBER_OBJECT, 0) &&
               out.writeDouble(unboxed.toNumber());
      }
      case ESClass::String: {
        RootedValue unboxed(context());
        if (!Unbox(context(), obj, &unboxed)) {
          return false;
        }
        return writeString(SCTAG_STRING_OBJECT, unboxed.toString());
      }
      case ESClass::Boolean: {
        RootedValue unboxed(context());
        if (!Unbox(context(), obj, &unboxed)) {
          return false;
        }
        return out.writePair(SCTAG_BOOLEAN_OBJECT, unboxed.toBoolean());
      }
      case ESClass::RegExp: {
        RegExpShared* re = RegExpToShared(context(), obj);
        if (!re) {
          return false;
        }
        return out.writePair(SCTAG_REGEXP_OBJECT, re->getFlags().value()) &&
               writeString(SCTAG_STRING, re->getSource());
      }
      case ESClass::ArrayBuffer: {
        if (JS::IsArrayBufferObject(obj) && JS::ArrayBufferHasData(obj)) {
          return writeArrayBuffer(obj);
        }
        break;
      }
      case ESClass::SharedArrayBuffer:
        if (JS::IsSharedArrayBufferObject(obj)) {
          return writeSharedArrayBuffer(obj);
        }
        break;
      case ESClass::Date: {
        RootedValue unboxed(context());
        if (!Unbox(context(), obj, &unboxed)) {
          return false;
        }
        return out.writePair(SCTAG_DATE_OBJECT, 0) &&
               out.writeDouble(unboxed.toNumber());
      }
      case ESClass::Set:
        return traverseSet(obj);
      case ESClass::Map:
        return traverseMap(obj);
      case ESClass::BigInt: {
        RootedValue unboxed(context());
        if (!Unbox(context(), obj, &unboxed)) {
          return false;
        }
        return writeBigInt(SCTAG_BIGINT_OBJECT, unboxed.toBigInt());
      }
      case ESClass::Promise:
      case ESClass::MapIterator:
      case ESClass::SetIterator:
      case ESClass::Arguments:
      case ESClass::Error:
      case ESClass::Function:
        break;

      case ESClass::Other: {
        if (obj->canUnwrapAs<TypedArrayObject>()) {
          return writeTypedArray(obj);
        }
        if (obj->canUnwrapAs<DataViewObject>()) {
          return writeDataView(obj);
        }
        if (wasm::IsSharedWasmMemoryObject(obj)) {
          return writeSharedWasmMemory(obj);
        }
        if (obj->canUnwrapAs<SavedFrame>()) {
          return traverseSavedFrame(obj);
        }
        break;
      }
    }

    if (out.buf.callbacks_ && out.buf.callbacks_->write) {
      bool sameProcessScopeRequired = false;
      if (!out.buf.callbacks_->write(context(), this, obj,
                                     &sameProcessScopeRequired,
                                     out.buf.closure_)) {
        return false;
      }

      if (sameProcessScopeRequired) {
        output().sameProcessScopeRequired();
      }

      return true;
    }
    // else fall through
  }

  return reportDataCloneError(JS_SCERR_UNSUPPORTED_TYPE);
}

bool JSStructuredCloneWriter::writeHeader() {
  return out.writePair(SCTAG_HEADER, (uint32_t)output().scope());
}

bool JSStructuredCloneWriter::writeTransferMap() {
  if (transferableObjects.empty()) {
    return true;
  }

  if (!out.writePair(SCTAG_TRANSFER_MAP_HEADER, (uint32_t)SCTAG_TM_UNREAD)) {
    return false;
  }

  if (!out.write(transferableObjects.count())) {
    return false;
  }

  RootedObject obj(context());
  for (auto tr = transferableObjects.all(); !tr.empty(); tr.popFront()) {
    obj = tr.front();
    if (!memory.put(obj, memory.count())) {
      ReportOutOfMemory(context());
      return false;
    }

    // Emit a placeholder pointer.  We defer stealing the data until later
    // (and, if necessary, detaching this object if it's an ArrayBuffer).
    if (!out.writePair(SCTAG_TRANSFER_MAP_PENDING_ENTRY,
                       JS::SCTAG_TMO_UNFILLED)) {
      return false;
    }
    if (!out.write(0)) {  // Pointer to ArrayBuffer contents.
      return false;
    }
    if (!out.write(0)) {  // extraData
      return false;
    }
  }

  return true;
}

bool JSStructuredCloneWriter::transferOwnership() {
  if (transferableObjects.empty()) {
    return true;
  }

  // Walk along the transferables and the transfer map at the same time,
  // grabbing out pointers from the transferables and stuffing them into the
  // transfer map.
  auto point = out.iter();
  MOZ_RELEASE_ASSERT(point.canPeek());
  MOZ_ASSERT(uint32_t(NativeEndian::swapFromLittleEndian(point.peek()) >> 32) ==
             SCTAG_HEADER);
  point++;
  MOZ_RELEASE_ASSERT(point.canPeek());
  MOZ_ASSERT(uint32_t(NativeEndian::swapFromLittleEndian(point.peek()) >> 32) ==
             SCTAG_TRANSFER_MAP_HEADER);
  point++;
  MOZ_RELEASE_ASSERT(point.canPeek());
  MOZ_ASSERT(NativeEndian::swapFromLittleEndian(point.peek()) ==
             transferableObjects.count());
  point++;

  JSContext* cx = context();
  RootedObject obj(cx);
  JS::StructuredCloneScope scope = output().scope();
  for (auto tr = transferableObjects.all(); !tr.empty(); tr.popFront()) {
    obj = tr.front();

    uint32_t tag;
    JS::TransferableOwnership ownership;
    void* content;
    uint64_t extraData;

#if DEBUG
    SCInput::getPair(point.peek(), &tag, (uint32_t*)&ownership);
    MOZ_ASSERT(tag == SCTAG_TRANSFER_MAP_PENDING_ENTRY);
    MOZ_ASSERT(ownership == JS::SCTAG_TMO_UNFILLED);
#endif

    ESClass cls;
    if (!GetBuiltinClass(cx, obj, &cls)) {
      return false;
    }

    if (cls == ESClass::ArrayBuffer) {
      tag = SCTAG_TRANSFER_MAP_ARRAY_BUFFER;

      // The current setup of the array buffer inheritance hierarchy doesn't
      // lend itself well to generic manipulation via proxies.
      Rooted<ArrayBufferObject*> arrayBuffer(
          cx, obj->maybeUnwrapAs<ArrayBufferObject>());
      JSAutoRealm ar(cx, arrayBuffer);

      if (arrayBuffer->isDetached()) {
        reportDataCloneError(JS_SCERR_TYPED_ARRAY_DETACHED);
        return false;
      }

      if (arrayBuffer->isPreparedForAsmJS()) {
        reportDataCloneError(JS_SCERR_WASM_NO_TRANSFER);
        return false;
      }

      if (scope == JS::StructuredCloneScope::DifferentProcess ||
          scope == JS::StructuredCloneScope::DifferentProcessForIndexedDB) {
        // Write Transferred ArrayBuffers in DifferentProcess scope at
        // the end of the clone buffer, and store the offset within the
        // buffer to where the ArrayBuffer was written. Note that this
        // will invalidate the current position iterator.

        size_t pointOffset = out.offset(point);
        tag = SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER;
        ownership = JS::SCTAG_TMO_UNOWNED;
        content = nullptr;
        extraData = out.tell() -
                    pointOffset;  // Offset from tag to current end of buffer
        if (!writeArrayBuffer(arrayBuffer)) {
          ReportOutOfMemory(cx);
          return false;
        }

        // Must refresh the point iterator after its collection has
        // been modified.
        point = out.iter();
        point += pointOffset;

        if (!JS::DetachArrayBuffer(cx, arrayBuffer)) {
          return false;
        }
      } else {
        size_t nbytes = arrayBuffer->byteLength();

        using BufferContents = ArrayBufferObject::BufferContents;

        BufferContents bufContents =
            ArrayBufferObject::extractStructuredCloneContents(cx, arrayBuffer);
        if (!bufContents) {
          return false;  // out of memory
        }

        content = bufContents.data();
        if (bufContents.kind() == ArrayBufferObject::MAPPED) {
          ownership = JS::SCTAG_TMO_MAPPED_DATA;
        } else {
          MOZ_ASSERT(bufContents.kind() == ArrayBufferObject::MALLOCED,
                     "failing to handle new ArrayBuffer kind?");
          ownership = JS::SCTAG_TMO_ALLOC_DATA;
        }
        extraData = nbytes;
      }
    } else {
      if (!out.buf.callbacks_ || !out.buf.callbacks_->writeTransfer) {
        return reportDataCloneError(JS_SCERR_TRANSFERABLE);
      }
      if (!out.buf.callbacks_->writeTransfer(cx, obj, out.buf.closure_, &tag,
                                             &ownership, &content,
                                             &extraData)) {
        return false;
      }
      MOZ_ASSERT(tag > SCTAG_TRANSFER_MAP_PENDING_ENTRY);
    }

    point.write(NativeEndian::swapToLittleEndian(PairToUInt64(tag, ownership)));
    MOZ_ALWAYS_TRUE(point.advance());
    point.write(
        NativeEndian::swapToLittleEndian(reinterpret_cast<uint64_t>(content)));
    MOZ_ALWAYS_TRUE(point.advance());
    point.write(NativeEndian::swapToLittleEndian(extraData));
    MOZ_ALWAYS_TRUE(point.advance());
  }

#if DEBUG
  // Make sure there aren't any more transfer map entries after the expected
  // number we read out.
  if (!point.done()) {
    uint32_t tag, data;
    SCInput::getPair(point.peek(), &tag, &data);
    MOZ_ASSERT(tag < SCTAG_TRANSFER_MAP_HEADER ||
               tag >= SCTAG_TRANSFER_MAP_END_OF_BUILTIN_TYPES);
  }
#endif
  return true;
}

bool JSStructuredCloneWriter::write(HandleValue v) {
  if (!startWrite(v)) {
    return false;
  }

  RootedObject obj(context());
  RootedValue key(context());
  RootedValue val(context());
  RootedId id(context());

  while (!counts.empty()) {
    obj = &objs.back().toObject();
    context()->check(obj);
    if (counts.back()) {
      counts.back()--;

      ESClass cls;
      if (!GetBuiltinClass(context(), obj, &cls)) {
        return false;
      }

      if (cls == ESClass::Map) {
        key = otherEntries.popCopy();
        checkStack();

        counts.back()--;
        val = otherEntries.popCopy();
        checkStack();

        if (!startWrite(key) || !startWrite(val)) {
          return false;
        }
      } else if (cls == ESClass::Set || obj->canUnwrapAs<SavedFrame>()) {
        key = otherEntries.popCopy();
        checkStack();

        if (!startWrite(key)) {
          return false;
        }
      } else {
        id = objectEntries.popCopy();
        key = IdToValue(id);
        checkStack();

        // If obj still has an own property named id, write it out.
        bool found;
        if (GetOwnPropertyPure(context(), obj, id, val.address(), &found)) {
          if (found) {
            if (!startWrite(key) || !startWrite(val)) {
              return false;
            }
          }
          continue;
        }

        if (!HasOwnProperty(context(), obj, id, &found)) {
          return false;
        }

        if (found) {
          if (!startWrite(key) || !GetProperty(context(), obj, obj, id, &val) ||
              !startWrite(val)) {
            return false;
          }
        }
      }
    } else {
      if (!out.writePair(SCTAG_END_OF_KEYS, 0)) {
        return false;
      }
      objs.popBack();
      counts.popBack();
    }
  }

  memory.clear();
  return transferOwnership();
}

template <typename CharT>
JSString* JSStructuredCloneReader::readStringImpl(uint32_t nchars,
                                                  gc::InitialHeap heap) {
  if (nchars > JSString::MAX_LENGTH) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA, "string length");
    return nullptr;
  }

  InlineCharBuffer<CharT> chars;
  if (!chars.maybeAlloc(context(), nchars) ||
      !in.readChars(chars.get(), nchars)) {
    return nullptr;
  }
  return chars.toStringDontDeflate(context(), nchars, heap);
}

JSString* JSStructuredCloneReader::readString(uint32_t data,
                                              gc::InitialHeap heap) {
  uint32_t nchars = data & BitMask(31);
  bool latin1 = data & (1 << 31);
  return latin1 ? readStringImpl<Latin1Char>(nchars, heap)
                : readStringImpl<char16_t>(nchars, heap);
}

BigInt* JSStructuredCloneReader::readBigInt(uint32_t data) {
  size_t length = data & BitMask(31);
  bool isNegative = data & (1 << 31);
  if (length == 0) {
    return BigInt::zero(context());
  }
  RootedBigInt result(
      context(), BigInt::createUninitialized(context(), length, isNegative));
  if (!result) {
    return nullptr;
  }
  if (!in.readArray(result->digits().data(), length)) {
    return nullptr;
  }
  return result;
}

static uint32_t TagToV1ArrayType(uint32_t tag) {
  MOZ_ASSERT(tag >= SCTAG_TYPED_ARRAY_V1_MIN &&
             tag <= SCTAG_TYPED_ARRAY_V1_MAX);
  return tag - SCTAG_TYPED_ARRAY_V1_MIN;
}

bool JSStructuredCloneReader::readTypedArray(uint32_t arrayType,
                                             uint64_t nelems,
                                             MutableHandleValue vp,
                                             bool v1Read) {
  if (arrayType > (v1Read ? Scalar::Uint8Clamped : Scalar::BigUint64)) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "unhandled typed array element type");
    return false;
  }

  // Push a placeholder onto the allObjs list to stand in for the typed array.
  uint32_t placeholderIndex = allObjs.length();
  Value dummy = UndefinedValue();
  if (!allObjs.append(dummy)) {
    return false;
  }

  // Read the ArrayBuffer object and its contents (but no properties)
  RootedValue v(context());
  uint64_t byteOffset;
  if (v1Read) {
    if (!readV1ArrayBuffer(arrayType, nelems, &v)) {
      return false;
    }
    byteOffset = 0;
  } else {
    if (!startRead(&v)) {
      return false;
    }
    if (!in.read(&byteOffset)) {
      return false;
    }
  }

  // Ensure invalid 64-bit values won't be truncated below.
  if (nelems > ArrayBufferObject::maxBufferByteLength() ||
      byteOffset > ArrayBufferObject::maxBufferByteLength()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid typed array length or offset");
    return false;
  }

  if (!v.isObject() || !v.toObject().is<ArrayBufferObjectMaybeShared>()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "typed array must be backed by an ArrayBuffer");
    return false;
  }

  RootedObject buffer(context(), &v.toObject());
  RootedObject obj(context(), nullptr);

  switch (arrayType) {
    case Scalar::Int8:
      obj = JS_NewInt8ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Uint8:
      obj = JS_NewUint8ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Int16:
      obj = JS_NewInt16ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Uint16:
      obj = JS_NewUint16ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Int32:
      obj = JS_NewInt32ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Uint32:
      obj = JS_NewUint32ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Float32:
      obj = JS_NewFloat32ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Float64:
      obj = JS_NewFloat64ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::Uint8Clamped:
      obj = JS_NewUint8ClampedArrayWithBuffer(context(), buffer, byteOffset,
                                              nelems);
      break;
    case Scalar::BigInt64:
      obj =
          JS_NewBigInt64ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    case Scalar::BigUint64:
      obj =
          JS_NewBigUint64ArrayWithBuffer(context(), buffer, byteOffset, nelems);
      break;
    default:
      MOZ_CRASH("Can't happen: arrayType range checked above");
  }

  if (!obj) {
    return false;
  }
  vp.setObject(*obj);

  allObjs[placeholderIndex].set(vp);

  return true;
}

bool JSStructuredCloneReader::readDataView(uint64_t byteLength,
                                           MutableHandleValue vp) {
  // Push a placeholder onto the allObjs list to stand in for the DataView.
  uint32_t placeholderIndex = allObjs.length();
  Value dummy = UndefinedValue();
  if (!allObjs.append(dummy)) {
    return false;
  }

  // Read the ArrayBuffer object and its contents (but no properties).
  RootedValue v(context());
  if (!startRead(&v)) {
    return false;
  }
  if (!v.isObject() || !v.toObject().is<ArrayBufferObjectMaybeShared>()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "DataView must be backed by an ArrayBuffer");
    return false;
  }

  // Read byteOffset.
  uint64_t byteOffset;
  if (!in.read(&byteOffset)) {
    return false;
  }

  // Ensure invalid 64-bit values won't be truncated below.
  if (byteLength > ArrayBufferObject::maxBufferByteLength() ||
      byteOffset > ArrayBufferObject::maxBufferByteLength()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid DataView length or offset");
    return false;
  }

  RootedObject buffer(context(), &v.toObject());
  RootedObject obj(context(),
                   JS_NewDataView(context(), buffer, byteOffset, byteLength));
  if (!obj) {
    return false;
  }
  vp.setObject(*obj);

  allObjs[placeholderIndex].set(vp);

  return true;
}

bool JSStructuredCloneReader::readArrayBuffer(StructuredDataType type,
                                              uint32_t data,
                                              MutableHandleValue vp) {
  // V2 stores the length in |data|. The current version stores the
  // length separately to allow larger length values.
  uint64_t nbytes = 0;
  if (type == SCTAG_ARRAY_BUFFER_OBJECT) {
    if (!in.read(&nbytes)) {
      return false;
    }
  } else {
    MOZ_ASSERT(type == SCTAG_ARRAY_BUFFER_OBJECT_V2);
    nbytes = data;
  }

  // The maximum ArrayBuffer size depends on the platform and prefs, and we cast
  // to size_t below, so we have to check this here.
  if (nbytes > ArrayBufferObject::maxBufferByteLength()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }

  JSObject* obj = ArrayBufferObject::createZeroed(context(), size_t(nbytes));
  if (!obj) {
    return false;
  }
  vp.setObject(*obj);
  ArrayBufferObject& buffer = obj->as<ArrayBufferObject>();
  MOZ_ASSERT(buffer.byteLength() == nbytes);
  return in.readArray(buffer.dataPointer(), nbytes);
}

bool JSStructuredCloneReader::readSharedArrayBuffer(MutableHandleValue vp) {
  if (!cloneDataPolicy.areIntraClusterClonableSharedObjectsAllowed() ||
      !cloneDataPolicy.areSharedMemoryObjectsAllowed()) {
    auto error = context()->realm()->creationOptions().getCoopAndCoepEnabled()
                     ? JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP
                     : JS_SCERR_NOT_CLONABLE;
    ReportDataCloneError(context(), callbacks, error, closure,
                         "SharedArrayBuffer");
    return false;
  }

  uint64_t byteLength;
  if (!in.readBytes(&byteLength, sizeof(byteLength))) {
    return in.reportTruncated();
  }

  // The maximum ArrayBuffer size depends on the platform and prefs, and we cast
  // to size_t below, so we have to check this here.
  if (byteLength > ArrayBufferObject::maxBufferByteLength()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }

  intptr_t p;
  if (!in.readBytes(&p, sizeof(p))) {
    return in.reportTruncated();
  }

  SharedArrayRawBuffer* rawbuf = reinterpret_cast<SharedArrayRawBuffer*>(p);

  // There's no guarantee that the receiving agent has enabled shared memory
  // even if the transmitting agent has done so.  Ideally we'd check at the
  // transmission point, but that's tricky, and it will be a very rare problem
  // in any case.  Just fail at the receiving end if we can't handle it.

  if (!context()
           ->realm()
           ->creationOptions()
           .getSharedMemoryAndAtomicsEnabled()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_SAB_DISABLED);
    return false;
  }

  // The new object will have a new reference to the rawbuf.

  if (!rawbuf->addReference()) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_SAB_REFCNT_OFLO);
    return false;
  }

  RootedObject obj(context(),
                   SharedArrayBufferObject::New(context(), rawbuf, byteLength));
  if (!obj) {
    rawbuf->dropReference();
    return false;
  }

  // `rawbuf` is now owned by `obj`.

  if (callbacks && callbacks->sabCloned &&
      !callbacks->sabCloned(context(), /*receiving=*/true, closure)) {
    return false;
  }

  vp.setObject(*obj);
  return true;
}

bool JSStructuredCloneReader::readSharedWasmMemory(uint32_t nbytes,
                                                   MutableHandleValue vp) {
  JSContext* cx = context();
  if (nbytes != 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid shared wasm memory tag");
    return false;
  }

  if (!cloneDataPolicy.areIntraClusterClonableSharedObjectsAllowed() ||
      !cloneDataPolicy.areSharedMemoryObjectsAllowed()) {
    auto error = context()->realm()->creationOptions().getCoopAndCoepEnabled()
                     ? JS_SCERR_NOT_CLONABLE_WITH_COOP_COEP
                     : JS_SCERR_NOT_CLONABLE;
    ReportDataCloneError(cx, callbacks, error, closure, "WebAssembly.Memory");
    return false;
  }

  // Read the SharedArrayBuffer object.
  RootedValue payload(cx);
  if (!startRead(&payload)) {
    return false;
  }
  if (!payload.isObject() ||
      !payload.toObject().is<SharedArrayBufferObject>()) {
    JS_ReportErrorNumberASCII(
        context(), GetErrorMessage, nullptr, JSMSG_SC_BAD_SERIALIZED_DATA,
        "shared wasm memory must be backed by a SharedArrayBuffer");
    return false;
  }

  Rooted<ArrayBufferObjectMaybeShared*> sab(
      cx, &payload.toObject().as<SharedArrayBufferObject>());

  // Construct the memory.
  RootedObject proto(
      cx, &cx->global()->getPrototype(JSProto_WasmMemory).toObject());
  RootedObject memory(cx, WasmMemoryObject::create(cx, sab, proto));
  if (!memory) {
    return false;
  }

  vp.setObject(*memory);
  return true;
}

/*
 * Read in the data for a structured clone version 1 ArrayBuffer, performing
 * endianness-conversion while reading.
 */
bool JSStructuredCloneReader::readV1ArrayBuffer(uint32_t arrayType,
                                                uint32_t nelems,
                                                MutableHandleValue vp) {
  if (arrayType > Scalar::Uint8Clamped) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid TypedArray type");
    return false;
  }

  mozilla::CheckedInt<size_t> nbytes =
      mozilla::CheckedInt<size_t>(nelems) *
      TypedArrayElemSize(static_cast<Scalar::Type>(arrayType));
  if (!nbytes.isValid() || nbytes.value() > UINT32_MAX) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid typed array size");
    return false;
  }

  JSObject* obj = ArrayBufferObject::createZeroed(context(), nbytes.value());
  if (!obj) {
    return false;
  }
  vp.setObject(*obj);
  ArrayBufferObject& buffer = obj->as<ArrayBufferObject>();
  MOZ_ASSERT(buffer.byteLength() == nbytes);

  switch (arrayType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
      return in.readArray((uint8_t*)buffer.dataPointer(), nelems);
    case Scalar::Int16:
    case Scalar::Uint16:
      return in.readArray((uint16_t*)buffer.dataPointer(), nelems);
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      return in.readArray((uint32_t*)buffer.dataPointer(), nelems);
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      return in.readArray((uint64_t*)buffer.dataPointer(), nelems);
    default:
      MOZ_CRASH("Can't happen: arrayType range checked by caller");
  }
}

static bool PrimitiveToObject(JSContext* cx, MutableHandleValue vp) {
  JSObject* obj = js::PrimitiveToObject(cx, vp);
  if (!obj) {
    return false;
  }

  vp.setObject(*obj);
  return true;
}

bool JSStructuredCloneReader::startRead(MutableHandleValue vp,
                                        gc::InitialHeap strHeap) {
  uint32_t tag, data;
  bool alreadAppended = false;

  if (!in.readPair(&tag, &data)) {
    return false;
  }

  numItemsRead++;

  switch (tag) {
    case SCTAG_NULL:
      vp.setNull();
      break;

    case SCTAG_UNDEFINED:
      vp.setUndefined();
      break;

    case SCTAG_INT32:
      vp.setInt32(data);
      break;

    case SCTAG_BOOLEAN:
    case SCTAG_BOOLEAN_OBJECT:
      vp.setBoolean(!!data);
      if (tag == SCTAG_BOOLEAN_OBJECT && !PrimitiveToObject(context(), vp)) {
        return false;
      }
      break;

    case SCTAG_STRING:
    case SCTAG_STRING_OBJECT: {
      JSString* str = readString(data, strHeap);
      if (!str) {
        return false;
      }
      vp.setString(str);
      if (tag == SCTAG_STRING_OBJECT && !PrimitiveToObject(context(), vp)) {
        return false;
      }
      break;
    }

    case SCTAG_NUMBER_OBJECT: {
      double d;
      if (!in.readDouble(&d)) {
        return false;
      }
      vp.setDouble(CanonicalizeNaN(d));
      if (!PrimitiveToObject(context(), vp)) {
        return false;
      }
      break;
    }

    case SCTAG_BIGINT:
    case SCTAG_BIGINT_OBJECT: {
      RootedBigInt bi(context(), readBigInt(data));
      if (!bi) {
        return false;
      }
      vp.setBigInt(bi);
      if (tag == SCTAG_BIGINT_OBJECT && !PrimitiveToObject(context(), vp)) {
        return false;
      }
      break;
    }

    case SCTAG_DATE_OBJECT: {
      double d;
      if (!in.readDouble(&d)) {
        return false;
      }
      JS::ClippedTime t = JS::TimeClip(d);
      if (!NumbersAreIdentical(d, t.toDouble())) {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA, "date");
        return false;
      }
      JSObject* obj = NewDateObjectMsec(context(), t);
      if (!obj) {
        return false;
      }
      vp.setObject(*obj);
      break;
    }

    case SCTAG_REGEXP_OBJECT: {
      if ((data & RegExpFlag::AllFlags) != data) {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA, "regexp");
        return false;
      }

      RegExpFlags flags(AssertedCast<uint8_t>(data));

      uint32_t tag2, stringData;
      if (!in.readPair(&tag2, &stringData)) {
        return false;
      }
      if (tag2 != SCTAG_STRING) {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA, "regexp");
        return false;
      }

      JSString* str = readString(stringData, gc::TenuredHeap);
      if (!str) {
        return false;
      }

      RootedAtom atom(context(), AtomizeString(context(), str));
      if (!atom) {
        return false;
      }

      RegExpObject* reobj =
          RegExpObject::create(context(), atom, flags, GenericObject);
      if (!reobj) {
        return false;
      }
      vp.setObject(*reobj);
      break;
    }

    case SCTAG_ARRAY_OBJECT:
    case SCTAG_OBJECT_OBJECT: {
      JSObject* obj =
          (tag == SCTAG_ARRAY_OBJECT)
              ? (JSObject*)NewDenseUnallocatedArray(
                    context(), NativeEndian::swapFromLittleEndian(data))
              : (JSObject*)NewBuiltinClassInstance<PlainObject>(context());
      if (!obj || !objs.append(ObjectValue(*obj))) {
        return false;
      }
      vp.setObject(*obj);
      break;
    }

    case SCTAG_BACK_REFERENCE_OBJECT: {
      if (data >= allObjs.length() || !allObjs[data].isObject()) {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA,
                                  "invalid back reference in input");
        return false;
      }
      vp.set(allObjs[data]);
      return true;
    }

    case SCTAG_TRANSFER_MAP_HEADER:
    case SCTAG_TRANSFER_MAP_PENDING_ENTRY:
      // We should be past all the transfer map tags.
      JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                JSMSG_SC_BAD_SERIALIZED_DATA, "invalid input");
      return false;

    case SCTAG_ARRAY_BUFFER_OBJECT_V2:
    case SCTAG_ARRAY_BUFFER_OBJECT:
      if (!readArrayBuffer(StructuredDataType(tag), data, vp)) {
        return false;
      }
      break;

    case SCTAG_SHARED_ARRAY_BUFFER_OBJECT:
      if (!readSharedArrayBuffer(vp)) {
        return false;
      }
      break;

    case SCTAG_SHARED_WASM_MEMORY_OBJECT:
      if (!readSharedWasmMemory(data, vp)) {
        return false;
      }
      break;

    case SCTAG_TYPED_ARRAY_OBJECT_V2: {
      // readTypedArray adds the array to allObjs.
      // V2 stores the length (nelems) in |data| and the arrayType separately.
      uint64_t arrayType;
      if (!in.read(&arrayType)) {
        return false;
      }
      uint64_t nelems = data;
      return readTypedArray(arrayType, nelems, vp);
    }

    case SCTAG_TYPED_ARRAY_OBJECT: {
      // readTypedArray adds the array to allObjs.
      // The current version stores the array type in |data| and the length
      // (nelems) separately to support large TypedArrays.
      uint32_t arrayType = data;
      uint64_t nelems;
      if (!in.read(&nelems)) {
        return false;
      }
      return readTypedArray(arrayType, nelems, vp);
    }

    case SCTAG_DATA_VIEW_OBJECT_V2: {
      // readDataView adds the array to allObjs.
      uint64_t byteLength = data;
      return readDataView(byteLength, vp);
    }

    case SCTAG_DATA_VIEW_OBJECT: {
      // readDataView adds the array to allObjs.
      uint64_t byteLength;
      if (!in.read(&byteLength)) {
        return false;
      }
      return readDataView(byteLength, vp);
    }

    case SCTAG_MAP_OBJECT: {
      JSObject* obj = MapObject::create(context());
      if (!obj || !objs.append(ObjectValue(*obj))) {
        return false;
      }
      vp.setObject(*obj);
      break;
    }

    case SCTAG_SET_OBJECT: {
      JSObject* obj = SetObject::create(context());
      if (!obj || !objs.append(ObjectValue(*obj))) {
        return false;
      }
      vp.setObject(*obj);
      break;
    }

    case SCTAG_SAVED_FRAME_OBJECT: {
      auto obj = readSavedFrame(data);
      if (!obj || !objs.append(ObjectValue(*obj))) {
        return false;
      }
      vp.setObject(*obj);
      break;
    }

    default: {
      if (tag <= SCTAG_FLOAT_MAX) {
        double d = ReinterpretPairAsDouble(tag, data);
        vp.setNumber(CanonicalizeNaN(d));
        break;
      }

      if (SCTAG_TYPED_ARRAY_V1_MIN <= tag && tag <= SCTAG_TYPED_ARRAY_V1_MAX) {
        // A v1-format typed array
        // readTypedArray adds the array to allObjs
        return readTypedArray(TagToV1ArrayType(tag), data, vp, true);
      }

      if (!callbacks || !callbacks->read) {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA,
                                  "unsupported type");
        return false;
      }

      // callbacks->read() might read other objects from the buffer.
      // In startWrite we always write the object itself before calling
      // the custom function. We should do the same here to keep
      // indexing consistent.
      uint32_t placeholderIndex = allObjs.length();
      Value dummy = UndefinedValue();
      if (!allObjs.append(dummy)) {
        return false;
      }
      JSObject* obj =
          callbacks->read(context(), this, cloneDataPolicy, tag, data, closure);
      if (!obj) {
        return false;
      }
      vp.setObject(*obj);
      allObjs[placeholderIndex].set(vp);
      alreadAppended = true;
    }
  }

  if (!alreadAppended && vp.isObject() && !allObjs.append(vp)) {
    return false;
  }

  return true;
}

bool JSStructuredCloneReader::readHeader() {
  uint32_t tag, data;
  if (!in.getPair(&tag, &data)) {
    return in.reportTruncated();
  }

  JS::StructuredCloneScope storedScope;
  if (tag == SCTAG_HEADER) {
    MOZ_ALWAYS_TRUE(in.readPair(&tag, &data));
    storedScope = JS::StructuredCloneScope(data);
  } else {
    // Old structured clone buffer. We must have read it from disk.
    storedScope = JS::StructuredCloneScope::DifferentProcessForIndexedDB;
  }

  // Backward compatibility with old structured clone buffers. Value '0' was
  // used for SameProcessSameThread scope.
  if ((int)storedScope == 0) {
    storedScope = JS::StructuredCloneScope::SameProcess;
  }

  if (storedScope < JS::StructuredCloneScope::SameProcess ||
      storedScope > JS::StructuredCloneScope::DifferentProcessForIndexedDB) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid structured clone scope");
    return false;
  }

  if (allowedScope == JS::StructuredCloneScope::DifferentProcessForIndexedDB) {
    // Bug 1434308 and bug 1458320 - the scopes stored in old IndexedDB
    // clones are incorrect. Treat them as if they were DifferentProcess.
    allowedScope = JS::StructuredCloneScope::DifferentProcess;
    return true;
  }

  if (storedScope < allowedScope) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "incompatible structured clone scope");
    return false;
  }

  return true;
}

bool JSStructuredCloneReader::readTransferMap() {
  JSContext* cx = context();
  auto headerPos = in.tell();

  uint32_t tag, data;
  if (!in.getPair(&tag, &data)) {
    return in.reportTruncated();
  }

  if (tag != SCTAG_TRANSFER_MAP_HEADER ||
      TransferableMapHeader(data) == SCTAG_TM_TRANSFERRED) {
    return true;
  }

  uint64_t numTransferables;
  MOZ_ALWAYS_TRUE(in.readPair(&tag, &data));
  if (!in.read(&numTransferables)) {
    return false;
  }

  for (uint64_t i = 0; i < numTransferables; i++) {
    auto pos = in.tell();

    if (!in.readPair(&tag, &data)) {
      return false;
    }

    if (tag == SCTAG_TRANSFER_MAP_PENDING_ENTRY) {
      ReportDataCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure);
      return false;
    }

    RootedObject obj(cx);

    void* content;
    if (!in.readPtr(&content)) {
      return false;
    }

    uint64_t extraData;
    if (!in.read(&extraData)) {
      return false;
    }

    if (tag == SCTAG_TRANSFER_MAP_ARRAY_BUFFER) {
      if (allowedScope == JS::StructuredCloneScope::DifferentProcess ||
          allowedScope ==
              JS::StructuredCloneScope::DifferentProcessForIndexedDB) {
        // Transferred ArrayBuffers in a DifferentProcess clone buffer
        // are treated as if they weren't Transferred at all. We should
        // only see SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER.
        ReportDataCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure);
        return false;
      }

      MOZ_RELEASE_ASSERT(extraData <= ArrayBufferObject::maxBufferByteLength());
      size_t nbytes = extraData;

      MOZ_ASSERT(data == JS::SCTAG_TMO_ALLOC_DATA ||
                 data == JS::SCTAG_TMO_MAPPED_DATA);
      if (data == JS::SCTAG_TMO_ALLOC_DATA) {
        obj = JS::NewArrayBufferWithContents(cx, nbytes, content);
      } else if (data == JS::SCTAG_TMO_MAPPED_DATA) {
        obj = JS::NewMappedArrayBufferWithContents(cx, nbytes, content);
      }
    } else if (tag == SCTAG_TRANSFER_MAP_STORED_ARRAY_BUFFER) {
      auto savedPos = in.tell();
      auto guard = mozilla::MakeScopeExit([&] { in.seekTo(savedPos); });
      in.seekTo(pos);
      if (!in.seekBy(static_cast<size_t>(extraData))) {
        return false;
      }

      uint32_t tag, data;
      if (!in.readPair(&tag, &data)) {
        return false;
      }
      if (tag != SCTAG_ARRAY_BUFFER_OBJECT_V2 &&
          tag != SCTAG_ARRAY_BUFFER_OBJECT) {
        ReportDataCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure);
        return false;
      }
      RootedValue val(cx);
      if (!readArrayBuffer(StructuredDataType(tag), data, &val)) {
        return false;
      }
      obj = &val.toObject();
    } else {
      if (!callbacks || !callbacks->readTransfer) {
        ReportDataCloneError(cx, callbacks, JS_SCERR_TRANSFERABLE, closure);
        return false;
      }
      if (!callbacks->readTransfer(cx, this, tag, content, extraData, closure,
                                   &obj)) {
        return false;
      }
      MOZ_ASSERT(obj);
      MOZ_ASSERT(!cx->isExceptionPending());
    }

    // On failure, the buffer will still own the data (since its ownership
    // will not get set to SCTAG_TMO_UNOWNED), so the data will be freed by
    // DiscardTransferables.
    if (!obj) {
      return false;
    }

    // Mark the SCTAG_TRANSFER_MAP_* entry as no longer owned by the input
    // buffer.
    pos.write(PairToUInt64(tag, JS::SCTAG_TMO_UNOWNED));
    MOZ_ASSERT(!pos.done());

    if (!allObjs.append(ObjectValue(*obj))) {
      return false;
    }
  }

  // Mark the whole transfer map as consumed.
#ifdef DEBUG
  SCInput::getPair(headerPos.peek(), &tag, &data);
  MOZ_ASSERT(tag == SCTAG_TRANSFER_MAP_HEADER);
  MOZ_ASSERT(TransferableMapHeader(data) != SCTAG_TM_TRANSFERRED);
#endif
  headerPos.write(
      PairToUInt64(SCTAG_TRANSFER_MAP_HEADER, SCTAG_TM_TRANSFERRED));

  return true;
}

JSObject* JSStructuredCloneReader::readSavedFrame(uint32_t principalsTag) {
  RootedSavedFrame savedFrame(context(), SavedFrame::create(context()));
  if (!savedFrame) {
    return nullptr;
  }

  JSPrincipals* principals;
  if (principalsTag == SCTAG_JSPRINCIPALS) {
    if (!context()->runtime()->readPrincipals) {
      JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                JSMSG_SC_UNSUPPORTED_TYPE);
      return nullptr;
    }

    if (!context()->runtime()->readPrincipals(context(), this, &principals)) {
      return nullptr;
    }
  } else if (principalsTag ==
             SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_SYSTEM) {
    principals = &ReconstructedSavedFramePrincipals::IsSystem;
    principals->refcount++;
  } else if (principalsTag ==
             SCTAG_RECONSTRUCTED_SAVED_FRAME_PRINCIPALS_IS_NOT_SYSTEM) {
    principals = &ReconstructedSavedFramePrincipals::IsNotSystem;
    principals->refcount++;
  } else if (principalsTag == SCTAG_NULL_JSPRINCIPALS) {
    principals = nullptr;
  } else {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "bad SavedFrame principals");
    return nullptr;
  }

  RootedValue mutedErrors(context());
  RootedValue source(context());
  {
    // Read a |mutedErrors| boolean followed by a |source| string.
    // The |mutedErrors| boolean is present in all new structured-clone data,
    // but in older data it will be absent and only the |source| string will be
    // found.
    if (!startRead(&mutedErrors)) {
      return nullptr;
    }

    if (mutedErrors.isBoolean()) {
      if (!startRead(&source, gc::TenuredHeap) || !source.isString()) {
        return nullptr;
      }
    } else if (mutedErrors.isString()) {
      // Backwards compatibility: Handle missing |mutedErrors| boolean,
      // this is actually just a |source| string.
      source = mutedErrors;
      mutedErrors.setBoolean(true);  // Safe default value.
    } else {
      // Invalid type.
      return nullptr;
    }
  }

  savedFrame->initPrincipalsAlreadyHeldAndMutedErrors(principals,
                                                      mutedErrors.toBoolean());

  auto atomSource = AtomizeString(context(), source.toString());
  if (!atomSource) {
    return nullptr;
  }
  savedFrame->initSource(atomSource);

  RootedValue lineVal(context());
  uint32_t line;
  if (!startRead(&lineVal) || !lineVal.isNumber() ||
      !ToUint32(context(), lineVal, &line)) {
    return nullptr;
  }
  savedFrame->initLine(line);

  RootedValue columnVal(context());
  uint32_t column;
  if (!startRead(&columnVal) || !columnVal.isNumber() ||
      !ToUint32(context(), columnVal, &column)) {
    return nullptr;
  }
  savedFrame->initColumn(column);

  // Don't specify a source ID when reading a cloned saved frame, as these IDs
  // are only valid within a specific process.
  savedFrame->initSourceId(0);

  RootedValue name(context());
  if (!startRead(&name, gc::TenuredHeap)) {
    return nullptr;
  }
  if (!(name.isString() || name.isNull())) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid saved frame cause");
    return nullptr;
  }
  JSAtom* atomName = nullptr;
  if (name.isString()) {
    atomName = AtomizeString(context(), name.toString());
    if (!atomName) {
      return nullptr;
    }
  }

  savedFrame->initFunctionDisplayName(atomName);

  RootedValue cause(context());
  if (!startRead(&cause, gc::TenuredHeap)) {
    return nullptr;
  }
  if (!(cause.isString() || cause.isNull())) {
    JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_SERIALIZED_DATA,
                              "invalid saved frame cause");
    return nullptr;
  }
  JSAtom* atomCause = nullptr;
  if (cause.isString()) {
    atomCause = AtomizeString(context(), cause.toString());
    if (!atomCause) {
      return nullptr;
    }
  }
  savedFrame->initAsyncCause(atomCause);

  return savedFrame;
}

// Class for counting "children" (actually parent frames) of the SavedFrames on
// the `objs` stack. When a SavedFrame is complete, it should have exactly 1
// parent frame.
//
// This class must be notified after every startRead() call.
//
// If we add other types with restrictions on the number of children, this
// should be expanded to handle those types as well.
//
class ChildCounter {
  JSContext* cx;
  Vector<size_t> counts;
  size_t objsLength;
  size_t objCountsIndex;

 public:
  explicit ChildCounter(JSContext* cx) : cx(cx), counts(cx), objsLength(0) {}

  void noteObjIsOnTopOfStack() { objCountsIndex = counts.length() - 1; }

  // startRead() will have pushed any newly seen object onto the `objs` stack.
  // If it did not read an object, or if the object it read was a backreference
  // to an earlier object, the stack will be unchanged.
  bool postStartRead(HandleValueVector objs) {
    if (objs.length() == objsLength) {
      // No new object pushed.
      return true;
    }

    // Push a new child counter (initialized to zero) for the new object.
    MOZ_ASSERT(objs.length() == objsLength + 1);
    objsLength = objs.length();
    if (objs.back().toObject().is<SavedFrame>()) {
      return counts.append(0);
    }

    // Not a SavedFrame; do nothing.
    return true;
  }

  // Reading has reached the end of the children for an object. Check whether
  // we saw the right number of children.
  bool handleEndOfChildren(HandleValueVector objs) {
    MOZ_ASSERT(objsLength > 0);
    objsLength--;

    if (objs.back().toObject().is<SavedFrame>()) {
      if (counts.back() != 1) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA,
                                  "must have single SavedFrame parent");
        return false;
      }

      counts.popBack();
    }
    return true;
  }

  // While we are reading children, we need to know whether this is the first
  // child seen or not, in order to avoid double-initializing in the error
  // case.
  bool checkSingleParentFrame() {
    // We are checking at a point where we have read 0 or more parent frames,
    // in which case `obj` may not be on top of the `objs` stack anymore and
    // the count on top of the `counts` stack will correspond to the most
    // recently read frame, not `obj`. Use the remembered `counts` index from
    // when `obj` *was* on top of the stack.
    return ++counts[objCountsIndex] == 1;
  }
};

// Perform the whole recursive reading procedure.
bool JSStructuredCloneReader::read(MutableHandleValue vp, size_t nbytes) {
  auto startTime = mozilla::TimeStamp::NowUnfuzzed();

  if (!readHeader()) {
    return false;
  }

  if (!readTransferMap()) {
    return false;
  }

  ChildCounter childCounter(context());

  // Start out by reading in the main object and pushing it onto the 'objs'
  // stack. The data related to this object and its descendants extends from
  // here to the SCTAG_END_OF_KEYS at the end of the stream.
  if (!startRead(vp) || !childCounter.postStartRead(objs)) {
    return false;
  }

  // Stop when the stack shows that all objects have been read.
  while (objs.length() != 0) {
    // What happens depends on the top obj on the objs stack.
    RootedObject obj(context(), &objs.back().toObject());
    childCounter.noteObjIsOnTopOfStack();

    uint32_t tag, data;
    if (!in.getPair(&tag, &data)) {
      return false;
    }

    if (tag == SCTAG_END_OF_KEYS) {
      if (!childCounter.handleEndOfChildren(objs)) {
        return false;
      }

      // Pop the current obj off the stack, since we are done with it and
      // its children.
      MOZ_ALWAYS_TRUE(in.readPair(&tag, &data));
      objs.popBack();
      continue;
    }

    // The input stream contains a sequence of "child" values, whose
    // interpretation depends on the type of obj. These values can be
    // anything, and startRead() will push onto 'objs' for any non-leaf
    // value (i.e., anything that may contain children).
    //
    // startRead() will allocate the (empty) object, but note that when
    // startRead() returns, 'key' is not yet initialized with any of its
    // properties. Those will be filled in by returning to the head of this
    // loop, processing the first child obj, and continuing until all
    // children have been fully created.
    //
    // Note that this means the ordering in the stream is a little funky for
    // things like Map. See the comment above traverseMap() for an example.
    RootedValue key(context());
    if (!startRead(&key) || !childCounter.postStartRead(objs)) {
      return false;
    }

    if (key.isNull() && !(obj->is<MapObject>() || obj->is<SetObject>() ||
                          obj->is<SavedFrame>())) {
      // Backwards compatibility: Null formerly indicated the end of
      // object properties.
      if (!childCounter.handleEndOfChildren(objs)) {
        return false;
      }
      objs.popBack();
      continue;
    }

    // Set object: the values between obj header (from startRead()) and
    // SCTAG_END_OF_KEYS are all interpreted as values to add to the set.
    if (obj->is<SetObject>()) {
      if (!SetObject::add(context(), obj, key)) {
        return false;
      }
      continue;
    }

    // SavedFrame object: there is one following value, the parent SavedFrame,
    // which is either null or another SavedFrame object.
    if (obj->is<SavedFrame>()) {
      SavedFrame* parentFrame;
      if (key.isNull()) {
        parentFrame = nullptr;
      } else if (key.isObject() && key.toObject().is<SavedFrame>()) {
        parentFrame = &key.toObject().as<SavedFrame>();
      } else {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA,
                                  "invalid SavedFrame parent");
        return false;
      }

      if (!childCounter.checkSingleParentFrame()) {
        // This is an error (more than one parent given), but it will be
        // reported when the SavedFrame is complete so it can be handled along
        // with the "no parent given" case.
      } else {
        obj->as<SavedFrame>().initParent(parentFrame);
      }

      continue;
    }

    // Everything else uses a series of key,value,key,value,... Value
    // objects.
    RootedValue val(context());
    if (!startRead(&val) || !childCounter.postStartRead(objs)) {
      return false;
    }

    if (obj->is<MapObject>()) {
      // For a Map, store those <key,value> pairs in the contained map
      // data structure.
      if (!MapObject::set(context(), obj, key, val)) {
        return false;
      }
    } else {
      // For any other Object, interpret them as plain properties.
      RootedId id(context());

      if (!key.isString() && !key.isInt32()) {
        JS_ReportErrorNumberASCII(context(), GetErrorMessage, nullptr,
                                  JSMSG_SC_BAD_SERIALIZED_DATA,
                                  "property key expected");
        return false;
      }

      if (!PrimitiveValueToId<CanGC>(context(), key, &id)) {
        return false;
      }

      if (!DefineDataProperty(context(), obj, id, val)) {
        return false;
      }
    }
  }

  allObjs.clear();

  JSRuntime* rt = context()->runtime();
  rt->addTelemetry(JS_TELEMETRY_DESERIALIZE_BYTES,
                   static_cast<uint32_t>(std::min(nbytes, size_t(MAX_UINT32))));
  rt->addTelemetry(
      JS_TELEMETRY_DESERIALIZE_ITEMS,
      static_cast<uint32_t>(std::min(numItemsRead, size_t(MAX_UINT32))));
  mozilla::TimeDuration elapsed = mozilla::TimeStamp::Now() - startTime;
  rt->addTelemetry(JS_TELEMETRY_DESERIALIZE_US,
                   static_cast<uint32_t>(elapsed.ToMicroseconds()));

  return true;
}

using namespace js;

JS_PUBLIC_API bool JS_ReadStructuredClone(
    JSContext* cx, const JSStructuredCloneData& buf, uint32_t version,
    JS::StructuredCloneScope scope, MutableHandleValue vp,
    const JS::CloneDataPolicy& cloneDataPolicy,
    const JSStructuredCloneCallbacks* optionalCallbacks, void* closure) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (version > JS_STRUCTURED_CLONE_VERSION) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SC_BAD_CLONE_VERSION);
    return false;
  }
  const JSStructuredCloneCallbacks* callbacks = optionalCallbacks;
  return ReadStructuredClone(cx, buf, scope, vp, cloneDataPolicy, callbacks,
                             closure);
}

JS_PUBLIC_API bool JS_WriteStructuredClone(
    JSContext* cx, HandleValue value, JSStructuredCloneData* bufp,
    JS::StructuredCloneScope scope, const JS::CloneDataPolicy& cloneDataPolicy,
    const JSStructuredCloneCallbacks* optionalCallbacks, void* closure,
    HandleValue transferable) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);

  const JSStructuredCloneCallbacks* callbacks = optionalCallbacks;
  return WriteStructuredClone(cx, value, bufp, scope, cloneDataPolicy,
                              callbacks, closure, transferable);
}

JS_PUBLIC_API bool JS_StructuredCloneHasTransferables(
    JSStructuredCloneData& data, bool* hasTransferable) {
  *hasTransferable = StructuredCloneHasTransferObjects(data);
  return true;
}

JS_PUBLIC_API bool JS_StructuredClone(
    JSContext* cx, HandleValue value, MutableHandleValue vp,
    const JSStructuredCloneCallbacks* optionalCallbacks, void* closure) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  // Strings are associated with zones, not compartments,
  // so we copy the string by wrapping it.
  if (value.isString()) {
    RootedString strValue(cx, value.toString());
    if (!cx->compartment()->wrap(cx, &strValue)) {
      return false;
    }
    vp.setString(strValue);
    return true;
  }

  const JSStructuredCloneCallbacks* callbacks = optionalCallbacks;

  JSAutoStructuredCloneBuffer buf(JS::StructuredCloneScope::SameProcess,
                                  callbacks, closure);
  {
    if (value.isObject()) {
      RootedObject obj(cx, &value.toObject());
      obj = CheckedUnwrapStatic(obj);
      if (!obj) {
        ReportAccessDenied(cx);
        return false;
      }
      AutoRealm ar(cx, obj);
      RootedValue unwrappedVal(cx, ObjectValue(*obj));
      if (!buf.write(cx, unwrappedVal, callbacks, closure)) {
        return false;
      }
    } else {
      if (!buf.write(cx, value, callbacks, closure)) {
        return false;
      }
    }
  }

  return buf.read(cx, vp, JS::CloneDataPolicy(), callbacks, closure);
}

JSAutoStructuredCloneBuffer::JSAutoStructuredCloneBuffer(
    JSAutoStructuredCloneBuffer&& other)
    : data_(other.scope()) {
  data_.ownTransferables_ = other.data_.ownTransferables_;
  other.steal(&data_, &version_, &data_.callbacks_, &data_.closure_);
}

JSAutoStructuredCloneBuffer& JSAutoStructuredCloneBuffer::operator=(
    JSAutoStructuredCloneBuffer&& other) {
  MOZ_ASSERT(&other != this);
  MOZ_ASSERT(scope() == other.scope());
  clear();
  data_.ownTransferables_ = other.data_.ownTransferables_;
  other.steal(&data_, &version_, &data_.callbacks_, &data_.closure_);
  return *this;
}

void JSAutoStructuredCloneBuffer::clear() {
  data_.discardTransferables();
  data_.ownTransferables_ = OwnTransferablePolicy::NoTransferables;
  data_.refsHeld_.releaseAll();
  data_.Clear();
  version_ = 0;
}

void JSAutoStructuredCloneBuffer::adopt(
    JSStructuredCloneData&& data, uint32_t version,
    const JSStructuredCloneCallbacks* callbacks, void* closure) {
  clear();
  data_ = std::move(data);
  version_ = version;
  data_.setCallbacks(callbacks, closure,
                     OwnTransferablePolicy::OwnsTransferablesIfAny);
}

void JSAutoStructuredCloneBuffer::steal(
    JSStructuredCloneData* data, uint32_t* versionp,
    const JSStructuredCloneCallbacks** callbacks, void** closure) {
  if (versionp) {
    *versionp = version_;
  }
  if (callbacks) {
    *callbacks = data_.callbacks_;
  }
  if (closure) {
    *closure = data_.closure_;
  }
  *data = std::move(data_);

  version_ = 0;
  data_.setCallbacks(nullptr, nullptr, OwnTransferablePolicy::NoTransferables);
}

bool JSAutoStructuredCloneBuffer::read(
    JSContext* cx, MutableHandleValue vp,
    const JS::CloneDataPolicy& cloneDataPolicy,
    const JSStructuredCloneCallbacks* optionalCallbacks, void* closure) {
  MOZ_ASSERT(cx);
  return !!JS_ReadStructuredClone(
      cx, data_, version_, data_.scope(), vp, cloneDataPolicy,
      optionalCallbacks ? optionalCallbacks : data_.callbacks_,
      optionalCallbacks ? closure : data_.closure_);
}

bool JSAutoStructuredCloneBuffer::write(
    JSContext* cx, HandleValue value,
    const JSStructuredCloneCallbacks* optionalCallbacks, void* closure) {
  HandleValue transferable = UndefinedHandleValue;
  return write(cx, value, transferable, JS::CloneDataPolicy(),
               optionalCallbacks ? optionalCallbacks : data_.callbacks_,
               optionalCallbacks ? closure : data_.closure_);
}

bool JSAutoStructuredCloneBuffer::write(
    JSContext* cx, HandleValue value, HandleValue transferable,
    const JS::CloneDataPolicy& cloneDataPolicy,
    const JSStructuredCloneCallbacks* optionalCallbacks, void* closure) {
  clear();
  bool ok = JS_WriteStructuredClone(
      cx, value, &data_, data_.scopeForInternalWriting(), cloneDataPolicy,
      optionalCallbacks ? optionalCallbacks : data_.callbacks_,
      optionalCallbacks ? closure : data_.closure_, transferable);

  if (ok) {
    data_.ownTransferables_ = OwnTransferablePolicy::OwnsTransferablesIfAny;
  } else {
    version_ = JS_STRUCTURED_CLONE_VERSION;
    data_.ownTransferables_ = OwnTransferablePolicy::NoTransferables;
  }
  return ok;
}

JS_PUBLIC_API bool JS_ReadUint32Pair(JSStructuredCloneReader* r, uint32_t* p1,
                                     uint32_t* p2) {
  return r->input().readPair((uint32_t*)p1, (uint32_t*)p2);
}

JS_PUBLIC_API bool JS_ReadBytes(JSStructuredCloneReader* r, void* p,
                                size_t len) {
  return r->input().readBytes(p, len);
}

JS_PUBLIC_API bool JS_ReadTypedArray(JSStructuredCloneReader* r,
                                     MutableHandleValue vp) {
  uint32_t tag, data;
  if (!r->input().readPair(&tag, &data)) {
    return false;
  }

  if (tag >= SCTAG_TYPED_ARRAY_V1_MIN && tag <= SCTAG_TYPED_ARRAY_V1_MAX) {
    return r->readTypedArray(TagToV1ArrayType(tag), data, vp, true);
  }

  if (tag == SCTAG_TYPED_ARRAY_OBJECT_V2) {
    // V2 stores the length (nelems) in |data| and the arrayType separately.
    uint64_t arrayType;
    if (!r->input().read(&arrayType)) {
      return false;
    }
    uint64_t nelems = data;
    return r->readTypedArray(arrayType, nelems, vp);
  }

  if (tag == SCTAG_TYPED_ARRAY_OBJECT) {
    // The current version stores the array type in |data| and the length
    // (nelems) separately to support large TypedArrays.
    uint32_t arrayType = data;
    uint64_t nelems;
    if (!r->input().read(&nelems)) {
      return false;
    }
    return r->readTypedArray(arrayType, nelems, vp);
  }

  JS_ReportErrorNumberASCII(r->context(), GetErrorMessage, nullptr,
                            JSMSG_SC_BAD_SERIALIZED_DATA,
                            "expected type array");
  return false;
}

JS_PUBLIC_API bool JS_WriteUint32Pair(JSStructuredCloneWriter* w, uint32_t tag,
                                      uint32_t data) {
  return w->output().writePair(tag, data);
}

JS_PUBLIC_API bool JS_WriteBytes(JSStructuredCloneWriter* w, const void* p,
                                 size_t len) {
  return w->output().writeBytes(p, len);
}

JS_PUBLIC_API bool JS_WriteString(JSStructuredCloneWriter* w,
                                  HandleString str) {
  return w->writeString(SCTAG_STRING, str);
}

JS_PUBLIC_API bool JS_WriteTypedArray(JSStructuredCloneWriter* w,
                                      HandleValue v) {
  MOZ_ASSERT(v.isObject());
  w->context()->check(v);
  RootedObject obj(w->context(), &v.toObject());

  // startWrite can write everything, thus we should check here
  // and report error if the user passes a wrong type.
  if (!obj->canUnwrapAs<TypedArrayObject>()) {
    ReportAccessDenied(w->context());
    return false;
  }

  // We should use startWrite instead of writeTypedArray, because
  // typed array is an object, we should add it to the |memory|
  // (allObjs) list. Directly calling writeTypedArray won't add it.
  return w->startWrite(v);
}

JS_PUBLIC_API bool JS_ObjectNotWritten(JSStructuredCloneWriter* w,
                                       HandleObject obj) {
  w->memory.remove(w->memory.lookup(obj));

  return true;
}

JS_PUBLIC_API JS::StructuredCloneScope JS_GetStructuredCloneScope(
    JSStructuredCloneWriter* w) {
  return w->output().scope();
}

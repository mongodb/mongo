/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The compiled representation of a RegExp, potentially shared among RegExp
 * instances created during separate evaluations of a single RegExp literal in
 * source code.
 */

#ifndef vm_RegExpShared_h
#define vm_RegExpShared_h

#include "mozilla/Assertions.h"
#include "mozilla/MemoryReporting.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "gc/ZoneAllocator.h"
#include "irregexp/RegExpTypes.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "js/AllocPolicy.h"
#include "js/RegExpFlags.h"  // JS::RegExpFlag, JS::RegExpFlags
#include "js/UbiNode.h"
#include "js/Vector.h"
#include "vm/ArrayObject.h"
#include "vm/JSAtom.h"

namespace js {

class ArrayObject;
class RegExpRealm;
class RegExpShared;
class RegExpStatics;
class VectorMatchPairs;

using RootedRegExpShared = JS::Rooted<RegExpShared*>;
using HandleRegExpShared = JS::Handle<RegExpShared*>;
using MutableHandleRegExpShared = JS::MutableHandle<RegExpShared*>;

enum RegExpRunStatus : int32_t {
  RegExpRunStatus_Error = -1,
  RegExpRunStatus_Success = 1,
  RegExpRunStatus_Success_NotFound = 0,
};

inline bool IsNativeRegExpEnabled() {
#ifdef JS_CODEGEN_NONE
  return false;
#else
  return jit::JitOptions.nativeRegExp;
#endif
}

/*
 * A RegExpShared is the compiled representation of a regexp. A RegExpShared is
 * potentially pointed to by multiple RegExpObjects. Additionally, C++ code may
 * have pointers to RegExpShareds on the stack. The RegExpShareds are kept in a
 * table so that they can be reused when compiling the same regex string.
 *
 * To save memory, a RegExpShared is not created for a RegExpObject until it is
 * needed for execution. When a RegExpShared needs to be created, it is looked
 * up in a per-compartment table to allow reuse between objects.
 *
 * During a GC, RegExpShared instances are marked and swept like GC things.
 * Usually, RegExpObjects clear their pointers to their RegExpShareds rather
 * than explicitly tracing them, so that the RegExpShared and any jitcode can
 * be reclaimed quicker. However, the RegExpShareds are traced through by
 * objects when we are preserving jitcode in their zone, to avoid the same
 * recompilation inefficiencies as normal Ion and baseline compilation.
 */
class RegExpShared
    : public gc::CellWithTenuredGCPointer<gc::TenuredCell, JSAtom> {
 public:
  enum class Kind { Unparsed, Atom, RegExp };
  enum class CodeKind { Bytecode, Jitcode, Any };

  using ByteCode = js::irregexp::ByteArrayData;
  using JitCodeTable = js::irregexp::ByteArray;
  using JitCodeTables = Vector<JitCodeTable, 0, SystemAllocPolicy>;

 private:
  friend class RegExpStatics;
  friend class RegExpZone;

  struct RegExpCompilation {
    WeakHeapPtr<jit::JitCode*> jitCode;
    ByteCode* byteCode = nullptr;

    bool compiled(CodeKind kind = CodeKind::Any) const {
      switch (kind) {
        case CodeKind::Bytecode:
          return !!byteCode;
        case CodeKind::Jitcode:
          return !!jitCode;
        case CodeKind::Any:
          return !!byteCode || !!jitCode;
      }
      MOZ_CRASH("Unreachable");
    }

    size_t byteCodeLength() const {
      MOZ_ASSERT(byteCode);
      return byteCode->length;
    }
  };

 public:
  /* Source to the RegExp, for lazy compilation. Stored in the cell header. */
  JSAtom* getSource() const { return headerPtr(); }

 private:
  RegExpCompilation compilationArray[2];

  uint32_t pairCount_;
  JS::RegExpFlags flags;

  RegExpShared::Kind kind_ = Kind::Unparsed;
  GCPtrAtom patternAtom_;
  uint32_t maxRegisters_ = 0;
  uint32_t ticks_ = 0;

  uint32_t numNamedCaptures_ = {};
  uint32_t* namedCaptureIndices_ = {};
  GCPtr<PlainObject*> groupsTemplate_ = {};

  static int CompilationIndex(bool latin1) { return latin1 ? 0 : 1; }

  // Tables referenced by JIT code.
  JitCodeTables tables;

  /* Internal functions. */
  RegExpShared(JSAtom* source, JS::RegExpFlags flags);

  const RegExpCompilation& compilation(bool latin1) const {
    return compilationArray[CompilationIndex(latin1)];
  }

  RegExpCompilation& compilation(bool latin1) {
    return compilationArray[CompilationIndex(latin1)];
  }

 public:
  ~RegExpShared() = delete;

  static bool compileIfNecessary(JSContext* cx, MutableHandleRegExpShared res,
                                 HandleLinearString input, CodeKind code);

  static RegExpRunStatus executeAtom(MutableHandleRegExpShared re,
                                     HandleLinearString input, size_t start,
                                     VectorMatchPairs* matches);

  // Execute this RegExp on input starting from searchIndex, filling in matches.
  static RegExpRunStatus execute(JSContext* cx, MutableHandleRegExpShared res,
                                 HandleLinearString input, size_t searchIndex,
                                 VectorMatchPairs* matches);

  // Register a table with this RegExpShared, and take ownership.
  bool addTable(JitCodeTable table) { return tables.append(std::move(table)); }

  /* Accessors */

  size_t pairCount() const {
    MOZ_ASSERT(kind() != Kind::Unparsed);
    return pairCount_;
  }

  RegExpShared::Kind kind() const { return kind_; }

  // Use simple string matching for this regexp.
  void useAtomMatch(HandleAtom pattern);

  // Use the regular expression engine for this regexp.
  void useRegExpMatch(size_t parenCount);

  static bool initializeNamedCaptures(JSContext* cx, HandleRegExpShared re,
                                      HandleNativeObject namedCaptures);
  PlainObject* getGroupsTemplate() { return groupsTemplate_; }

  void tierUpTick();
  bool markedForTierUp() const;

  void setByteCode(ByteCode* code, bool latin1) {
    compilation(latin1).byteCode = code;
  }
  ByteCode* getByteCode(bool latin1) const {
    return compilation(latin1).byteCode;
  }
  void setJitCode(jit::JitCode* code, bool latin1) {
    compilation(latin1).jitCode = code;
  }
  jit::JitCode* getJitCode(bool latin1) const {
    return compilation(latin1).jitCode;
  }
  uint32_t getMaxRegisters() const { return maxRegisters_; }
  void updateMaxRegisters(uint32_t numRegisters) {
    maxRegisters_ = std::max(maxRegisters_, numRegisters);
  }

  uint32_t numNamedCaptures() const { return numNamedCaptures_; }
  int32_t getNamedCaptureIndex(uint32_t idx) const {
    MOZ_ASSERT(idx < numNamedCaptures());
    MOZ_ASSERT(namedCaptureIndices_);
    return namedCaptureIndices_[idx];
  }

  JSAtom* patternAtom() const { return patternAtom_; }

  JS::RegExpFlags getFlags() const { return flags; }

  bool hasIndices() const { return flags.hasIndices(); }
  bool global() const { return flags.global(); }
  bool ignoreCase() const { return flags.ignoreCase(); }
  bool multiline() const { return flags.multiline(); }
  bool dotAll() const { return flags.dotAll(); }
  bool unicode() const { return flags.unicode(); }
  bool sticky() const { return flags.sticky(); }

  bool isCompiled(bool latin1, CodeKind codeKind = CodeKind::Any) const {
    return compilation(latin1).compiled(codeKind);
  }
  bool isCompiled() const { return isCompiled(true) || isCompiled(false); }

  void traceChildren(JSTracer* trc);
  void discardJitCode();
  void finalize(JSFreeOp* fop);

  static size_t offsetOfSource() { return offsetOfHeaderPtr(); }

  static size_t offsetOfPatternAtom() {
    return offsetof(RegExpShared, patternAtom_);
  }

  static size_t offsetOfFlags() { return offsetof(RegExpShared, flags); }

  static size_t offsetOfPairCount() {
    return offsetof(RegExpShared, pairCount_);
  }

  static size_t offsetOfJitCode(bool latin1) {
    return offsetof(RegExpShared, compilationArray) +
           (CompilationIndex(latin1) * sizeof(RegExpCompilation)) +
           offsetof(RegExpCompilation, jitCode);
  }

  static size_t offsetOfGroupsTemplate() {
    return offsetof(RegExpShared, groupsTemplate_);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

#ifdef DEBUG
  static bool dumpBytecode(JSContext* cx, MutableHandleRegExpShared res,
                           HandleLinearString input);
#endif

 public:
  static const JS::TraceKind TraceKind = JS::TraceKind::RegExpShared;
};

class RegExpZone {
  struct Key {
    JSAtom* atom = nullptr;
    JS::RegExpFlags flags = JS::RegExpFlag::NoFlags;

    Key() = default;
    Key(JSAtom* atom, JS::RegExpFlags flags) : atom(atom), flags(flags) {}
    MOZ_IMPLICIT Key(const WeakHeapPtr<RegExpShared*>& shared)
        : atom(shared.unbarrieredGet()->getSource()),
          flags(shared.unbarrieredGet()->getFlags()) {}

    using Lookup = Key;
    static HashNumber hash(const Lookup& l) {
      HashNumber hash = DefaultHasher<JSAtom*>::hash(l.atom);
      return mozilla::AddToHash(hash, l.flags.value());
    }
    static bool match(Key l, Key r) {
      return l.atom == r.atom && l.flags == r.flags;
    }
  };

  /*
   * The set of all RegExpShareds in the zone. On every GC, every RegExpShared
   * that was not marked is deleted and removed from the set.
   */
  using Set = JS::WeakCache<
      JS::GCHashSet<WeakHeapPtr<RegExpShared*>, Key, ZoneAllocPolicy>>;
  Set set_;

 public:
  explicit RegExpZone(Zone* zone);

  ~RegExpZone() { MOZ_ASSERT(set_.empty()); }

  bool empty() const { return set_.empty(); }

  RegExpShared* maybeGet(JSAtom* source, JS::RegExpFlags flags) const {
    Set::Ptr p = set_.lookup(Key(source, flags));
    return p ? *p : nullptr;
  }

  RegExpShared* get(JSContext* cx, HandleAtom source, JS::RegExpFlags flags);

#ifdef DEBUG
  void clear() { set_.clear(); }
#endif

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

class RegExpRealm {
 public:
  enum ResultTemplateKind { Normal, WithIndices, Indices, NumKinds };

 private:
  /*
   * The template objects that the result of re.exec() is based on, if
   * there is a result. These are used in CreateRegExpMatchResult.
   * There are three template objects, each of which is an ArrayObject
   * with some additional properties. We decide which to use based on
   * the |hasIndices| (/d) flag.
   *
   *  Normal: Has |index|, |input|, and |groups| properties.
   *          Used for the result object if |hasIndices| is not set.
   *
   *  WithIndices: Has |index|, |input|, |groups|, and |indices| properties.
   *               Used for the result object if |hasIndices| is set.
   *
   *  Indices: Has a |groups| property. If |hasIndices| is set, used
   *           for the |.indices| property of the result object.
   */
  WeakHeapPtr<ArrayObject*>
      matchResultTemplateObjects_[ResultTemplateKind::NumKinds];

  /*
   * The shape of RegExp.prototype object that satisfies following:
   *   * RegExp.prototype.flags getter is not modified
   *   * RegExp.prototype.global getter is not modified
   *   * RegExp.prototype.ignoreCase getter is not modified
   *   * RegExp.prototype.multiline getter is not modified
   *   * RegExp.prototype.dotAll getter is not modified
   *   * RegExp.prototype.sticky getter is not modified
   *   * RegExp.prototype.unicode getter is not modified
   *   * RegExp.prototype.exec is an own data property
   *   * RegExp.prototype[@@match] is an own data property
   *   * RegExp.prototype[@@search] is an own data property
   */
  WeakHeapPtr<Shape*> optimizableRegExpPrototypeShape_;

  /*
   * The shape of RegExp instance that satisfies following:
   *   * lastProperty is lastIndex
   *   * prototype is RegExp.prototype
   */
  WeakHeapPtr<Shape*> optimizableRegExpInstanceShape_;

  ArrayObject* createMatchResultTemplateObject(JSContext* cx,
                                               ResultTemplateKind kind);

 public:
  explicit RegExpRealm();

  void traceWeak(JSTracer* trc);

  static const size_t MatchResultObjectIndexSlot = 0;
  static const size_t MatchResultObjectInputSlot = 1;
  static const size_t MatchResultObjectGroupsSlot = 2;
  static const size_t MatchResultObjectIndicesSlot = 3;

  static const size_t IndicesGroupsSlot = 0;

  static size_t offsetOfMatchResultObjectIndexSlot() {
    return sizeof(Value) * MatchResultObjectIndexSlot;
  }
  static size_t offsetOfMatchResultObjectInputSlot() {
    return sizeof(Value) * MatchResultObjectInputSlot;
  }
  static size_t offsetOfMatchResultObjectGroupsSlot() {
    return sizeof(Value) * MatchResultObjectGroupsSlot;
  }
  static size_t offsetOfMatchResultObjectIndicesSlot() {
    return sizeof(Value) * MatchResultObjectIndicesSlot;
  }

  /* Get or create template object used to base the result of .exec() on. */
  ArrayObject* getOrCreateMatchResultTemplateObject(
      JSContext* cx, ResultTemplateKind kind = ResultTemplateKind::Normal) {
    if (matchResultTemplateObjects_[kind]) {
      return matchResultTemplateObjects_[kind];
    }
    return createMatchResultTemplateObject(cx, kind);
  }

  Shape* getOptimizableRegExpPrototypeShape() {
    return optimizableRegExpPrototypeShape_;
  }
  void setOptimizableRegExpPrototypeShape(Shape* shape) {
    optimizableRegExpPrototypeShape_ = shape;
  }
  Shape* getOptimizableRegExpInstanceShape() {
    return optimizableRegExpInstanceShape_;
  }
  void setOptimizableRegExpInstanceShape(Shape* shape) {
    optimizableRegExpInstanceShape_ = shape;
  }

  static size_t offsetOfOptimizableRegExpPrototypeShape() {
    return offsetof(RegExpRealm, optimizableRegExpPrototypeShape_);
  }
  static size_t offsetOfOptimizableRegExpInstanceShape() {
    return offsetof(RegExpRealm, optimizableRegExpInstanceShape_);
  }
};

RegExpRunStatus ExecuteRegExpAtomRaw(RegExpShared* re, JSLinearString* input,
                                     size_t start, MatchPairs* matchPairs);

} /* namespace js */

namespace JS {
namespace ubi {

template <>
class Concrete<js::RegExpShared> : TracerConcrete<js::RegExpShared> {
 protected:
  explicit Concrete(js::RegExpShared* ptr)
      : TracerConcrete<js::RegExpShared>(ptr) {}

 public:
  static void construct(void* storage, js::RegExpShared* ptr) {
    new (storage) Concrete(ptr);
  }

  CoarseType coarseType() const final { return CoarseType::Other; }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif /* vm_RegExpShared_h */

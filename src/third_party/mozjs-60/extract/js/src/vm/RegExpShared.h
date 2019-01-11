/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The compiled representation of a RegExp, potentially shared among RegExp instances created
 * during separate evaluations of a single RegExp literal in source code.
 */

#ifndef vm_RegExpShared_h
#define vm_RegExpShared_h

#include "mozilla/Assertions.h"
#include "mozilla/MemoryReporting.h"

#include "builtin/SelfHostingDefines.h"
#include "gc/Barrier.h"
#include "gc/Heap.h"
#include "gc/Marking.h"
#include "js/AllocPolicy.h"
#include "js/UbiNode.h"
#include "js/Vector.h"
#include "vm/ArrayObject.h"
#include "vm/JSAtom.h"

namespace js {

class ArrayObject;
class RegExpCompartment;
class RegExpShared;
class RegExpStatics;
class VectorMatchPairs;

using RootedRegExpShared = JS::Rooted<RegExpShared*>;
using HandleRegExpShared = JS::Handle<RegExpShared*>;
using MutableHandleRegExpShared = JS::MutableHandle<RegExpShared*>;

enum RegExpFlag : uint8_t
{
    IgnoreCaseFlag  = 0x01,
    GlobalFlag      = 0x02,
    MultilineFlag   = 0x04,
    StickyFlag      = 0x08,
    UnicodeFlag     = 0x10,

    NoFlags         = 0x00,
    AllFlags        = 0x1f
};

static_assert(IgnoreCaseFlag == REGEXP_IGNORECASE_FLAG &&
              GlobalFlag == REGEXP_GLOBAL_FLAG &&
              MultilineFlag == REGEXP_MULTILINE_FLAG &&
              StickyFlag == REGEXP_STICKY_FLAG &&
              UnicodeFlag == REGEXP_UNICODE_FLAG,
              "Flag values should be in sync with self-hosted JS");

enum RegExpRunStatus
{
    RegExpRunStatus_Error,
    RegExpRunStatus_Success,
    RegExpRunStatus_Success_NotFound
};

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
class RegExpShared : public gc::TenuredCell
{
  public:
    enum CompilationMode {
        Normal,
        MatchOnly
    };

    enum ForceByteCodeEnum {
        DontForceByteCode,
        ForceByteCode
    };

    using JitCodeTable = UniquePtr<uint8_t[], JS::FreePolicy>;
    using JitCodeTables = Vector<JitCodeTable, 0, SystemAllocPolicy>;

  private:
    friend class RegExpStatics;
    friend class RegExpZone;

    struct RegExpCompilation
    {
        ReadBarriered<jit::JitCode*> jitCode;
        uint8_t* byteCode;

        RegExpCompilation() : byteCode(nullptr) {}

        bool compiled(ForceByteCodeEnum force = DontForceByteCode) const {
            return byteCode || (force == DontForceByteCode && jitCode);
        }
    };

    /* Source to the RegExp, for lazy compilation. */
    GCPtr<JSAtom*>     source;

    RegExpFlag         flags;
    bool               canStringMatch;
    size_t             parenCount;

    RegExpCompilation  compilationArray[4];

    static int CompilationIndex(CompilationMode mode, bool latin1) {
        switch (mode) {
          case Normal:    return latin1 ? 0 : 1;
          case MatchOnly: return latin1 ? 2 : 3;
        }
        MOZ_CRASH();
    }

    // Tables referenced by JIT code.
    JitCodeTables tables;

    /* Internal functions. */
    RegExpShared(JSAtom* source, RegExpFlag flags);

    static bool compile(JSContext* cx, MutableHandleRegExpShared res, HandleLinearString input,
                        CompilationMode mode, ForceByteCodeEnum force);
    static bool compile(JSContext* cx, MutableHandleRegExpShared res, HandleAtom pattern,
                        HandleLinearString input, CompilationMode mode, ForceByteCodeEnum force);

    static bool compileIfNecessary(JSContext* cx, MutableHandleRegExpShared res,
                                   HandleLinearString input, CompilationMode mode,
                                   ForceByteCodeEnum force);

    const RegExpCompilation& compilation(CompilationMode mode, bool latin1) const {
        return compilationArray[CompilationIndex(mode, latin1)];
    }

    RegExpCompilation& compilation(CompilationMode mode, bool latin1) {
        return compilationArray[CompilationIndex(mode, latin1)];
    }

  public:
    ~RegExpShared() = delete;

    // Execute this RegExp on input starting from searchIndex, filling in
    // matches if specified and otherwise only determining if there is a match.
    static RegExpRunStatus execute(JSContext* cx, MutableHandleRegExpShared res,
                                   HandleLinearString input, size_t searchIndex,
                                   VectorMatchPairs* matches, size_t* endIndex);

    // Register a table with this RegExpShared, and take ownership.
    bool addTable(JitCodeTable table) {
        return tables.append(Move(table));
    }

    /* Accessors */

    size_t getParenCount() const {
        MOZ_ASSERT(isCompiled());
        return parenCount;
    }

    /* Accounts for the "0" (whole match) pair. */
    size_t pairCount() const            { return getParenCount() + 1; }

    JSAtom* getSource() const           { return source; }
    RegExpFlag getFlags() const         { return flags; }
    bool ignoreCase() const             { return flags & IgnoreCaseFlag; }
    bool global() const                 { return flags & GlobalFlag; }
    bool multiline() const              { return flags & MultilineFlag; }
    bool sticky() const                 { return flags & StickyFlag; }
    bool unicode() const                { return flags & UnicodeFlag; }

    bool isCompiled(CompilationMode mode, bool latin1,
                    ForceByteCodeEnum force = DontForceByteCode) const {
        return compilation(mode, latin1).compiled(force);
    }
    bool isCompiled() const {
        return isCompiled(Normal, true) || isCompiled(Normal, false)
            || isCompiled(MatchOnly, true) || isCompiled(MatchOnly, false);
    }

    void traceChildren(JSTracer* trc);
    void discardJitCode();
    void finalize(FreeOp* fop);

    static size_t offsetOfSource() {
        return offsetof(RegExpShared, source);
    }

    static size_t offsetOfFlags() {
        return offsetof(RegExpShared, flags);
    }

    static size_t offsetOfParenCount() {
        return offsetof(RegExpShared, parenCount);
    }

    static size_t offsetOfLatin1JitCode(CompilationMode mode) {
        return offsetof(RegExpShared, compilationArray)
             + (CompilationIndex(mode, true) * sizeof(RegExpCompilation))
             + offsetof(RegExpCompilation, jitCode);
    }
    static size_t offsetOfTwoByteJitCode(CompilationMode mode) {
        return offsetof(RegExpShared, compilationArray)
             + (CompilationIndex(mode, false) * sizeof(RegExpCompilation))
             + offsetof(RegExpCompilation, jitCode);
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

#ifdef DEBUG
    static bool dumpBytecode(JSContext* cx, MutableHandleRegExpShared res, bool match_only,
                             HandleLinearString input);
#endif
};

class RegExpZone
{
    struct Key {
        JSAtom* atom;
        uint16_t flag;

        Key() {}
        Key(JSAtom* atom, RegExpFlag flag)
          : atom(atom), flag(flag)
        { }
        MOZ_IMPLICIT Key(const ReadBarriered<RegExpShared*>& shared)
          : atom(shared.unbarrieredGet()->getSource()),
            flag(shared.unbarrieredGet()->getFlags())
        { }

        typedef Key Lookup;
        static HashNumber hash(const Lookup& l) {
            HashNumber hash = DefaultHasher<JSAtom*>::hash(l.atom);
            return mozilla::AddToHash(hash, l.flag);
        }
        static bool match(Key l, Key r) {
            return l.atom == r.atom && l.flag == r.flag;
        }
    };

    /*
     * The set of all RegExpShareds in the zone. On every GC, every RegExpShared
     * that was not marked is deleted and removed from the set.
     */
    using Set = JS::WeakCache<JS::GCHashSet<ReadBarriered<RegExpShared*>, Key, ZoneAllocPolicy>>;
    Set set_;

  public:
    explicit RegExpZone(Zone* zone);

    ~RegExpZone() {
        MOZ_ASSERT_IF(set_.initialized(), set_.empty());
    }

    bool init();

    bool empty() const { return set_.empty(); }

    RegExpShared* maybeGet(JSAtom* source, RegExpFlag flags) const {
        Set::Ptr p = set_.lookup(Key(source, flags));
        return p ? *p : nullptr;
    }

    RegExpShared* get(JSContext* cx, HandleAtom source, RegExpFlag flags);

    /* Like 'get', but compile 'maybeOpt' (if non-null). */
    RegExpShared* get(JSContext* cx, HandleAtom source, JSString* maybeOpt);

#ifdef DEBUG
    void clear() { set_.clear(); }
#endif

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
};

class RegExpCompartment
{
    /*
     * This is the template object where the result of re.exec() is based on,
     * if there is a result. This is used in CreateRegExpMatchResult to set
     * the input/index properties faster.
     */
    ReadBarriered<ArrayObject*> matchResultTemplateObject_;

    /*
     * The shape of RegExp.prototype object that satisfies following:
     *   * RegExp.prototype.flags getter is not modified
     *   * RegExp.prototype.global getter is not modified
     *   * RegExp.prototype.ignoreCase getter is not modified
     *   * RegExp.prototype.multiline getter is not modified
     *   * RegExp.prototype.sticky getter is not modified
     *   * RegExp.prototype.unicode getter is not modified
     *   * RegExp.prototype.exec is an own data property
     *   * RegExp.prototype[@@match] is an own data property
     *   * RegExp.prototype[@@search] is an own data property
     */
    ReadBarriered<Shape*> optimizableRegExpPrototypeShape_;

    /*
     * The shape of RegExp instance that satisfies following:
     *   * lastProperty is lastIndex
     *   * prototype is RegExp.prototype
     */
    ReadBarriered<Shape*> optimizableRegExpInstanceShape_;

    ArrayObject* createMatchResultTemplateObject(JSContext* cx);

  public:
    explicit RegExpCompartment();

    void sweep();

    /* Get or create template object used to base the result of .exec() on. */
    ArrayObject* getOrCreateMatchResultTemplateObject(JSContext* cx) {
        if (matchResultTemplateObject_)
            return matchResultTemplateObject_;
        return createMatchResultTemplateObject(cx);
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
        return offsetof(RegExpCompartment, optimizableRegExpPrototypeShape_);
    }
    static size_t offsetOfOptimizableRegExpInstanceShape() {
        return offsetof(RegExpCompartment, optimizableRegExpInstanceShape_);
    }
};

} /* namespace js */

namespace JS {
namespace ubi {

template <>
class Concrete<js::RegExpShared> : TracerConcrete<js::RegExpShared>
{
  protected:
    explicit Concrete(js::RegExpShared* ptr) : TracerConcrete<js::RegExpShared>(ptr) { }

  public:
    static void construct(void* storage, js::RegExpShared* ptr) {
        new (storage) Concrete(ptr);
    }

    CoarseType coarseType() const final { return CoarseType::Other; }

    Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

    const char16_t* typeName() const override { return concreteTypeName; }
    static const char16_t concreteTypeName[];
};

} // namespace ubi
} // namespace JS

#endif /* vm_RegExpShared_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS script descriptor. */

#ifndef jsscript_h
#define jsscript_h

#include "mozilla/Atomics.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include "jsatom.h"
#include "jslock.h"
#include "jsopcode.h"
#include "jstypes.h"

#include "gc/Barrier.h"
#include "gc/Rooting.h"
#include "jit/IonCode.h"
#include "js/UbiNode.h"
#include "vm/NativeObject.h"
#include "vm/Shape.h"

namespace JS {
struct ScriptSourceInfo;
} // namespace JS

namespace js {

namespace jit {
    struct BaselineScript;
    struct IonScriptCounts;
} // namespace jit

# define ION_DISABLED_SCRIPT ((js::jit::IonScript*)0x1)
# define ION_COMPILING_SCRIPT ((js::jit::IonScript*)0x2)
# define ION_PENDING_SCRIPT ((js::jit::IonScript*)0x3)

# define BASELINE_DISABLED_SCRIPT ((js::jit::BaselineScript*)0x1)

class BreakpointSite;
class BindingIter;
class Debugger;
class LazyScript;
class ModuleObject;
class NestedScopeObject;
class RegExpObject;
struct SourceCompressionTask;
class Shape;

namespace frontend {
    struct BytecodeEmitter;
    class UpvarCookie;
    class FunctionBox;
    class ModuleBox;
} // namespace frontend

namespace detail {

// Do not call this directly! It is exposed for the friend declarations in
// this file.
bool
CopyScript(JSContext* cx, HandleObject scriptStaticScope, HandleScript src, HandleScript dst);

} // namespace detail

} // namespace js

/*
 * Type of try note associated with each catch or finally block, and also with
 * for-in and other kinds of loops. Non-for-in loops do not need these notes
 * for exception unwinding, but storing their boundaries here is helpful for
 * heuristics that need to know whether a given op is inside a loop.
 */
enum JSTryNoteKind {
    JSTRY_CATCH,
    JSTRY_FINALLY,
    JSTRY_FOR_IN,
    JSTRY_FOR_OF,
    JSTRY_LOOP
};

/*
 * Exception handling record.
 */
struct JSTryNote {
    uint8_t         kind;       /* one of JSTryNoteKind */
    uint32_t        stackDepth; /* stack depth upon exception handler entry */
    uint32_t        start;      /* start of the try statement or loop
                                   relative to script->main */
    uint32_t        length;     /* length of the try statement or loop */
};

namespace js {

// A block scope has a range in bytecode: it is entered at some offset, and left
// at some later offset.  Scopes can be nested.  Given an offset, the
// BlockScopeNote containing that offset whose with the highest start value
// indicates the block scope.  The block scope list is sorted by increasing
// start value.
//
// It is possible to leave a scope nonlocally, for example via a "break"
// statement, so there may be short bytecode ranges in a block scope in which we
// are popping the block chain in preparation for a goto.  These exits are also
// nested with respect to outer scopes.  The scopes in these exits are indicated
// by the "index" field, just like any other block.  If a nonlocal exit pops the
// last block scope, the index will be NoBlockScopeIndex.
//
struct BlockScopeNote {
    static const uint32_t NoBlockScopeIndex = UINT32_MAX;

    uint32_t        index;      // Index of NestedScopeObject in the object
                                // array, or NoBlockScopeIndex if there is no
                                // block scope in this range.
    uint32_t        start;      // Bytecode offset at which this scope starts,
                                // from script->main().
    uint32_t        length;     // Bytecode length of scope.
    uint32_t        parent;     // Index of parent block scope in notes, or UINT32_MAX.
};

struct ConstArray {
    js::HeapValue*  vector;    /* array of indexed constant values */
    uint32_t        length;
};

struct ObjectArray {
    js::HeapPtrObject* vector;  // Array of indexed objects.
    uint32_t        length;     // Count of indexed objects.
};

struct TryNoteArray {
    JSTryNote*      vector;    // Array of indexed try notes.
    uint32_t        length;     // Count of indexed try notes.
};

struct BlockScopeArray {
    BlockScopeNote* vector;     // Array of indexed BlockScopeNote records.
    uint32_t        length;     // Count of indexed try notes.
};

class YieldOffsetArray {
    friend bool
    detail::CopyScript(JSContext* cx, HandleObject scriptStaticScope, HandleScript src,
                       HandleScript dst);

    uint32_t*       vector_;   // Array of bytecode offsets.
    uint32_t        length_;    // Count of bytecode offsets.

  public:
    void init(uint32_t* vector, uint32_t length) {
        vector_ = vector;
        length_ = length;
    }
    uint32_t& operator[](uint32_t index) {
        MOZ_ASSERT(index < length_);
        return vector_[index];
    }
    uint32_t length() const {
        return length_;
    }
};

class Binding : public JS::Traceable
{
    // One JSScript stores one Binding per formal/variable so we use a
    // packed-word representation.
    uintptr_t bits_;

    static const uintptr_t KIND_MASK = 0x3;
    static const uintptr_t ALIASED_BIT = 0x4;
    static const uintptr_t NAME_MASK = ~(KIND_MASK | ALIASED_BIT);

  public:
    // A "binding" is a formal parameter, 'var' (also a stand in for
    // body-level 'let' declarations), or 'const' declaration. A function's
    // lexical scope is composed of these three kinds of bindings.
    enum Kind { ARGUMENT, VARIABLE, CONSTANT };

    explicit Binding() : bits_(0) {}

    Binding(PropertyName* name, Kind kind, bool aliased) {
        JS_STATIC_ASSERT(CONSTANT <= KIND_MASK);
        MOZ_ASSERT((uintptr_t(name) & ~NAME_MASK) == 0);
        MOZ_ASSERT((uintptr_t(kind) & ~KIND_MASK) == 0);
        bits_ = uintptr_t(name) | uintptr_t(kind) | (aliased ? ALIASED_BIT : 0);
    }

    PropertyName* name() const {
        return (PropertyName*)(bits_ & NAME_MASK);
    }

    Kind kind() const {
        return Kind(bits_ & KIND_MASK);
    }

    bool aliased() const {
        return bool(bits_ & ALIASED_BIT);
    }

    static void trace(Binding* self, JSTracer* trc) { self->trace(trc); }
    void trace(JSTracer* trc);
};

JS_STATIC_ASSERT(sizeof(Binding) == sizeof(uintptr_t));

/*
 * Formal parameters and local variables are stored in a shape tree
 * path encapsulated within this class.  This class represents bindings for
 * both function and top-level scripts (the latter is needed to track names in
 * strict mode eval code, to give such code its own lexical environment).
 */
class Bindings : public JS::Traceable
{
    friend class BindingIter;
    friend class AliasedFormalIter;
    template <typename Outer> friend class BindingsOperations;
    template <typename Outer> friend class MutableBindingsOperations;

    RelocatablePtrShape callObjShape_;
    uintptr_t bindingArrayAndFlag_;
    uint16_t numArgs_;
    uint16_t numBlockScoped_;
    uint16_t numBodyLevelLexicals_;
    uint16_t aliasedBodyLevelLexicalBegin_;
    uint16_t numUnaliasedBodyLevelLexicals_;
    uint32_t numVars_;
    uint32_t numUnaliasedVars_;

#if JS_BITS_PER_WORD == 32
    // Bindings is allocated inline inside JSScript, which needs to be
    // gc::Cell aligned.
    uint32_t padding_;
#endif

    /*
     * During parsing, bindings are allocated out of a temporary LifoAlloc.
     * After parsing, a JSScript object is created and the bindings are
     * permanently transferred to it. On error paths, the JSScript object may
     * end up with bindings that still point to the (new released) LifoAlloc
     * memory. To avoid tracing these bindings during GC, we keep track of
     * whether the bindings are temporary or permanent in the low bit of
     * bindingArrayAndFlag_.
     */
    static const uintptr_t TEMPORARY_STORAGE_BIT = 0x1;
    bool bindingArrayUsingTemporaryStorage() const {
        return bindingArrayAndFlag_ & TEMPORARY_STORAGE_BIT;
    }

  public:

    Binding* bindingArray() const {
        return reinterpret_cast<Binding*>(bindingArrayAndFlag_ & ~TEMPORARY_STORAGE_BIT);
    }

    Bindings()
      : callObjShape_(nullptr), bindingArrayAndFlag_(TEMPORARY_STORAGE_BIT),
        numArgs_(0), numBlockScoped_(0),
        numBodyLevelLexicals_(0), numUnaliasedBodyLevelLexicals_(0),
        numVars_(0), numUnaliasedVars_(0)
    {}

    /*
     * Initialize a Bindings with a pointer into temporary storage.
     * bindingArray must have length numArgs + numVars +
     * numBodyLevelLexicals. Before the temporary storage is release,
     * switchToScriptStorage must be called, providing a pointer into the
     * Binding array stored in script->data.
     */
    static bool initWithTemporaryStorage(ExclusiveContext* cx, MutableHandle<Bindings> self,
                                         uint32_t numArgs,
                                         uint32_t numVars,
                                         uint32_t numBodyLevelLexicals,
                                         uint32_t numBlockScoped,
                                         uint32_t numUnaliasedVars,
                                         uint32_t numUnaliasedBodyLevelLexicals,
                                         const Binding* bindingArray,
                                         bool isModule = false);

    // Initialize a trivial Bindings with no slots and an empty callObjShape.
    bool initTrivial(ExclusiveContext* cx);

    // CompileScript parses and compiles one statement at a time, but the result
    // is one Script object.  There will be no vars or bindings, because those
    // go on the global, but there may be block-scoped locals, and the number of
    // block-scoped locals may increase as we parse more expressions.  This
    // helper updates the number of block scoped variables in a script as it is
    // being parsed.
    void updateNumBlockScoped(unsigned numBlockScoped) {
        MOZ_ASSERT(!callObjShape_);
        MOZ_ASSERT(numVars_ == 0);
        MOZ_ASSERT(numBlockScoped < LOCALNO_LIMIT);
        MOZ_ASSERT(numBlockScoped >= numBlockScoped_);
        numBlockScoped_ = numBlockScoped;
    }

    void setAllLocalsAliased() {
        numBlockScoped_ = 0;
    }

    uint8_t* switchToScriptStorage(Binding* newStorage);

    /*
     * Clone srcScript's bindings (as part of js::CloneScript). dstScriptData
     * is the pointer to what will eventually be dstScript->data.
     */
    static bool clone(JSContext* cx, MutableHandle<Bindings> self, uint8_t* dstScriptData,
                      HandleScript srcScript);

    uint32_t numArgs() const { return numArgs_; }
    uint32_t numVars() const { return numVars_; }
    uint32_t numBodyLevelLexicals() const { return numBodyLevelLexicals_; }
    uint32_t numBlockScoped() const { return numBlockScoped_; }
    uint32_t numBodyLevelLocals() const { return numVars_ + numBodyLevelLexicals_; }
    uint32_t numUnaliasedBodyLevelLocals() const { return numUnaliasedVars_ + numUnaliasedBodyLevelLexicals_; }
    uint32_t numAliasedBodyLevelLocals() const { return numBodyLevelLocals() - numUnaliasedBodyLevelLocals(); }
    uint32_t numLocals() const { return numVars() + numBodyLevelLexicals() + numBlockScoped(); }
    uint32_t numFixedLocals() const { return numUnaliasedVars() + numUnaliasedBodyLevelLexicals() + numBlockScoped(); }
    uint32_t lexicalBegin() const { return numArgs() + numVars(); }
    uint32_t aliasedBodyLevelLexicalBegin() const { return aliasedBodyLevelLexicalBegin_; }

    uint32_t numUnaliasedVars() const { return numUnaliasedVars_; }
    uint32_t numUnaliasedBodyLevelLexicals() const { return numUnaliasedBodyLevelLexicals_; }

    // Return the size of the bindingArray.
    uint32_t count() const { return numArgs() + numVars() + numBodyLevelLexicals(); }

    /* Return the initial shape of call objects created for this scope. */
    Shape* callObjShape() const { return callObjShape_; }

    /* Convenience method to get the var index of 'arguments' or 'this'. */
    static BindingIter argumentsBinding(ExclusiveContext* cx, HandleScript script);
    static BindingIter thisBinding(ExclusiveContext* cx, HandleScript script);

    /* Return whether the binding at bindingIndex is aliased. */
    bool bindingIsAliased(uint32_t bindingIndex);

    /* Return whether this scope has any aliased bindings. */
    bool hasAnyAliasedBindings() const {
        if (!callObjShape_)
            return false;

        return !callObjShape_->isEmptyShape();
    }

    Binding* begin() const { return bindingArray(); }
    Binding* end() const { return bindingArray() + count(); }

    static void trace(Bindings* self, JSTracer* trc) { self->trace(trc); }
    void trace(JSTracer* trc);
};

template <class Outer>
class BindingsOperations
{
    const Bindings& bindings() const { return static_cast<const Outer*>(this)->get(); }

  public:
    // Direct data access to the underlying bindings.
    const RelocatablePtrShape& callObjShape() const {
        return bindings().callObjShape_;
    }
    uint16_t numArgs() const {
        return bindings().numArgs_;
    }
    uint16_t numBlockScoped() const {
        return bindings().numBlockScoped_;
    }
    uint16_t numBodyLevelLexicals() const {
        return bindings().numBodyLevelLexicals_;
    }
    uint16_t aliasedBodyLevelLexicalBegin() const {
        return bindings().aliasedBodyLevelLexicalBegin_;
    }
    uint16_t numUnaliasedBodyLevelLexicals() const {
        return bindings().numUnaliasedBodyLevelLexicals_;
    }
    uint32_t numVars() const {
        return bindings().numVars_;
    }
    uint32_t numUnaliasedVars() const {
        return bindings().numUnaliasedVars_;
    }

    // Binding array access.
    bool bindingArrayUsingTemporaryStorage() const {
        return bindings().bindingArrayUsingTemporaryStorage();
    }
    const Binding* bindingArray() const {
        return bindings().bindingArray();
    }
    uint32_t count() const {
        return bindings().count();
    }

    // Helpers.
    uint32_t numBodyLevelLocals() const {
        return numVars() + numBodyLevelLexicals();
    }
    uint32_t numUnaliasedBodyLevelLocals() const {
        return numUnaliasedVars() + numUnaliasedBodyLevelLexicals();
    }
    uint32_t numAliasedBodyLevelLocals() const {
        return numBodyLevelLocals() - numUnaliasedBodyLevelLocals();
    }
    uint32_t numLocals() const {
        return numVars() + numBodyLevelLexicals() + numBlockScoped();
    }
    uint32_t numFixedLocals() const {
        return numUnaliasedVars() + numUnaliasedBodyLevelLexicals() + numBlockScoped();
    }
    uint32_t lexicalBegin() const {
        return numArgs() + numVars();
    }
};

template <class Outer>
class MutableBindingsOperations : public BindingsOperations<Outer>
{
    Bindings& bindings() { return static_cast<Outer*>(this)->get(); }

  public:
    void setCallObjShape(HandleShape shape) { bindings().callObjShape_ = shape; }
    void setBindingArray(const Binding* bindingArray, uintptr_t temporaryBit) {
        bindings().bindingArrayAndFlag_ = uintptr_t(bindingArray) | temporaryBit;
    }
    void setNumArgs(uint16_t num) { bindings().numArgs_ = num; }
    void setNumVars(uint32_t num) { bindings().numVars_ = num; }
    void setNumBodyLevelLexicals(uint16_t num) { bindings().numBodyLevelLexicals_ = num; }
    void setNumBlockScoped(uint16_t num) { bindings().numBlockScoped_ = num; }
    void setNumUnaliasedVars(uint32_t num) { bindings().numUnaliasedVars_ = num; }
    void setNumUnaliasedBodyLevelLexicals(uint16_t num) {
        bindings().numUnaliasedBodyLevelLexicals_ = num;
    }
    void setAliasedBodyLevelLexicalBegin(uint16_t offset) {
        bindings().aliasedBodyLevelLexicalBegin_ = offset;
    }
    uint8_t* switchToScriptStorage(Binding* permanentStorage) {
        return bindings().switchToScriptStorage(permanentStorage);
    }
};

template <>
class HandleBase<Bindings> : public BindingsOperations<JS::Handle<Bindings>>
{};

template <>
class MutableHandleBase<Bindings>
  : public MutableBindingsOperations<JS::MutableHandle<Bindings>>
{};

class ScriptCounts
{
  public:
    typedef mozilla::Vector<PCCounts, 0, SystemAllocPolicy> PCCountsVector;

    inline ScriptCounts();
    inline explicit ScriptCounts(PCCountsVector&& jumpTargets);
    inline ScriptCounts(ScriptCounts&& src);
    inline ~ScriptCounts();

    inline ScriptCounts& operator=(ScriptCounts&& src);

    // Return the counter used to count the number of visits. Returns null if
    // the element is not found.
    PCCounts* maybeGetPCCounts(size_t offset);
    const PCCounts* maybeGetPCCounts(size_t offset) const;

    // PCCounts are stored at jump-target offsets. This function looks for the
    // previous PCCount which is in the same basic block as the current offset.
    PCCounts* getImmediatePrecedingPCCounts(size_t offset);

    // Return the counter used to count the number of throws. Returns null if
    // the element is not found.
    const PCCounts* maybeGetThrowCounts(size_t offset) const;

    // Throw counts are stored at the location of each throwing
    // instruction. This function looks for the previous throw count.
    //
    // Note: if the offset of the returned count is higher than the offset of
    // the immediate preceding PCCount, then this throw happened in the same
    // basic block.
    const PCCounts* getImmediatePrecedingThrowCounts(size_t offset) const;

    // Return the counter used to count the number of throws. Allocate it if
    // none exists yet. Returns null if the allocation failed.
    PCCounts* getThrowCounts(size_t offset);

  private:
    friend class ::JSScript;
    friend struct ScriptAndCounts;

    // This sorted array is used to map an offset to the number of times a
    // branch got visited.
    PCCountsVector pcCounts_;

    // This sorted vector is used to map an offset to the number of times an
    // instruction throw.
    PCCountsVector throwCounts_;

    // Information about any Ion compilations for the script.
    jit::IonScriptCounts* ionCounts_;
};

// Note: The key of this hash map is a weak reference to a JSScript.  We do not
// use the WeakMap implementation provided in jsweakmap.h because it would be
// collected at the beginning of the sweeping of the compartment, thus before
// the calls to the JSScript::finalize function which are used to aggregate code
// coverage results on the compartment.
typedef HashMap<JSScript*,
                ScriptCounts,
                DefaultHasher<JSScript*>,
                SystemAllocPolicy> ScriptCountsMap;

class DebugScript
{
    friend class ::JSScript;

    /*
     * When non-zero, compile script in single-step mode. The top bit is set and
     * cleared by setStepMode, as used by JSD. The lower bits are a count,
     * adjusted by changeStepModeCount, used by the Debugger object. Only
     * when the bit is clear and the count is zero may we compile the script
     * without single-step support.
     */
    uint32_t        stepMode;

    /*
     * Number of breakpoint sites at opcodes in the script. This is the number
     * of populated entries in DebugScript::breakpoints, below.
     */
    uint32_t        numSites;

    /*
     * Breakpoints set in our script. For speed and simplicity, this array is
     * parallel to script->code(): the BreakpointSite for the opcode at
     * script->code()[offset] is debugScript->breakpoints[offset]. Naturally,
     * this array's true length is script->length().
     */
    BreakpointSite* breakpoints[1];
};

typedef HashMap<JSScript*,
                DebugScript*,
                DefaultHasher<JSScript*>,
                SystemAllocPolicy> DebugScriptMap;

class ScriptSource;

class UncompressedSourceCache
{
    typedef HashMap<ScriptSource*,
                    const char16_t*,
                    DefaultHasher<ScriptSource*>,
                    SystemAllocPolicy> Map;

  public:
    // Hold an entry in the source data cache and prevent it from being purged on GC.
    class AutoHoldEntry
    {
        UncompressedSourceCache* cache_;
        ScriptSource* source_;
        const char16_t* charsToFree_;
      public:
        explicit AutoHoldEntry();
        ~AutoHoldEntry();
      private:
        void holdEntry(UncompressedSourceCache* cache, ScriptSource* source);
        void deferDelete(const char16_t* chars);
        ScriptSource* source() const { return source_; }
        friend class UncompressedSourceCache;
    };

  private:
    Map* map_;
    AutoHoldEntry* holder_;

  public:
    UncompressedSourceCache() : map_(nullptr), holder_(nullptr) {}

    const char16_t* lookup(ScriptSource* ss, AutoHoldEntry& asp);
    bool put(ScriptSource* ss, const char16_t* chars, AutoHoldEntry& asp);

    void purge();

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

  private:
    void holdEntry(AutoHoldEntry& holder, ScriptSource* ss);
    void releaseEntry(AutoHoldEntry& holder);
};

class ScriptSource
{
    friend struct SourceCompressionTask;

    uint32_t refs;

    // Note: while ScriptSources may be compressed off thread, they are only
    // modified by the main thread, and all members are always safe to access
    // on the main thread.

    // Indicate which field in the |data| union is active.
    enum {
        DataMissing,
        DataUncompressed,
        DataCompressed,
        DataParent
    } dataType;

    union {
        struct {
            const char16_t* chars;
            bool ownsChars;
        } uncompressed;

        struct {
            void* raw;
            size_t nbytes;
            HashNumber hash;
        } compressed;

        ScriptSource* parent;
    } data;

    uint32_t length_;

    // The filename of this script.
    mozilla::UniquePtr<char[], JS::FreePolicy> filename_;

    mozilla::UniquePtr<char16_t[], JS::FreePolicy> displayURL_;
    mozilla::UniquePtr<char16_t[], JS::FreePolicy> sourceMapURL_;
    bool mutedErrors_;

    // bytecode offset in caller script that generated this code.
    // This is present for eval-ed code, as well as "new Function(...)"-introduced
    // scripts.
    uint32_t introductionOffset_;

    // If this ScriptSource was generated by a code-introduction mechanism such
    // as |eval| or |new Function|, the debugger needs access to the "raw"
    // filename of the top-level script that contains the eval-ing code.  To
    // keep track of this, we must preserve the original outermost filename (of
    // the original introducer script), so that instead of a filename of
    // "foo.js line 30 > eval line 10 > Function", we can obtain the original
    // raw filename of "foo.js".
    //
    // In the case described above, this field will be non-null and will be the
    // original raw filename from above.  Otherwise this field will be null.
    mozilla::UniquePtr<char[], JS::FreePolicy> introducerFilename_;

    // A string indicating how this source code was introduced into the system.
    // This accessor returns one of the following values:
    //      "eval" for code passed to |eval|.
    //      "Function" for code passed to the |Function| constructor.
    //      "Worker" for code loaded by calling the Web worker constructor&mdash;the worker's main script.
    //      "importScripts" for code by calling |importScripts| in a web worker.
    //      "handler" for code assigned to DOM elements' event handler IDL attributes.
    //      "scriptElement" for code belonging to <script> elements.
    //      undefined if the implementation doesn't know how the code was introduced.
    // This is a constant, statically allocated C string, so does not need
    // memory management.
    const char* introductionType_;

    // True if we can call JSRuntime::sourceHook to load the source on
    // demand. If sourceRetrievable_ and hasSourceData() are false, it is not
    // possible to get source at all.
    bool sourceRetrievable_:1;
    bool argumentsNotIncluded_:1;
    bool hasIntroductionOffset_:1;

    // Whether this is in the runtime's set of compressed ScriptSources.
    bool inCompressedSourceSet:1;

  public:
    explicit ScriptSource()
      : refs(0),
        dataType(DataMissing),
        length_(0),
        filename_(nullptr),
        displayURL_(nullptr),
        sourceMapURL_(nullptr),
        mutedErrors_(false),
        introductionOffset_(0),
        introducerFilename_(nullptr),
        introductionType_(nullptr),
        sourceRetrievable_(false),
        argumentsNotIncluded_(false),
        hasIntroductionOffset_(false),
        inCompressedSourceSet(false)
    {
    }
    ~ScriptSource();
    void incref() { refs++; }
    void decref() {
        MOZ_ASSERT(refs != 0);
        if (--refs == 0)
            js_delete(this);
    }
    bool initFromOptions(ExclusiveContext* cx, const ReadOnlyCompileOptions& options);
    bool setSourceCopy(ExclusiveContext* cx,
                       JS::SourceBufferHolder& srcBuf,
                       bool argumentsNotIncluded,
                       SourceCompressionTask* tok);
    void setSourceRetrievable() { sourceRetrievable_ = true; }
    bool sourceRetrievable() const { return sourceRetrievable_; }
    bool hasSourceData() const { return dataType != DataMissing; }
    bool hasCompressedSource() const { return dataType == DataCompressed; }
    size_t length() const {
        MOZ_ASSERT(hasSourceData());
        return length_;
    }
    bool argumentsNotIncluded() const {
        MOZ_ASSERT(hasSourceData());
        return argumentsNotIncluded_;
    }
    const char16_t* chars(JSContext* cx, UncompressedSourceCache::AutoHoldEntry& asp);
    JSFlatString* substring(JSContext* cx, uint32_t start, uint32_t stop);
    JSFlatString* substringDontDeflate(JSContext* cx, uint32_t start, uint32_t stop);
    void addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                JS::ScriptSourceInfo* info) const;

    const char16_t* uncompressedChars() const {
        MOZ_ASSERT(dataType == DataUncompressed);
        return data.uncompressed.chars;
    }

    bool ownsUncompressedChars() const {
        MOZ_ASSERT(dataType == DataUncompressed);
        return data.uncompressed.ownsChars;
    }

    void* compressedData() const {
        MOZ_ASSERT(dataType == DataCompressed);
        return data.compressed.raw;
    }

    size_t compressedBytes() const {
        MOZ_ASSERT(dataType == DataCompressed);
        return data.compressed.nbytes;
    }

    HashNumber compressedHash() const {
        MOZ_ASSERT(dataType == DataCompressed);
        return data.compressed.hash;
    }

    ScriptSource* parent() const {
        MOZ_ASSERT(dataType == DataParent);
        return data.parent;
    }

    void setSource(const char16_t* chars, size_t length, bool ownsChars = true);
    void setCompressedSource(JSRuntime* maybert, void* raw, size_t nbytes, HashNumber hash);
    void updateCompressedSourceSet(JSRuntime* rt);
    bool ensureOwnsSource(ExclusiveContext* cx);

    // XDR handling
    template <XDRMode mode>
    bool performXDR(XDRState<mode>* xdr);

    bool setFilename(ExclusiveContext* cx, const char* filename);
    const char* introducerFilename() const {
        return introducerFilename_ ? introducerFilename_.get() : filename_.get();
    }
    bool hasIntroductionType() const {
        return introductionType_;
    }
    const char* introductionType() const {
        MOZ_ASSERT(hasIntroductionType());
        return introductionType_;
    }
    const char* filename() const {
        return filename_.get();
    }

    // Display URLs
    bool setDisplayURL(ExclusiveContext* cx, const char16_t* displayURL);
    bool hasDisplayURL() const { return displayURL_ != nullptr; }
    const char16_t * displayURL() {
        MOZ_ASSERT(hasDisplayURL());
        return displayURL_.get();
    }

    // Source maps
    bool setSourceMapURL(ExclusiveContext* cx, const char16_t* sourceMapURL);
    bool hasSourceMapURL() const { return sourceMapURL_ != nullptr; }
    const char16_t * sourceMapURL() {
        MOZ_ASSERT(hasSourceMapURL());
        return sourceMapURL_.get();
    }

    bool mutedErrors() const { return mutedErrors_; }

    bool hasIntroductionOffset() const { return hasIntroductionOffset_; }
    uint32_t introductionOffset() const {
        MOZ_ASSERT(hasIntroductionOffset());
        return introductionOffset_;
    }
    void setIntroductionOffset(uint32_t offset) {
        MOZ_ASSERT(!hasIntroductionOffset());
        MOZ_ASSERT(offset <= (uint32_t)INT32_MAX);
        introductionOffset_ = offset;
        hasIntroductionOffset_ = true;
    }

  private:
    size_t computedSizeOfData() const;
};

class ScriptSourceHolder
{
    ScriptSource* ss;
  public:
    explicit ScriptSourceHolder(ScriptSource* ss)
      : ss(ss)
    {
        ss->incref();
    }
    ~ScriptSourceHolder()
    {
        ss->decref();
    }
};

struct CompressedSourceHasher
{
    typedef ScriptSource* Lookup;

    static HashNumber computeHash(const void* data, size_t nbytes) {
        return mozilla::HashBytes(data, nbytes);
    }

    static HashNumber hash(const ScriptSource* ss) {
        return ss->compressedHash();
    }

    static bool match(const ScriptSource* a, const ScriptSource* b) {
        return a->compressedBytes() == b->compressedBytes() &&
               a->compressedHash() == b->compressedHash() &&
               !memcmp(a->compressedData(), b->compressedData(), a->compressedBytes());
    }
};

typedef HashSet<ScriptSource*, CompressedSourceHasher, SystemAllocPolicy> CompressedSourceSet;

class ScriptSourceObject : public NativeObject
{
  public:
    static const Class class_;

    static void trace(JSTracer* trc, JSObject* obj);
    static void finalize(FreeOp* fop, JSObject* obj);
    static ScriptSourceObject* create(ExclusiveContext* cx, ScriptSource* source);

    // Initialize those properties of this ScriptSourceObject whose values
    // are provided by |options|, re-wrapping as necessary.
    static bool initFromOptions(JSContext* cx, HandleScriptSource source,
                                const ReadOnlyCompileOptions& options);

    ScriptSource* source() const {
        return static_cast<ScriptSource*>(getReservedSlot(SOURCE_SLOT).toPrivate());
    }
    JSObject* element() const {
        return getReservedSlot(ELEMENT_SLOT).toObjectOrNull();
    }
    const Value& elementAttributeName() const {
        MOZ_ASSERT(!getReservedSlot(ELEMENT_PROPERTY_SLOT).isMagic());
        return getReservedSlot(ELEMENT_PROPERTY_SLOT);
    }
    JSScript* introductionScript() const {
        if (getReservedSlot(INTRODUCTION_SCRIPT_SLOT).isUndefined())
            return nullptr;
        void* untyped = getReservedSlot(INTRODUCTION_SCRIPT_SLOT).toPrivate();
        MOZ_ASSERT(untyped);
        return static_cast<JSScript*>(untyped);
    }

  private:
    static const uint32_t SOURCE_SLOT = 0;
    static const uint32_t ELEMENT_SLOT = 1;
    static const uint32_t ELEMENT_PROPERTY_SLOT = 2;
    static const uint32_t INTRODUCTION_SCRIPT_SLOT = 3;
    static const uint32_t RESERVED_SLOTS = 4;
};

enum GeneratorKind { NotGenerator, LegacyGenerator, StarGenerator };

static inline unsigned
GeneratorKindAsBits(GeneratorKind generatorKind) {
    return static_cast<unsigned>(generatorKind);
}

static inline GeneratorKind
GeneratorKindFromBits(unsigned val) {
    MOZ_ASSERT(val <= StarGenerator);
    return static_cast<GeneratorKind>(val);
}

/*
 * NB: after a successful XDR_DECODE, XDRScript callers must do any required
 * subsequent set-up of owning function or script object and then call
 * CallNewScriptHook.
 */
template<XDRMode mode>
bool
XDRScript(XDRState<mode>* xdr, HandleObject enclosingScope, HandleScript enclosingScript,
          HandleFunction fun, MutableHandleScript scriptp);

template<XDRMode mode>
bool
XDRLazyScript(XDRState<mode>* xdr, HandleObject enclosingScope, HandleScript enclosingScript,
              HandleFunction fun, MutableHandle<LazyScript*> lazy);

/*
 * Code any constant value.
 */
template<XDRMode mode>
bool
XDRScriptConst(XDRState<mode>* xdr, MutableHandleValue vp);

} /* namespace js */

class JSScript : public js::gc::TenuredCell
{
    template <js::XDRMode mode>
    friend
    bool
    js::XDRScript(js::XDRState<mode>* xdr, js::HandleObject enclosingScope,
                  js::HandleScript enclosingScript,
                  js::HandleFunction fun, js::MutableHandleScript scriptp);

    friend bool
    js::detail::CopyScript(JSContext* cx, js::HandleObject scriptStaticScope, js::HandleScript src,
                           js::HandleScript dst);

  public:
    //
    // We order fields according to their size in order to avoid wasting space
    // for alignment.
    //

    // Larger-than-word-sized fields.

  public:
    js::Bindings    bindings;   /* names of top-level variables in this script
                                   (and arguments if this is a function script) */

    bool hasAnyAliasedBindings() const {
        return bindings.hasAnyAliasedBindings();
    }

    js::Binding* bindingArray() const {
        return bindings.bindingArray();
    }

    unsigned numArgs() const {
        return bindings.numArgs();
    }

    js::Shape* callObjShape() const {
        return bindings.callObjShape();
    }

    // Word-sized fields.

  private:
    jsbytecode*     code_;     /* bytecodes and their immediate operands */
  public:
    uint8_t*        data;      /* pointer to variable-length data array (see
                                   comment above Create() for details) */

    js::HeapPtrAtom* atoms;     /* maps immediate index to literal struct */

    JSCompartment*  compartment_;

  private:
    /* Persistent type information retained across GCs. */
    js::TypeScript* types_;

    // This script's ScriptSourceObject, or a CCW thereof.
    //
    // (When we clone a JSScript into a new compartment, we don't clone its
    // source object. Instead, the clone refers to a wrapper.)
    js::HeapPtrObject sourceObject_;

    js::HeapPtrFunction function_;
    js::HeapPtr<js::ModuleObject*> module_;
    js::HeapPtrObject   enclosingStaticScope_;

    /*
     * Information attached by Ion. Nexto a valid IonScript this could be
     * ION_DISABLED_SCRIPT, ION_COMPILING_SCRIPT or ION_PENDING_SCRIPT.
     * The later is a ion compilation that is ready, but hasn't been linked
     * yet.
     */
    js::jit::IonScript* ion;

    /* Information attached by Baseline. */
    js::jit::BaselineScript* baseline;

    /* Information used to re-lazify a lazily-parsed interpreted function. */
    js::LazyScript* lazyScript;

    /*
     * Pointer to either baseline->method()->raw() or ion->method()->raw(), or
     * nullptr if there's no Baseline or Ion script.
     */
    uint8_t* baselineOrIonRaw;
    uint8_t* baselineOrIonSkipArgCheck;

    // 32-bit fields.

    uint32_t        length_;    /* length of code vector */
    uint32_t        dataSize_;  /* size of the used part of the data array */

    uint32_t        lineno_;    /* base line number of script */
    uint32_t        column_;    /* base column of script, optionally set */

    uint32_t        mainOffset_;/* offset of main entry point from code, after
                                   predef'ing prologue */

    uint32_t        natoms_;    /* length of atoms array */
    uint32_t        nslots_;    /* vars plus maximum stack depth */

    /* Range of characters in scriptSource which contains this script's source. */
    uint32_t        sourceStart_;
    uint32_t        sourceEnd_;

    uint32_t        warmUpCount; /* Number of times the script has been called
                                  * or has had backedges taken. When running in
                                  * ion, also increased for any inlined scripts.
                                  * Reset if the script's JIT code is forcibly
                                  * discarded. */

    // 16-bit fields.

    uint16_t        warmUpResetCount; /* Number of times the |warmUpCount| was
                                 * forcibly discarded. The counter is reset when
                                 * a script is successfully jit-compiled. */

    uint16_t        version;    /* JS version under which script was compiled */

    uint16_t        funLength_; /* ES6 function length */

    uint16_t        nTypeSets_; /* number of type sets used in this script for
                                   dynamic type monitoring */

    // Bit fields.

  public:
    // The kinds of the optional arrays.
    enum ArrayKind {
        CONSTS,
        OBJECTS,
        REGEXPS,
        TRYNOTES,
        BLOCK_SCOPES,
        ARRAY_KIND_BITS
    };

  private:
    // The bits in this field indicate the presence/non-presence of several
    // optional arrays in |data|.  See the comments above Create() for details.
    uint8_t         hasArrayBits:ARRAY_KIND_BITS;

    // The GeneratorKind of the script.
    uint8_t         generatorKindBits_:2;

    // 1-bit fields.

    // No need for result value of last expression statement.
    bool noScriptRval_:1;

    // Can call getCallerFunction().
    bool savedCallerFun_:1;

    // Code is in strict mode.
    bool strict_:1;

    // Code has "use strict"; explicitly.
    bool explicitUseStrict_:1;

    // True if the script has a non-syntactic scope on its dynamic scope chain.
    // That is, there are objects about which we know nothing between the
    // outermost syntactic scope and the global.
    bool hasNonSyntacticScope_:1;

    // see Parser::selfHostingMode.
    bool selfHosted_:1;

    // See FunctionContextFlags.
    bool bindingsAccessedDynamically_:1;
    bool funHasExtensibleScope_:1;
    bool funNeedsDeclEnvObject_:1;

    // True if any formalIsAliased(i).
    bool funHasAnyAliasedFormal_:1;

    // Have warned about uses of undefined properties in this script.
    bool warnedAboutUndefinedProp_:1;

    // Script has singleton objects.
    bool hasSingletons_:1;

    // Script is a lambda to treat as running once or a global or eval script
    // that will only run once.  Which one it is can be disambiguated by
    // checking whether function_ is null.
    bool treatAsRunOnce_:1;

    // If treatAsRunOnce, whether script has executed.
    bool hasRunOnce_:1;

    // Script has been reused for a clone.
    bool hasBeenCloned_:1;

    // Script came from eval(), and is still active.
    bool isActiveEval_:1;

    // Script came from eval(), and is in eval cache.
    bool isCachedEval_:1;

    // 'this', 'arguments' and f.apply() are used. This is likely to be a wrapper.
    bool usesArgumentsApplyAndThis_:1;

    // IonMonkey compilation hints.
    bool failedBoundsCheck_:1; /* script has had hoisted bounds checks fail */
    bool failedShapeGuard_:1; /* script has had hoisted shape guard fail */
    bool hadFrequentBailouts_:1;
    bool uninlineable_:1;    /* explicitly marked as uninlineable */

    // Idempotent cache has triggered invalidation.
    bool invalidatedIdempotentCache_:1;

    // Lexical check did fail and bail out.
    bool failedLexicalCheck_:1;

    // If the generator was created implicitly via a generator expression,
    // isGeneratorExp will be true.
    bool isGeneratorExp_:1;

    // Script has an entry in JSCompartment::scriptCountsMap.
    bool hasScriptCounts_:1;

    // Script has an entry in JSCompartment::debugScriptMap.
    bool hasDebugScript_:1;

    // Freeze constraints for stack type sets have been generated.
    bool hasFreezeConstraints_:1;

    /* See comments below. */
    bool argsHasVarBinding_:1;
    bool needsArgsAnalysis_:1;
    bool needsArgsObj_:1;
    bool functionHasThisBinding_:1;

    // Whether the arguments object for this script, if it needs one, should be
    // mapped (alias formal parameters).
    bool hasMappedArgsObj_:1;

    // Generation for this script's TypeScript. If out of sync with the
    // TypeZone's generation, the TypeScript needs to be swept.
    //
    // This should be a uint32 but is instead a bool so that MSVC packs it
    // correctly.
    bool typesGeneration_:1;

    // Do not relazify this script. This is used by the relazify() testing
    // function for scripts that are on the stack and also by the AutoDelazify
    // RAII class. Usually we don't relazify functions in compartments with
    // scripts on the stack, but the relazify() testing function overrides that,
    // and sometimes we're working with a cross-compartment function and need to
    // keep it from relazifying.
    bool doNotRelazify_:1;

    // Script contains inner functions. Used to check if we can relazify the
    // script.
    bool hasInnerFunctions_:1;

    bool needsHomeObject_:1;

    bool isDerivedClassConstructor_:1;

    // Add padding so JSScript is gc::Cell aligned. Make padding protected
    // instead of private to suppress -Wunused-private-field compiler warnings.
  protected:
#if JS_BITS_PER_WORD == 32
    // No padding currently required.
#endif

    //
    // End of fields.  Start methods.
    //

  public:
    static JSScript* Create(js::ExclusiveContext* cx,
                            js::HandleObject enclosingScope, bool savedCallerFun,
                            const JS::ReadOnlyCompileOptions& options,
                            js::HandleObject sourceObject, uint32_t sourceStart,
                            uint32_t sourceEnd);

    void initCompartment(js::ExclusiveContext* cx);

    // Three ways ways to initialize a JSScript. Callers of partiallyInit()
    // and fullyInitTrivial() are responsible for notifying the debugger after
    // successfully creating any kind (function or other) of new JSScript.
    // However, callers of fullyInitFromEmitter() do not need to do this.
    static bool partiallyInit(js::ExclusiveContext* cx, JS::Handle<JSScript*> script,
                              uint32_t nconsts, uint32_t nobjects, uint32_t nregexps,
                              uint32_t ntrynotes, uint32_t nblockscopes, uint32_t nyieldoffsets,
                              uint32_t nTypeSets);
    static bool fullyInitFromEmitter(js::ExclusiveContext* cx, JS::Handle<JSScript*> script,
                                     js::frontend::BytecodeEmitter* bce);
    static void linkToFunctionFromEmitter(js::ExclusiveContext* cx, JS::Handle<JSScript*> script,
                                          js::frontend::FunctionBox* funbox);
    static void linkToModuleFromEmitter(js::ExclusiveContext* cx, JS::Handle<JSScript*> script,
                                        js::frontend::ModuleBox* funbox);
    // Initialize a no-op script.
    static bool fullyInitTrivial(js::ExclusiveContext* cx, JS::Handle<JSScript*> script);

    inline JSPrincipals* principals();

    JSCompartment* compartment() const { return compartment_; }
    JSCompartment* maybeCompartment() const { return compartment(); }

    void setVersion(JSVersion v) { version = v; }

    // Script bytecode is immutable after creation.
    jsbytecode* code() const {
        return code_;
    }
    size_t length() const {
        return length_;
    }

    void setCode(jsbytecode* code) { code_ = code; }
    void setLength(size_t length) { length_ = length; }

    jsbytecode* codeEnd() const { return code() + length(); }

    jsbytecode* lastPC() const {
        jsbytecode* pc = codeEnd() - js::JSOP_RETRVAL_LENGTH;
        MOZ_ASSERT(*pc == JSOP_RETRVAL);
        return pc;
    }

    bool containsPC(const jsbytecode* pc) const {
        return pc >= code() && pc < codeEnd();
    }

    size_t pcToOffset(const jsbytecode* pc) const {
        MOZ_ASSERT(containsPC(pc));
        return size_t(pc - code());
    }

    jsbytecode* offsetToPC(size_t offset) const {
        MOZ_ASSERT(offset < length());
        return code() + offset;
    }

    size_t mainOffset() const {
        return mainOffset_;
    }

    size_t lineno() const {
        return lineno_;
    }

    size_t column() const {
        return column_;
    }

    void setColumn(size_t column) { column_ = column; }

    // The fixed part of a stack frame is comprised of vars (in function code)
    // and block-scoped locals (in all kinds of code).
    size_t nfixed() const {
        return function_ ? bindings.numFixedLocals() : bindings.numBlockScoped();
    }

    // Number of fixed slots reserved for vars.  Only nonzero for function
    // code.
    size_t nfixedvars() const {
        return function_ ? bindings.numUnaliasedVars() : 0;
    }

    // Number of fixed slots reserved for body-level lexicals and vars. This
    // value minus nfixedvars() is the number of body-level lexicals. Only
    // nonzero for function code.
    size_t nbodyfixed() const {
        return function_ ? bindings.numUnaliasedBodyLevelLocals() : 0;
    }

    // Calculate the number of fixed slots that are live at a particular bytecode.
    size_t calculateLiveFixed(jsbytecode* pc);

    // Aliases for clarity when dealing with lexical slots.
    size_t fixedLexicalBegin() const {
        return nfixedvars();
    }

    size_t fixedLexicalEnd() const {
        return nfixed();
    }

    size_t nslots() const {
        return nslots_;
    }

    size_t nTypeSets() const {
        return nTypeSets_;
    }

    size_t funLength() const {
        return funLength_;
    }

    size_t sourceStart() const {
        return sourceStart_;
    }

    size_t sourceEnd() const {
        return sourceEnd_;
    }

    bool noScriptRval() const {
        return noScriptRval_;
    }

    bool savedCallerFun() const { return savedCallerFun_; }

    bool strict() const {
        return strict_;
    }

    bool explicitUseStrict() const { return explicitUseStrict_; }

    bool hasNonSyntacticScope() const {
        return hasNonSyntacticScope_;
    }

    bool selfHosted() const { return selfHosted_; }
    bool bindingsAccessedDynamically() const { return bindingsAccessedDynamically_; }
    bool funHasExtensibleScope() const {
        return funHasExtensibleScope_;
    }
    bool funNeedsDeclEnvObject() const {
        return funNeedsDeclEnvObject_;
    }
    bool funHasAnyAliasedFormal() const {
        return funHasAnyAliasedFormal_;
    }

    bool hasSingletons() const { return hasSingletons_; }
    bool treatAsRunOnce() const {
        return treatAsRunOnce_;
    }
    bool hasRunOnce() const { return hasRunOnce_; }
    bool hasBeenCloned() const { return hasBeenCloned_; }

    void setTreatAsRunOnce() { treatAsRunOnce_ = true; }
    void setHasRunOnce() { hasRunOnce_ = true; }
    void setHasBeenCloned() { hasBeenCloned_ = true; }

    bool isActiveEval() const { return isActiveEval_; }
    bool isCachedEval() const { return isCachedEval_; }

    void cacheForEval() {
        MOZ_ASSERT(isActiveEval() && !isCachedEval());
        isActiveEval_ = false;
        isCachedEval_ = true;
        // IsEvalCacheCandidate will make sure that there's nothing in this
        // script that would prevent reexecution even if isRunOnce is
        // true.  So just pretend like we never ran this script.
        hasRunOnce_ = false;
    }

    void uncacheForEval() {
        MOZ_ASSERT(isCachedEval() && !isActiveEval());
        isCachedEval_ = false;
        isActiveEval_ = true;
    }

    void setActiveEval() { isActiveEval_ = true; }

    bool usesArgumentsApplyAndThis() const {
        return usesArgumentsApplyAndThis_;
    }
    void setUsesArgumentsApplyAndThis() { usesArgumentsApplyAndThis_ = true; }

    bool isGeneratorExp() const { return isGeneratorExp_; }

    bool failedBoundsCheck() const {
        return failedBoundsCheck_;
    }
    bool failedShapeGuard() const {
        return failedShapeGuard_;
    }
    bool hadFrequentBailouts() const {
        return hadFrequentBailouts_;
    }
    bool uninlineable() const {
        return uninlineable_;
    }
    bool invalidatedIdempotentCache() const {
        return invalidatedIdempotentCache_;
    }
    bool failedLexicalCheck() const {
        return failedLexicalCheck_;
    }

    void setFailedBoundsCheck() { failedBoundsCheck_ = true; }
    void setFailedShapeGuard() { failedShapeGuard_ = true; }
    void setHadFrequentBailouts() { hadFrequentBailouts_ = true; }
    void setUninlineable() { uninlineable_ = true; }
    void setInvalidatedIdempotentCache() { invalidatedIdempotentCache_ = true; }
    void setFailedLexicalCheck() { failedLexicalCheck_ = true; }

    bool hasScriptCounts() const { return hasScriptCounts_; }

    bool hasFreezeConstraints() const { return hasFreezeConstraints_; }
    void setHasFreezeConstraints() { hasFreezeConstraints_ = true; }

    bool warnedAboutUndefinedProp() const { return warnedAboutUndefinedProp_; }
    void setWarnedAboutUndefinedProp() { warnedAboutUndefinedProp_ = true; }

    /* See ContextFlags::funArgumentsHasLocalBinding comment. */
    bool argumentsHasVarBinding() const {
        return argsHasVarBinding_;
    }
    jsbytecode* argumentsBytecode() const { MOZ_ASSERT(code()[0] == JSOP_ARGUMENTS); return code(); }
    void setArgumentsHasVarBinding();
    bool argumentsAliasesFormals() const {
        return argumentsHasVarBinding() && hasMappedArgsObj();
    }

    js::GeneratorKind generatorKind() const {
        return js::GeneratorKindFromBits(generatorKindBits_);
    }
    bool isGenerator() const { return generatorKind() != js::NotGenerator; }
    bool isLegacyGenerator() const { return generatorKind() == js::LegacyGenerator; }
    bool isStarGenerator() const { return generatorKind() == js::StarGenerator; }
    void setGeneratorKind(js::GeneratorKind kind) {
        // A script only gets its generator kind set as part of initialization,
        // so it can only transition from not being a generator.
        MOZ_ASSERT(!isGenerator());
        generatorKindBits_ = GeneratorKindAsBits(kind);
    }

    void setNeedsHomeObject() {
        needsHomeObject_ = true;
    }
    bool needsHomeObject() const {
        return needsHomeObject_;
    }

    bool isDerivedClassConstructor() const {
        return isDerivedClassConstructor_;
    }

    /*
     * As an optimization, even when argsHasLocalBinding, the function prologue
     * may not need to create an arguments object. This is determined by
     * needsArgsObj which is set by AnalyzeArgumentsUsage. When !needsArgsObj,
     * the prologue may simply write MagicValue(JS_OPTIMIZED_ARGUMENTS) to
     * 'arguments's slot and any uses of 'arguments' will be guaranteed to
     * handle this magic value. To avoid spurious arguments object creation, we
     * maintain the invariant that needsArgsObj is only called after the script
     * has been analyzed.
     */
    bool analyzedArgsUsage() const { return !needsArgsAnalysis_; }
    inline bool ensureHasAnalyzedArgsUsage(JSContext* cx);
    bool needsArgsObj() const {
        MOZ_ASSERT(analyzedArgsUsage());
        return needsArgsObj_;
    }
    void setNeedsArgsObj(bool needsArgsObj);
    static bool argumentsOptimizationFailed(JSContext* cx, js::HandleScript script);

    bool hasMappedArgsObj() const {
        return hasMappedArgsObj_;
    }

    bool functionHasThisBinding() const {
        return functionHasThisBinding_;
    }

    /*
     * Arguments access (via JSOP_*ARG* opcodes) must access the canonical
     * location for the argument. If an arguments object exists AND it's mapped
     * ('arguments' aliases formals), then all access must go through the
     * arguments object. Otherwise, the local slot is the canonical location for
     * the arguments. Note: if a formal is aliased through the scope chain, then
     * script->formalIsAliased and JSOP_*ARG* opcodes won't be emitted at all.
     */
    bool argsObjAliasesFormals() const {
        return needsArgsObj() && hasMappedArgsObj();
    }

    uint32_t typesGeneration() const {
        return (uint32_t) typesGeneration_;
    }

    void setTypesGeneration(uint32_t generation) {
        MOZ_ASSERT(generation <= 1);
        typesGeneration_ = (bool) generation;
    }

    void setDoNotRelazify(bool b) {
        doNotRelazify_ = b;
    }

    void setHasInnerFunctions(bool b) {
        hasInnerFunctions_ = b;
    }

    bool hasInnerFunctions() const {
        return hasInnerFunctions_;
    }

    bool hasAnyIonScript() const {
        return hasIonScript();
    }

    bool hasIonScript() const {
        bool res = ion && ion != ION_DISABLED_SCRIPT && ion != ION_COMPILING_SCRIPT &&
                          ion != ION_PENDING_SCRIPT;
        MOZ_ASSERT_IF(res, baseline);
        return res;
    }
    bool canIonCompile() const {
        return ion != ION_DISABLED_SCRIPT;
    }
    bool isIonCompilingOffThread() const {
        return ion == ION_COMPILING_SCRIPT;
    }

    js::jit::IonScript* ionScript() const {
        MOZ_ASSERT(hasIonScript());
        return ion;
    }
    js::jit::IonScript* maybeIonScript() const {
        return ion;
    }
    js::jit::IonScript* const* addressOfIonScript() const {
        return &ion;
    }
    void setIonScript(JSContext* maybecx, js::jit::IonScript* ionScript);

    bool hasBaselineScript() const {
        bool res = baseline && baseline != BASELINE_DISABLED_SCRIPT;
        MOZ_ASSERT_IF(!res, !ion || ion == ION_DISABLED_SCRIPT);
        return res;
    }
    bool canBaselineCompile() const {
        return baseline != BASELINE_DISABLED_SCRIPT;
    }
    js::jit::BaselineScript* baselineScript() const {
        MOZ_ASSERT(hasBaselineScript());
        return baseline;
    }
    inline void setBaselineScript(JSContext* maybecx, js::jit::BaselineScript* baselineScript);

    void updateBaselineOrIonRaw(JSContext* maybecx);

    static size_t offsetOfBaselineScript() {
        return offsetof(JSScript, baseline);
    }
    static size_t offsetOfIonScript() {
        return offsetof(JSScript, ion);
    }
    static size_t offsetOfBaselineOrIonRaw() {
        return offsetof(JSScript, baselineOrIonRaw);
    }
    uint8_t* baselineOrIonRawPointer() const {
        return baselineOrIonRaw;
    }
    static size_t offsetOfBaselineOrIonSkipArgCheck() {
        return offsetof(JSScript, baselineOrIonSkipArgCheck);
    }

    bool isRelazifiable() const {
        return (selfHosted() || lazyScript) && !hasInnerFunctions_ && !types_ &&
               !isGenerator() && !hasBaselineScript() && !hasAnyIonScript() &&
               !hasScriptCounts() && !doNotRelazify_;
    }
    void setLazyScript(js::LazyScript* lazy) {
        lazyScript = lazy;
    }
    js::LazyScript* maybeLazyScript() {
        return lazyScript;
    }

    /*
     * Original compiled function for the script, if it has a function.
     * nullptr for global and eval scripts.
     * The delazifying variant ensures that the function isn't lazy. The
     * non-delazifying variant must only be used after earlier code has
     * called ensureNonLazyCanonicalFunction and while the function can't
     * have been relazified.
     */
    inline JSFunction* functionDelazifying() const;
    JSFunction* functionNonDelazifying() const {
        return function_;
    }
    inline void setFunction(JSFunction* fun);
    /*
     * De-lazifies the canonical function. Must be called before entering code
     * that expects the function to be non-lazy.
     */
    inline void ensureNonLazyCanonicalFunction(JSContext* cx);

    js::ModuleObject* module() const {
        return module_;
    }
    inline void setModule(js::ModuleObject* module);

    // Returns true if the script may read formal arguments on the stack
    // directly, via lazy arguments or a rest parameter.
    bool mayReadFrameArgsDirectly();

    JSFlatString* sourceData(JSContext* cx);

    static bool loadSource(JSContext* cx, js::ScriptSource* ss, bool* worked);

    void setSourceObject(JSObject* object);
    JSObject* sourceObject() const {
        return sourceObject_;
    }
    js::ScriptSourceObject& scriptSourceUnwrap() const;
    js::ScriptSource* scriptSource() const;
    js::ScriptSource* maybeForwardedScriptSource() const;
    bool mutedErrors() const { return scriptSource()->mutedErrors(); }
    const char* filename() const { return scriptSource()->filename(); }
    const char* maybeForwardedFilename() const { return maybeForwardedScriptSource()->filename(); }

  public:

    /* Return whether this script was compiled for 'eval' */
    bool isForEval() { return isCachedEval() || isActiveEval(); }

    /*
     * Return whether this script is a top-level script.
     *
     * If we evaluate some code which contains a syntax error, then we might
     * produce a JSScript which has no associated bytecode. Testing with
     * |code()| filters out this kind of scripts.
     *
     * If this script has a function associated to it, then it is not the
     * top-level of a file.
     */
    bool isTopLevel() { return code() && !functionNonDelazifying(); }

    /* Ensure the script has a TypeScript. */
    inline bool ensureHasTypes(JSContext* cx);

    inline js::TypeScript* types();

    void maybeSweepTypes(js::AutoClearTypeInferenceStateOnOOM* oom);

    inline js::GlobalObject& global() const;
    js::GlobalObject& uninlinedGlobal() const;

    /* See StaticScopeIter comment. */
    JSObject* enclosingStaticScope() const {
        return enclosingStaticScope_;
    }

    // Switch the script over from the off-thread compartment's static
    // global lexical scope to the main thread compartment's.
    void fixEnclosingStaticGlobalLexicalScope();

  private:
    bool makeTypes(JSContext* cx);

  public:
    uint32_t getWarmUpCount() const { return warmUpCount; }
    uint32_t incWarmUpCounter(uint32_t amount = 1) { return warmUpCount += amount; }
    uint32_t* addressOfWarmUpCounter() { return &warmUpCount; }
    static size_t offsetOfWarmUpCounter() { return offsetof(JSScript, warmUpCount); }
    void resetWarmUpCounter() { incWarmUpResetCounter(); warmUpCount = 0; }

    uint16_t getWarmUpResetCount() const { return warmUpResetCount; }
    uint16_t incWarmUpResetCounter(uint16_t amount = 1) { return warmUpResetCount += amount; }
    void resetWarmUpResetCounter() { warmUpResetCount = 0; }

  public:
    bool initScriptCounts(JSContext* cx);
    js::ScriptCounts& getScriptCounts();
    js::PCCounts* maybeGetPCCounts(jsbytecode* pc);
    const js::PCCounts* maybeGetThrowCounts(jsbytecode* pc);
    js::PCCounts* getThrowCounts(jsbytecode* pc);
    uint64_t getHitCount(jsbytecode* pc);
    void incHitCount(jsbytecode* pc); // Used when we bailout out of Ion.
    void addIonCounts(js::jit::IonScriptCounts* ionCounts);
    js::jit::IonScriptCounts* getIonCounts();
    void releaseScriptCounts(js::ScriptCounts* counts);
    void destroyScriptCounts(js::FreeOp* fop);
    // The entry should be removed after using this function.
    void takeOverScriptCountsMapEntry(js::ScriptCounts* entryValue);

    jsbytecode* main() {
        return code() + mainOffset();
    }

    /*
     * computedSizeOfData() is the in-use size of all the data sections.
     * sizeOfData() is the size of the block allocated to hold all the data
     * sections (which can be larger than the in-use size).
     */
    size_t computedSizeOfData() const;
    size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf) const;
    size_t sizeOfTypeScript(mozilla::MallocSizeOf mallocSizeOf) const;

    uint32_t numNotes();  /* Number of srcnote slots in the srcnotes section */

    /* Script notes are allocated right after the code. */
    jssrcnote* notes() { return (jssrcnote*)(code() + length()); }

    bool hasArray(ArrayKind kind) {
        return hasArrayBits & (1 << kind);
    }
    void setHasArray(ArrayKind kind) { hasArrayBits |= (1 << kind); }
    void cloneHasArray(JSScript* script) { hasArrayBits = script->hasArrayBits; }

    bool hasConsts()        { return hasArray(CONSTS);      }
    bool hasObjects()       { return hasArray(OBJECTS);     }
    bool hasRegexps()       { return hasArray(REGEXPS);     }
    bool hasTrynotes()      { return hasArray(TRYNOTES);    }
    bool hasBlockScopes()   { return hasArray(BLOCK_SCOPES); }
    bool hasYieldOffsets()  { return isGenerator(); }

    #define OFF(fooOff, hasFoo, t)   (fooOff() + (hasFoo() ? sizeof(t) : 0))

    size_t constsOffset()       { return 0; }
    size_t objectsOffset()      { return OFF(constsOffset,      hasConsts,      js::ConstArray);      }
    size_t regexpsOffset()      { return OFF(objectsOffset,     hasObjects,     js::ObjectArray);     }
    size_t trynotesOffset()     { return OFF(regexpsOffset,     hasRegexps,     js::ObjectArray);     }
    size_t blockScopesOffset()  { return OFF(trynotesOffset,    hasTrynotes,    js::TryNoteArray);    }
    size_t yieldOffsetsOffset() { return OFF(blockScopesOffset, hasBlockScopes, js::BlockScopeArray); }

    size_t dataSize() const { return dataSize_; }

    js::ConstArray* consts() {
        MOZ_ASSERT(hasConsts());
        return reinterpret_cast<js::ConstArray*>(data + constsOffset());
    }

    js::ObjectArray* objects() {
        MOZ_ASSERT(hasObjects());
        return reinterpret_cast<js::ObjectArray*>(data + objectsOffset());
    }

    js::ObjectArray* regexps() {
        MOZ_ASSERT(hasRegexps());
        return reinterpret_cast<js::ObjectArray*>(data + regexpsOffset());
    }

    js::TryNoteArray* trynotes() {
        MOZ_ASSERT(hasTrynotes());
        return reinterpret_cast<js::TryNoteArray*>(data + trynotesOffset());
    }

    js::BlockScopeArray* blockScopes() {
        MOZ_ASSERT(hasBlockScopes());
        return reinterpret_cast<js::BlockScopeArray*>(data + blockScopesOffset());
    }

    js::YieldOffsetArray& yieldOffsets() {
        MOZ_ASSERT(hasYieldOffsets());
        return *reinterpret_cast<js::YieldOffsetArray*>(data + yieldOffsetsOffset());
    }

    bool hasLoops();

    size_t natoms() const { return natoms_; }

    js::HeapPtrAtom& getAtom(size_t index) const {
        MOZ_ASSERT(index < natoms());
        return atoms[index];
    }

    js::HeapPtrAtom& getAtom(jsbytecode* pc) const {
        MOZ_ASSERT(containsPC(pc) && containsPC(pc + sizeof(uint32_t)));
        return getAtom(GET_UINT32_INDEX(pc));
    }

    js::PropertyName* getName(size_t index) {
        return getAtom(index)->asPropertyName();
    }

    js::PropertyName* getName(jsbytecode* pc) const {
        MOZ_ASSERT(containsPC(pc) && containsPC(pc + sizeof(uint32_t)));
        return getAtom(GET_UINT32_INDEX(pc))->asPropertyName();
    }

    JSObject* getObject(size_t index) {
        js::ObjectArray* arr = objects();
        MOZ_ASSERT(index < arr->length);
        MOZ_ASSERT(arr->vector[index]->isTenured());
        return arr->vector[index];
    }

    size_t innerObjectsStart() {
        // The first object contains the caller if savedCallerFun is used.
        return savedCallerFun() ? 1 : 0;
    }

    JSObject* getObject(jsbytecode* pc) {
        MOZ_ASSERT(containsPC(pc) && containsPC(pc + sizeof(uint32_t)));
        return getObject(GET_UINT32_INDEX(pc));
    }

    JSVersion getVersion() const {
        return JSVersion(version);
    }

    inline JSFunction* getFunction(size_t index);
    inline JSFunction* getCallerFunction();
    inline JSFunction* functionOrCallerFunction();

    inline js::RegExpObject* getRegExp(size_t index);
    inline js::RegExpObject* getRegExp(jsbytecode* pc);

    const js::Value& getConst(size_t index) {
        js::ConstArray* arr = consts();
        MOZ_ASSERT(index < arr->length);
        return arr->vector[index];
    }

    // The following 4 functions find the static scope just before the
    // execution of the instruction pointed to by pc.

    js::NestedScopeObject* getStaticBlockScope(jsbytecode* pc);

    // Returns the innermost static scope at pc if it falls within the extent
    // of the script. Returns nullptr otherwise.
    JSObject* innermostStaticScopeInScript(jsbytecode* pc);

    // As innermostStaticScopeInScript, but returns the enclosing static scope
    // if the innermost static scope falls without the extent of the script.
    JSObject* innermostStaticScope(jsbytecode* pc);

    JSObject* innermostStaticScope() { return innermostStaticScope(main()); }

    /*
     * The isEmpty method tells whether this script has code that computes any
     * result (not return value, result AKA normal completion value) other than
     * JSVAL_VOID, or any other effects.
     */
    bool isEmpty() const {
        if (length() > 3)
            return false;

        jsbytecode* pc = code();
        if (noScriptRval() && JSOp(*pc) == JSOP_FALSE)
            ++pc;
        return JSOp(*pc) == JSOP_RETRVAL;
    }

    bool bindingIsAliased(const js::BindingIter& bi);
    bool formalIsAliased(unsigned argSlot);
    bool formalLivesInArgumentsObject(unsigned argSlot);
    bool localIsAliased(unsigned localSlot);

  private:
    /* Change this->stepMode to |newValue|. */
    void setNewStepMode(js::FreeOp* fop, uint32_t newValue);

    bool ensureHasDebugScript(JSContext* cx);
    js::DebugScript* debugScript();
    js::DebugScript* releaseDebugScript();
    void destroyDebugScript(js::FreeOp* fop);

  public:
    bool hasBreakpointsAt(jsbytecode* pc);
    bool hasAnyBreakpointsOrStepMode() { return hasDebugScript_; }

    // See comment above 'debugMode' in jscompartment.h for explanation of
    // invariants of debuggee compartments, scripts, and frames.
    inline bool isDebuggee() const;

    js::BreakpointSite* getBreakpointSite(jsbytecode* pc)
    {
        return hasDebugScript_ ? debugScript()->breakpoints[pcToOffset(pc)] : nullptr;
    }

    js::BreakpointSite* getOrCreateBreakpointSite(JSContext* cx, jsbytecode* pc);

    void destroyBreakpointSite(js::FreeOp* fop, jsbytecode* pc);

    void clearBreakpointsIn(js::FreeOp* fop, js::Debugger* dbg, JSObject* handler);

    /*
     * Increment or decrement the single-step count. If the count is non-zero
     * then the script is in single-step mode.
     *
     * Only incrementing is fallible, as it could allocate a DebugScript.
     */
    bool incrementStepModeCount(JSContext* cx);
    void decrementStepModeCount(js::FreeOp* fop);

    bool stepModeEnabled() { return hasDebugScript_ && !!debugScript()->stepMode; }

#ifdef DEBUG
    uint32_t stepModeCount() { return hasDebugScript_ ? debugScript()->stepMode : 0; }
#endif

    void finalize(js::FreeOp* fop);
    void fixupAfterMovingGC() {}

    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_SCRIPT; }

    void traceChildren(JSTracer* trc);

    // A helper class to prevent relazification of the given function's script
    // while it's holding on to it.  This class automatically roots the script.
    class AutoDelazify;
    friend class AutoDelazify;

    class AutoDelazify
    {
        JS::RootedScript script_;
        JSContext* cx_;
        bool oldDoNotRelazify_;
      public:
        explicit AutoDelazify(JSContext* cx, JS::HandleFunction fun = nullptr)
            : script_(cx)
            , cx_(cx)
        {
            holdScript(fun);
        }

        ~AutoDelazify()
        {
            dropScript();
        }

        void operator=(JS::HandleFunction fun)
        {
            dropScript();
            holdScript(fun);
        }

        operator JS::HandleScript() const { return script_; }
        explicit operator bool() const { return script_; }

      private:
        void holdScript(JS::HandleFunction fun);
        void dropScript();
    };
};

/* If this fails, add/remove padding within JSScript. */
static_assert(sizeof(JSScript) % js::gc::CellSize == 0,
              "Size of JSScript must be an integral multiple of js::gc::CellSize");

namespace js {

/*
 * Iterator over a script's bindings (formals and variables).
 * The order of iteration is:
 *  - first, formal arguments, from index 0 to numArgs
 *  - next, variables, from index 0 to numLocals
 */
class BindingIter
{
    Handle<Bindings> bindings_;
    uint32_t i_;
    uint32_t unaliasedLocal_;

    friend class ::JSScript;
    friend class Bindings;

  public:
    explicit BindingIter(Handle<Bindings> bindings)
      : bindings_(bindings), i_(0), unaliasedLocal_(0)
    {}

    explicit BindingIter(const HandleScript& script)
      : bindings_(Handle<Bindings>::fromMarkedLocation(&script->bindings)),
        i_(0), unaliasedLocal_(0)
    {}

    bool done() const { return i_ == bindings_.count(); }
    explicit operator bool() const { return !done(); }
    BindingIter& operator++() { (*this)++; return *this; }

    void operator++(int) {
        MOZ_ASSERT(!done());
        const Binding& binding = **this;
        if (binding.kind() != Binding::ARGUMENT && !binding.aliased())
            unaliasedLocal_++;
        i_++;
    }

    // Stack slots are assigned to arguments (aliased and unaliased) and
    // unaliased locals. frameIndex() returns the slot index. It's invalid to
    // call this method when the iterator is stopped on an aliased local, as it
    // has no stack slot.
    uint32_t frameIndex() const {
        MOZ_ASSERT(!done());
        if (i_ < bindings_.numArgs())
            return i_;
        MOZ_ASSERT(!(*this)->aliased());
        return unaliasedLocal_;
    }

    // If the current binding is an argument, argIndex() returns its index.
    // It returns the same value as frameIndex(), as slots are allocated for
    // both unaliased and aliased arguments.
    uint32_t argIndex() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(i_ < bindings_.numArgs());
        return i_;
    }
    uint32_t argOrLocalIndex() const {
        MOZ_ASSERT(!done());
        return i_ < bindings_.numArgs() ? i_ : i_ - bindings_.numArgs();
    }
    uint32_t localIndex() const {
        MOZ_ASSERT(!done());
        MOZ_ASSERT(i_ >= bindings_.numArgs());
        return i_ - bindings_.numArgs();
    }
    bool isBodyLevelLexical() const {
        MOZ_ASSERT(!done());
        const Binding& binding = **this;
        return binding.kind() != Binding::ARGUMENT;
    }

    const Binding& operator*() const { MOZ_ASSERT(!done()); return bindings_.bindingArray()[i_]; }
    const Binding* operator->() const { MOZ_ASSERT(!done()); return &bindings_.bindingArray()[i_]; }
};

/*
 * Iterator over the aliased formal bindings in ascending index order. This can
 * be veiwed as a filtering of BindingIter with predicate
 *   bi->aliased() && bi->kind() == Binding::ARGUMENT
 */
class AliasedFormalIter
{
    const Binding* begin_;
    const Binding* p_;
    const Binding* end_;
    unsigned slot_;

    void settle() {
        while (p_ != end_ && !p_->aliased())
            p_++;
    }

  public:
    explicit inline AliasedFormalIter(JSScript* script);

    bool done() const { return p_ == end_; }
    explicit operator bool() const { return !done(); }
    void operator++(int) { MOZ_ASSERT(!done()); p_++; slot_++; settle(); }

    const Binding& operator*() const { MOZ_ASSERT(!done()); return *p_; }
    const Binding* operator->() const { MOZ_ASSERT(!done()); return p_; }
    unsigned frameIndex() const { MOZ_ASSERT(!done()); return p_ - begin_; }
    unsigned scopeSlot() const { MOZ_ASSERT(!done()); return slot_; }
};

// Information about a script which may be (or has been) lazily compiled to
// bytecode from its source.
class LazyScript : public gc::TenuredCell
{
  public:
    class FreeVariable
    {
        // Variable name is stored as a tagged JSAtom pointer.
        uintptr_t bits_;

        static const uintptr_t HOISTED_USE_BIT = 0x1;
        static const uintptr_t MASK = ~HOISTED_USE_BIT;

      public:
        explicit FreeVariable()
          : bits_(0)
        { }

        explicit FreeVariable(JSAtom* name)
          : bits_(uintptr_t(name))
        {
            // We rely on not requiring any write barriers so we can tag the
            // pointer. This code needs to change if we start allocating
            // JSAtoms inside the nursery.
            MOZ_ASSERT(!IsInsideNursery(name));
        }

        JSAtom* atom() const { return (JSAtom*)(bits_ & MASK); }
        void setIsHoistedUse() { bits_ |= HOISTED_USE_BIT; }
        bool isHoistedUse() const { return bool(bits_ & HOISTED_USE_BIT); }
    };

  private:
    // If non-nullptr, the script has been compiled and this is a forwarding
    // pointer to the result. This is a weak pointer: after relazification, we
    // can collect the script if there are no other pointers to it.
    WeakRef<JSScript*> script_;

    // Original function with which the lazy script is associated.
    HeapPtrFunction function_;

    // Function or block chain in which the script is nested, or nullptr.
    HeapPtrObject enclosingScope_;

    // ScriptSourceObject, or nullptr if the script in which this is nested
    // has not been compiled yet. This is never a CCW; we don't clone
    // LazyScripts into other compartments.
    HeapPtrObject sourceObject_;

    // Heap allocated table with any free variables or inner functions.
    void* table_;

    // Add padding so LazyScript is gc::Cell aligned. Make padding protected
    // instead of private to suppress -Wunused-private-field compiler warnings.
  protected:
#if JS_BITS_PER_WORD == 32
    uint32_t padding;
#endif
  private:

    struct PackedView {
        // Assorted bits that should really be in ScriptSourceObject.
        uint32_t version : 8;

        uint32_t numFreeVariables : 24;
        uint32_t numInnerFunctions : 20;

        uint32_t generatorKindBits : 2;

        // N.B. These are booleans but need to be uint32_t to pack correctly on MSVC.
        // If you add another boolean here, make sure to initialze it in
        // LazyScript::CreateRaw().
        uint32_t strict : 1;
        uint32_t bindingsAccessedDynamically : 1;
        uint32_t hasDebuggerStatement : 1;
        uint32_t hasDirectEval : 1;
        uint32_t usesArgumentsApplyAndThis : 1;
        uint32_t hasBeenCloned : 1;
        uint32_t treatAsRunOnce : 1;
        uint32_t isDerivedClassConstructor : 1;
        uint32_t needsHomeObject : 1;
    };

    union {
        PackedView p_;
        uint64_t packedFields_;
    };

    // Source location for the script.
    uint32_t begin_;
    uint32_t end_;
    uint32_t lineno_;
    uint32_t column_;

    LazyScript(JSFunction* fun, void* table, uint64_t packedFields,
               uint32_t begin, uint32_t end, uint32_t lineno, uint32_t column);

    // Create a LazyScript without initializing the freeVariables and the
    // innerFunctions. To be GC-safe, the caller must initialize both vectors
    // with valid atoms and functions.
    static LazyScript* CreateRaw(ExclusiveContext* cx, HandleFunction fun,
                                 uint64_t packedData, uint32_t begin, uint32_t end,
                                 uint32_t lineno, uint32_t column);

  public:
    // Create a LazyScript without initializing the freeVariables and the
    // innerFunctions. To be GC-safe, the caller must initialize both vectors
    // with valid atoms and functions.
    static LazyScript* CreateRaw(ExclusiveContext* cx, HandleFunction fun,
                                 uint32_t numFreeVariables, uint32_t numInnerFunctions,
                                 JSVersion version, uint32_t begin, uint32_t end,
                                 uint32_t lineno, uint32_t column);

    // Create a LazyScript and initialize the freeVariables and the
    // innerFunctions with dummy values to be replaced in a later initialization
    // phase.
    //
    // The "script" argument to this function can be null.  If it's non-null,
    // then this LazyScript should be associated with the given JSScript.
    //
    // The sourceObjectScript argument must be non-null and is the script that
    // should be used to get the sourceObject_ of this lazyScript.
    static LazyScript* Create(ExclusiveContext* cx, HandleFunction fun,
                              HandleScript script, HandleObject enclosingScope,
                              HandleScript sourceObjectScript,
                              uint64_t packedData, uint32_t begin, uint32_t end,
                              uint32_t lineno, uint32_t column);

    void initRuntimeFields(uint64_t packedFields);

    inline JSFunction* functionDelazifying(JSContext* cx) const;
    JSFunction* functionNonDelazifying() const {
        return function_;
    }

    void initScript(JSScript* script);
    void resetScript();

    JSScript* maybeScript() {
        return script_;
    }
    const JSScript* maybeScriptUnbarriered() const {
        return script_.unbarrieredGet();
    }
    bool hasScript() const {
        return bool(script_);
    }

    JSObject* enclosingScope() const {
        return enclosingScope_;
    }

    // Switch the script over from the off-thread compartment's static
    // global lexical scope to the main thread compartment's.
    void fixEnclosingStaticGlobalLexicalScope();

    ScriptSourceObject* sourceObject() const;
    ScriptSource* scriptSource() const {
        return sourceObject()->source();
    }
    ScriptSource* maybeForwardedScriptSource() const;
    bool mutedErrors() const {
        return scriptSource()->mutedErrors();
    }
    JSVersion version() const {
        JS_STATIC_ASSERT(JSVERSION_UNKNOWN == -1);
        return (p_.version == JS_BIT(8) - 1) ? JSVERSION_UNKNOWN : JSVersion(p_.version);
    }

    void setParent(JSObject* enclosingScope, ScriptSourceObject* sourceObject);

    uint32_t numFreeVariables() const {
        return p_.numFreeVariables;
    }
    FreeVariable* freeVariables() {
        return (FreeVariable*)table_;
    }

    uint32_t numInnerFunctions() const {
        return p_.numInnerFunctions;
    }
    HeapPtrFunction* innerFunctions() {
        return (HeapPtrFunction*)&freeVariables()[numFreeVariables()];
    }

    GeneratorKind generatorKind() const { return GeneratorKindFromBits(p_.generatorKindBits); }

    bool isGenerator() const { return generatorKind() != NotGenerator; }

    bool isLegacyGenerator() const { return generatorKind() == LegacyGenerator; }

    bool isStarGenerator() const { return generatorKind() == StarGenerator; }

    void setGeneratorKind(GeneratorKind kind) {
        // A script only gets its generator kind set as part of initialization,
        // so it can only transition from NotGenerator.
        MOZ_ASSERT(!isGenerator());
        // Legacy generators cannot currently be lazy.
        MOZ_ASSERT(kind != LegacyGenerator);
        p_.generatorKindBits = GeneratorKindAsBits(kind);
    }

    bool strict() const {
        return p_.strict;
    }
    void setStrict() {
        p_.strict = true;
    }

    bool bindingsAccessedDynamically() const {
        return p_.bindingsAccessedDynamically;
    }
    void setBindingsAccessedDynamically() {
        p_.bindingsAccessedDynamically = true;
    }

    bool hasDebuggerStatement() const {
        return p_.hasDebuggerStatement;
    }
    void setHasDebuggerStatement() {
        p_.hasDebuggerStatement = true;
    }

    bool hasDirectEval() const {
        return p_.hasDirectEval;
    }
    void setHasDirectEval() {
        p_.hasDirectEval = true;
    }

    bool usesArgumentsApplyAndThis() const {
        return p_.usesArgumentsApplyAndThis;
    }
    void setUsesArgumentsApplyAndThis() {
        p_.usesArgumentsApplyAndThis = true;
    }

    bool hasBeenCloned() const {
        return p_.hasBeenCloned;
    }
    void setHasBeenCloned() {
        p_.hasBeenCloned = true;
    }

    bool treatAsRunOnce() const {
        return p_.treatAsRunOnce;
    }
    void setTreatAsRunOnce() {
        p_.treatAsRunOnce = true;
    }

    bool isDerivedClassConstructor() const {
        return p_.isDerivedClassConstructor;
    }
    void setIsDerivedClassConstructor() {
        p_.isDerivedClassConstructor = true;
    }

    bool needsHomeObject() const {
        return p_.needsHomeObject;
    }
    void setNeedsHomeObject() {
        p_.needsHomeObject = true;
    }

    const char* filename() const {
        return scriptSource()->filename();
    }
    uint32_t begin() const {
        return begin_;
    }
    uint32_t end() const {
        return end_;
    }
    uint32_t lineno() const {
        return lineno_;
    }
    uint32_t column() const {
        return column_;
    }

    bool hasUncompiledEnclosingScript() const;

    friend class GCMarker;
    void traceChildren(JSTracer* trc);
    void finalize(js::FreeOp* fop);
    void fixupAfterMovingGC() {}

    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_LAZY_SCRIPT; }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
    {
        return mallocSizeOf(table_);
    }

    uint64_t packedFields() const {
        return packedFields_;
    }
};

/* If this fails, add/remove padding within LazyScript. */
JS_STATIC_ASSERT(sizeof(LazyScript) % js::gc::CellSize == 0);

struct SharedScriptData
{
    uint32_t length;
    uint32_t natoms;
    mozilla::Atomic<bool, mozilla::ReleaseAcquire> marked;
    jsbytecode data[1];

    static SharedScriptData* new_(ExclusiveContext* cx, uint32_t codeLength,
                                  uint32_t srcnotesLength, uint32_t natoms);

    HeapPtrAtom* atoms() {
        if (!natoms)
            return nullptr;
        return reinterpret_cast<HeapPtrAtom*>(data + length - sizeof(JSAtom*) * natoms);
    }

    static SharedScriptData* fromBytecode(const jsbytecode* bytecode) {
        return (SharedScriptData*)(bytecode - offsetof(SharedScriptData, data));
    }

  private:
    SharedScriptData() = delete;
    SharedScriptData(const SharedScriptData&) = delete;
};

struct ScriptBytecodeHasher
{
    struct Lookup
    {
        jsbytecode*         code;
        uint32_t            length;

        explicit Lookup(SharedScriptData* ssd) : code(ssd->data), length(ssd->length) {}
    };
    static HashNumber hash(const Lookup& l) { return mozilla::HashBytes(l.code, l.length); }
    static bool match(SharedScriptData* entry, const Lookup& lookup) {
        if (entry->length != lookup.length)
            return false;
        return mozilla::PodEqual<jsbytecode>(entry->data, lookup.code, lookup.length);
    }
};

typedef HashSet<SharedScriptData*,
                ScriptBytecodeHasher,
                SystemAllocPolicy> ScriptDataTable;

extern void
UnmarkScriptData(JSRuntime* rt);

extern void
SweepScriptData(JSRuntime* rt);

extern void
FreeScriptData(JSRuntime* rt);

struct ScriptAndCounts
{
    /* This structure is stored and marked from the JSRuntime. */
    JSScript* script;
    ScriptCounts scriptCounts;

    inline explicit ScriptAndCounts(JSScript* script);
    inline ScriptAndCounts(ScriptAndCounts&& sac);

    const PCCounts* maybeGetPCCounts(jsbytecode* pc) const {
        return scriptCounts.maybeGetPCCounts(script->pcToOffset(pc));
    }
    const PCCounts* maybeGetThrowCounts(jsbytecode* pc) const {
        return scriptCounts.maybeGetThrowCounts(script->pcToOffset(pc));
    }

    jit::IonScriptCounts* getIonCounts() const {
        return scriptCounts.ionCounts_;
    }

    void trace(JSTracer* trc) {
        TraceRoot(trc, &script, "ScriptAndCounts::script");
    }
};

struct GSNCache;

jssrcnote*
GetSrcNote(GSNCache& cache, JSScript* script, jsbytecode* pc);

extern jssrcnote*
GetSrcNote(JSContext* cx, JSScript* script, jsbytecode* pc);

extern jsbytecode*
LineNumberToPC(JSScript* script, unsigned lineno);

extern JS_FRIEND_API(unsigned)
GetScriptLineExtent(JSScript* script);

} /* namespace js */

namespace js {

extern unsigned
PCToLineNumber(JSScript* script, jsbytecode* pc, unsigned* columnp = nullptr);

extern unsigned
PCToLineNumber(unsigned startLine, jssrcnote* notes, jsbytecode* code, jsbytecode* pc,
               unsigned* columnp = nullptr);

/*
 * This function returns the file and line number of the script currently
 * executing on cx. If there is no current script executing on cx (e.g., a
 * native called directly through JSAPI (e.g., by setTimeout)), nullptr and 0
 * are returned as the file and line. Additionally, this function avoids the
 * full linear scan to compute line number when the caller guarantees that the
 * script compilation occurs at a JSOP_EVAL/JSOP_SPREADEVAL.
 */

enum LineOption {
    CALLED_FROM_JSOP_EVAL,
    NOT_CALLED_FROM_JSOP_EVAL
};

extern void
DescribeScriptedCallerForCompilation(JSContext* cx, MutableHandleScript maybeScript,
                                     const char** file, unsigned* linenop,
                                     uint32_t* pcOffset, bool* mutedErrors,
                                     LineOption opt = NOT_CALLED_FROM_JSOP_EVAL);

JSScript*
CloneScriptIntoFunction(JSContext* cx, HandleObject enclosingScope, HandleFunction fun,
                        HandleScript src);

JSScript*
CloneGlobalScript(JSContext* cx, Handle<ScopeObject*> enclosingScope, HandleScript src);

} /* namespace js */

// JS::ubi::Nodes can point to js::LazyScripts; they're js::gc::Cell instances
// with no associated compartment.
namespace JS {
namespace ubi {
template<>
struct Concrete<js::LazyScript> : TracerConcrete<js::LazyScript> {
    CoarseType coarseType() const final { return CoarseType::Script; }
    Size size(mozilla::MallocSizeOf mallocSizeOf) const override;
    const char* scriptFilename() const final;

  protected:
    explicit Concrete(js::LazyScript *ptr) : TracerConcrete<js::LazyScript>(ptr) { }

  public:
    static void construct(void *storage, js::LazyScript *ptr) { new (storage) Concrete(ptr); }
};
} // namespace ubi
} // namespace JS

#endif /* jsscript_h */

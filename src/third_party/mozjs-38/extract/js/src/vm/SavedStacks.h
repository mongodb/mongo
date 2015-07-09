/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SavedStacks_h
#define vm_SavedStacks_h

#include "jscntxt.h"
#include "jsmath.h"
#include "js/HashTable.h"
#include "vm/Stack.h"

namespace js {

class SavedFrame;
typedef JS::Handle<SavedFrame*> HandleSavedFrame;
typedef JS::MutableHandle<SavedFrame*> MutableHandleSavedFrame;
typedef JS::Rooted<SavedFrame*> RootedSavedFrame;

class SavedFrame : public NativeObject {
    friend class SavedStacks;

  public:
    static const Class          class_;
    static void finalize(FreeOp* fop, JSObject* obj);
    static const JSPropertySpec protoAccessors[];
    static const JSFunctionSpec protoFunctions[];
    static const JSFunctionSpec staticFunctions[];

    // Prototype methods and properties to be exposed to JS.
    static bool construct(JSContext* cx, unsigned argc, Value* vp);
    static bool sourceProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool lineProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool columnProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool functionDisplayNameProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool parentProperty(JSContext* cx, unsigned argc, Value* vp);
    static bool toStringMethod(JSContext* cx, unsigned argc, Value* vp);

    // Convenient getters for SavedFrame's reserved slots for use from C++.
    JSAtom*      getSource();
    uint32_t     getLine();
    uint32_t     getColumn();
    JSAtom*      getFunctionDisplayName();
    SavedFrame*  getParent();
    JSPrincipals* getPrincipals();

    bool         isSelfHosted();

    struct Lookup;
    struct HashPolicy;

    typedef HashSet<js::ReadBarriered<SavedFrame*>,
                    HashPolicy,
                    SystemAllocPolicy> Set;

    typedef RootedGeneric<Lookup*> AutoLookupRooter;
    typedef AutoLookupRooter& HandleLookup;
    class AutoLookupVector;

  private:
    static bool finishSavedFrameInit(JSContext* cx, HandleObject ctor, HandleObject proto);
    void initFromLookup(HandleLookup lookup);

    enum {
        // The reserved slots in the SavedFrame class.
        JSSLOT_SOURCE,
        JSSLOT_LINE,
        JSSLOT_COLUMN,
        JSSLOT_FUNCTIONDISPLAYNAME,
        JSSLOT_PARENT,
        JSSLOT_PRINCIPALS,
        JSSLOT_PRIVATE_PARENT,

        // The total number of reserved slots in the SavedFrame class.
        JSSLOT_COUNT
    };

    // Because we hash the parent pointer, we need to rekey a saved frame
    // whenever its parent was relocated by the GC. However, the GC doesn't
    // notify us when this occurs. As a work around, we keep a duplicate copy of
    // the parent pointer as a private value in a reserved slot. Whenever the
    // private value parent pointer doesn't match the regular parent pointer, we
    // know that GC moved the parent and we need to update our private value and
    // rekey the saved frame in its hash set. These two methods are helpers for
    // this process.
    bool parentMoved();
    void updatePrivateParent();

    static bool checkThis(JSContext* cx, CallArgs& args, const char* fnName,
                          MutableHandleSavedFrame frame);
};

struct SavedFrame::HashPolicy
{
    typedef SavedFrame::Lookup               Lookup;
    typedef PointerHasher<SavedFrame*, 3>   SavedFramePtrHasher;
    typedef PointerHasher<JSPrincipals*, 3> JSPrincipalsPtrHasher;

    static HashNumber hash(const Lookup& lookup);
    static bool       match(SavedFrame* existing, const Lookup& lookup);

    typedef ReadBarriered<SavedFrame*> Key;
    static void rekey(Key& key, const Key& newKey);
};

class SavedStacks {
    friend bool SavedStacksMetadataCallback(JSContext* cx, JSObject** pmetadata);

  public:
    SavedStacks()
      : frames(),
        allocationSamplingProbability(1.0),
        allocationSkipCount(0),
        // XXX: Initialize the RNG state to 0 so that random_initSeed is lazily
        // called for us on the first call to random_next (via
        // random_nextDouble). We need to do this here because /dev/urandom
        // doesn't exist on Android, resulting in assertion failures.
        rngState(0)
    { }

    bool     init();
    bool     initialized() const { return frames.initialized(); }
    bool     saveCurrentStack(JSContext* cx, MutableHandleSavedFrame frame, unsigned maxFrameCount = 0);
    void     sweep(JSRuntime* rt);
    void     trace(JSTracer* trc);
    uint32_t count();
    void     clear();
    void     setRNGState(uint64_t state) { rngState = state; }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

  private:
    SavedFrame::Set     frames;
    double              allocationSamplingProbability;
    uint32_t            allocationSkipCount;
    uint64_t            rngState;

    bool       insertFrames(JSContext* cx, FrameIter& iter, MutableHandleSavedFrame frame,
                            unsigned maxFrameCount = 0);
    SavedFrame* getOrCreateSavedFrame(JSContext* cx, SavedFrame::HandleLookup lookup);
    SavedFrame* createFrameFromLookup(JSContext* cx, SavedFrame::HandleLookup lookup);
    void       chooseSamplingProbability(JSContext* cx);

    // Cache for memoizing PCToLineNumber lookups.

    struct PCKey {
        PCKey(JSScript* script, jsbytecode* pc) : script(script), pc(pc) { }

        PreBarrieredScript script;
        jsbytecode*        pc;
    };

    struct LocationValue {
        LocationValue() : source(nullptr), line(0), column(0) { }
        LocationValue(JSAtom* source, size_t line, uint32_t column)
            : source(source),
              line(line),
              column(column)
        { }

        void trace(JSTracer* trc) {
            if (source)
                gc::MarkString(trc, &source, "SavedStacks::LocationValue::source");
        }

        PreBarrieredAtom source;
        size_t           line;
        uint32_t         column;
    };

    class MOZ_STACK_CLASS AutoLocationValueRooter : public JS::CustomAutoRooter
    {
      public:
        explicit AutoLocationValueRooter(JSContext* cx)
            : JS::CustomAutoRooter(cx),
              value() {}

        inline LocationValue* operator->() { return &value; }
        void set(LocationValue& loc) { value = loc; }
        LocationValue& get() { return value; }

      private:
        virtual void trace(JSTracer* trc) {
            value.trace(trc);
        }

        SavedStacks::LocationValue value;
    };

    class MOZ_STACK_CLASS MutableHandleLocationValue
    {
      public:
        inline MOZ_IMPLICIT MutableHandleLocationValue(AutoLocationValueRooter* location)
            : location(location) {}

        inline LocationValue* operator->() { return &location->get(); }
        void set(LocationValue& loc) { location->set(loc); }

      private:
        AutoLocationValueRooter* location;
    };

    struct PCLocationHasher : public DefaultHasher<PCKey> {
        typedef PointerHasher<JSScript*, 3>   ScriptPtrHasher;
        typedef PointerHasher<jsbytecode*, 3> BytecodePtrHasher;

        static HashNumber hash(const PCKey& key) {
            return mozilla::AddToHash(ScriptPtrHasher::hash(key.script),
                                      BytecodePtrHasher::hash(key.pc));
        }

        static bool match(const PCKey& l, const PCKey& k) {
            return l.script == k.script && l.pc == k.pc;
        }
    };

    typedef HashMap<PCKey, LocationValue, PCLocationHasher, SystemAllocPolicy> PCLocationMap;

    PCLocationMap pcLocationMap;

    void sweepPCLocationMap();
    bool getLocation(JSContext* cx, const FrameIter& iter, MutableHandleLocationValue locationp);
};

bool SavedStacksMetadataCallback(JSContext* cx, JSObject** pmetadata);

} /* namespace js */

#endif /* vm_SavedStacks_h */

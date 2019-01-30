/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2017 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmProcess.h"

#include "mozilla/BinarySearch.h"

#include "vm/MutexIDs.h"
#include "wasm/WasmCode.h"

using namespace js;
using namespace wasm;

using mozilla::BinarySearchIf;

// Per-process map from values of program-counter (pc) to CodeSegments.
//
// Whenever a new CodeSegment is ready to use, it has to be registered so that
// we can have fast lookups from pc to CodeSegments in numerous places. Since
// wasm compilation may be tiered, and the second tier doesn't have access to
// any JSContext/JSCompartment/etc lying around, we have to use a process-wide
// map instead.

typedef Vector<const CodeSegment*, 0, SystemAllocPolicy> CodeSegmentVector;

Atomic<bool> wasm::CodeExists(false);

class ProcessCodeSegmentMap
{
    // Since writes (insertions or removals) can happen on any background
    // thread at the same time, we need a lock here.

    Mutex mutatorsMutex_;

    CodeSegmentVector segments1_;
    CodeSegmentVector segments2_;

    // Because of sampling/interruptions/stack iteration in general, the
    // thread running wasm might need to know to which CodeSegment the
    // current PC belongs, during a call to lookup(). A lookup is a
    // read-only operation, and we don't want to take a lock then
    // (otherwise, we could have a deadlock situation if an async lookup
    // happened on a given thread that was holding mutatorsMutex_ while getting
    // interrupted/sampled). Since the writer could be modifying the data that
    // is getting looked up, the writer functions use spin-locks to know if
    // there are any observers (i.e. calls to lookup()) of the atomic data.

    Atomic<size_t> observers_;

    // Except during swapAndWait(), there are no lookup() observers of the
    // vector pointed to by mutableCodeSegments_

    CodeSegmentVector* mutableCodeSegments_;
    Atomic<const CodeSegmentVector*> readonlyCodeSegments_;

    struct CodeSegmentPC
    {
        const void* pc;
        explicit CodeSegmentPC(const void* pc) : pc(pc) {}
        int operator()(const CodeSegment* cs) const {
            if (cs->containsCodePC(pc))
                return 0;
            if (pc < cs->base())
                return -1;
            return 1;
        }
    };

    void swapAndWait() {
        // Both vectors are consistent for look up at this point, although their
        // contents are different: there is no way for the looked up PC to be
        // in the code segment that is getting registered, because the code
        // segment is not even fully created yet.

        // If a lookup happens before this instruction, then the
        // soon-to-become-former read-only pointer is used during the lookup,
        // which is valid.

        mutableCodeSegments_ = const_cast<CodeSegmentVector*>(
            readonlyCodeSegments_.exchange(mutableCodeSegments_)
        );

        // If a lookup happens after this instruction, then the updated vector
        // is used, which is valid:
        // - in case of insertion, it means the new vector contains more data,
        // but it's fine since the code segment is getting registered and thus
        // isn't even fully created yet, so the code can't be running.
        // - in case of removal, it means the new vector contains one less
        // entry, but it's fine since unregistering means the code segment
        // isn't used by any live instance anymore, thus PC can't be in the
        // to-be-removed code segment's range.

        // A lookup could have happened on any of the two vectors. Wait for
        // observers to be done using any vector before mutating.

        while (observers_);
    }

  public:
    ProcessCodeSegmentMap()
      : mutatorsMutex_(mutexid::WasmCodeSegmentMap),
        observers_(0),
        mutableCodeSegments_(&segments1_),
        readonlyCodeSegments_(&segments2_)
    {
    }

    ~ProcessCodeSegmentMap()
    {
        MOZ_ASSERT(segments1_.empty());
        MOZ_ASSERT(segments2_.empty());
    }

    void freeAll() {
        MOZ_ASSERT(segments1_.empty());
        MOZ_ASSERT(segments2_.empty());
        segments1_.clearAndFree();
        segments2_.clearAndFree();
    }

    bool insert(const CodeSegment* cs) {
        LockGuard<Mutex> lock(mutatorsMutex_);

        size_t index;
        MOZ_ALWAYS_FALSE(BinarySearchIf(*mutableCodeSegments_, 0, mutableCodeSegments_->length(),
                                        CodeSegmentPC(cs->base()), &index));

        if (!mutableCodeSegments_->insert(mutableCodeSegments_->begin() + index, cs))
            return false;

        CodeExists = true;

        swapAndWait();

#ifdef DEBUG
        size_t otherIndex;
        MOZ_ALWAYS_FALSE(BinarySearchIf(*mutableCodeSegments_, 0, mutableCodeSegments_->length(),
                                        CodeSegmentPC(cs->base()), &otherIndex));
        MOZ_ASSERT(index == otherIndex);
#endif

        // Although we could simply revert the insertion in the read-only
        // vector, it is simpler to just crash and given that each CodeSegment
        // consumes multiple pages, it is unlikely this insert() would OOM in
        // practice
        AutoEnterOOMUnsafeRegion oom;
        if (!mutableCodeSegments_->insert(mutableCodeSegments_->begin() + index, cs))
            oom.crash("when inserting a CodeSegment in the process-wide map");

        return true;
    }

    void remove(const CodeSegment* cs) {
        LockGuard<Mutex> lock(mutatorsMutex_);

        size_t index;
        MOZ_ALWAYS_TRUE(BinarySearchIf(*mutableCodeSegments_, 0, mutableCodeSegments_->length(),
                                       CodeSegmentPC(cs->base()), &index));

        mutableCodeSegments_->erase(mutableCodeSegments_->begin() + index);

        if (!mutableCodeSegments_->length())
            CodeExists = false;

        swapAndWait();

#ifdef DEBUG
        size_t otherIndex;
        MOZ_ALWAYS_TRUE(BinarySearchIf(*mutableCodeSegments_, 0, mutableCodeSegments_->length(),
                                       CodeSegmentPC(cs->base()), &otherIndex));
        MOZ_ASSERT(index == otherIndex);
#endif

        mutableCodeSegments_->erase(mutableCodeSegments_->begin() + index);
    }

    const CodeSegment* lookup(const void* pc) {
        auto decObserver = mozilla::MakeScopeExit([&] {
            observers_--;
        });
        observers_++;

        // Once atomically-read, the readonly vector is valid as long as
        // observers_ has been incremented (see swapAndWait()).
        const CodeSegmentVector* readonly = readonlyCodeSegments_;

        size_t index;
        if (!BinarySearchIf(*readonly, 0, readonly->length(), CodeSegmentPC(pc), &index))
            return nullptr;

        // It is fine returning a raw CodeSegment*, because we assume we are
        // looking up a live PC in code which is on the stack, keeping the
        // CodeSegment alive.

        return (*readonly)[index];
    }
};

static ProcessCodeSegmentMap processCodeSegmentMap;

bool
wasm::RegisterCodeSegment(const CodeSegment* cs)
{
    return processCodeSegmentMap.insert(cs);
}

void
wasm::UnregisterCodeSegment(const CodeSegment* cs)
{
    processCodeSegmentMap.remove(cs);
}

const CodeSegment*
wasm::LookupCodeSegment(const void* pc, const CodeRange** cr /*= nullptr */)
{
    if (const CodeSegment* found = processCodeSegmentMap.lookup(pc)) {
        if (cr) {
            *cr = found->isModule()
                  ? found->asModule()->lookupRange(pc)
                  : found->asLazyStub()->lookupRange(pc);
        }
        return found;
    }
    return nullptr;
}

const Code*
wasm::LookupCode(const void* pc, const CodeRange** cr /* = nullptr */)
{
    const CodeSegment* found = LookupCodeSegment(pc, cr);
    return found ? &found->code() : nullptr;
}

void
wasm::ShutDownProcessStaticData()
{
    processCodeSegmentMap.freeAll();
}

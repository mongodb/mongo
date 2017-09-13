/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_RegExpStatics_h
#define vm_RegExpStatics_h

#include "gc/Marking.h"
#include "vm/MatchPairs.h"
#include "vm/RegExpObject.h"
#include "vm/Runtime.h"

namespace js {

class GlobalObject;
class RegExpStaticsObject;

class RegExpStatics
{
    /* The latest RegExp output, set after execution. */
    VectorMatchPairs        matches;
    RelocatablePtrLinearString matchesInput;

    /*
     * The previous RegExp input, used to resolve lazy state.
     * A raw RegExpShared cannot be stored because it may be in
     * a different compartment via evalcx().
     */
    RelocatablePtrAtom      lazySource;
    RegExpFlag              lazyFlags;
    size_t                  lazyIndex;

    /* The latest RegExp input, set before execution. */
    RelocatablePtrString    pendingInput;
    RegExpFlag              flags;

    /*
     * If non-zero, |matchesInput| and the |lazy*| fields may be used
     * to replay the last executed RegExp, and |matches| is invalid.
     */
    int32_t                 pendingLazyEvaluation;

  public:
    RegExpStatics() { clear(); }
    static RegExpStaticsObject* create(ExclusiveContext* cx, Handle<GlobalObject*> parent);

  private:
    bool executeLazy(JSContext* cx);

    inline void checkInvariants();

    /*
     * Check whether a match for pair |pairNum| occurred.  If so, allocate and
     * store the match string in |*out|; otherwise place |undefined| in |*out|.
     */
    bool makeMatch(JSContext* cx, size_t pairNum, MutableHandleValue out);
    bool createDependent(JSContext* cx, size_t start, size_t end, MutableHandleValue out);

    void markFlagsSet(JSContext* cx);

    struct InitBuffer {};
    explicit RegExpStatics(InitBuffer) {}

    friend class AutoRegExpStaticsBuffer;

  public:
    /* Mutators. */
    inline void updateLazily(JSContext* cx, JSLinearString* input,
                             RegExpShared* shared, size_t lastIndex);
    inline bool updateFromMatchPairs(JSContext* cx, JSLinearString* input, MatchPairs& newPairs);

    void setMultiline(JSContext* cx, bool enabled) {
        if (enabled) {
            flags = RegExpFlag(flags | MultilineFlag);
            markFlagsSet(cx);
        } else {
            flags = RegExpFlag(flags & ~MultilineFlag);
        }
    }

    inline void clear();

    /* Corresponds to JSAPI functionality to set the pending RegExp input. */
    void reset(JSContext* cx, JSString* newInput, bool newMultiline) {
        clear();
        pendingInput = newInput;
        setMultiline(cx, newMultiline);
        checkInvariants();
    }

    inline void setPendingInput(JSString* newInput);

  public:
    /* Default match accessor. */
    const MatchPairs& getMatches() const {
        /* Safe: only used by String methods, which do not set lazy mode. */
        MOZ_ASSERT(!pendingLazyEvaluation);
        return matches;
    }

    JSString* getPendingInput() const { return pendingInput; }

    RegExpFlag getFlags() const { return flags; }
    bool multiline() const { return flags & MultilineFlag; }

    void mark(JSTracer* trc) {
        /*
         * Changes to this function must also be reflected in
         * RegExpStatics::AutoRooter::trace().
         */
        if (matchesInput)
            TraceEdge(trc, &matchesInput, "res->matchesInput");
        if (lazySource)
            TraceEdge(trc, &lazySource, "res->lazySource");
        if (pendingInput)
            TraceEdge(trc, &pendingInput, "res->pendingInput");
    }

    /* Value creators. */

    bool createPendingInput(JSContext* cx, MutableHandleValue out);
    bool createLastMatch(JSContext* cx, MutableHandleValue out);
    bool createLastParen(JSContext* cx, MutableHandleValue out);
    bool createParen(JSContext* cx, size_t pairNum, MutableHandleValue out);
    bool createLeftContext(JSContext* cx, MutableHandleValue out);
    bool createRightContext(JSContext* cx, MutableHandleValue out);

    /* Infallible substring creators. */

    void getParen(size_t pairNum, JSSubString* out) const;
    void getLastMatch(JSSubString* out) const;
    void getLastParen(JSSubString* out) const;
    void getLeftContext(JSSubString* out) const;
    void getRightContext(JSSubString* out) const;

    static size_t offsetOfPendingInput() {
        return offsetof(RegExpStatics, pendingInput);
    }

    static size_t offsetOfMatchesInput() {
        return offsetof(RegExpStatics, matchesInput);
    }

    static size_t offsetOfLazySource() {
        return offsetof(RegExpStatics, lazySource);
    }

    static size_t offsetOfLazyFlags() {
        return offsetof(RegExpStatics, lazyFlags);
    }

    static size_t offsetOfLazyIndex() {
        return offsetof(RegExpStatics, lazyIndex);
    }

    static size_t offsetOfPendingLazyEvaluation() {
        return offsetof(RegExpStatics, pendingLazyEvaluation);
    }
};

class MOZ_RAII AutoRegExpStaticsBuffer : private JS::CustomAutoRooter
{
  public:
    explicit AutoRegExpStaticsBuffer(JSContext* cx
                                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : CustomAutoRooter(cx), statics(RegExpStatics::InitBuffer())
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    RegExpStatics& getStatics() { return statics; }

  private:
    virtual void trace(JSTracer* trc) {
        if (statics.matchesInput) {
            TraceRoot(trc, reinterpret_cast<JSString**>(&statics.matchesInput),
                      "AutoRegExpStaticsBuffer matchesInput");
        }
        if (statics.lazySource) {
            TraceRoot(trc, reinterpret_cast<JSString**>(&statics.lazySource),
                      "AutoRegExpStaticsBuffer lazySource");
        }
        if (statics.pendingInput) {
            TraceRoot(trc, reinterpret_cast<JSString**>(&statics.pendingInput),
                      "AutoRegExpStaticsBuffer pendingInput");
        }
    }

    RegExpStatics statics;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

inline bool
RegExpStatics::createDependent(JSContext* cx, size_t start, size_t end, MutableHandleValue out)
{
    /* Private function: caller must perform lazy evaluation. */
    MOZ_ASSERT(!pendingLazyEvaluation);

    MOZ_ASSERT(start <= end);
    MOZ_ASSERT(end <= matchesInput->length());
    JSString* str = NewDependentString(cx, matchesInput, start, end - start);
    if (!str)
        return false;
    out.setString(str);
    return true;
}

inline bool
RegExpStatics::createPendingInput(JSContext* cx, MutableHandleValue out)
{
    /* Lazy evaluation need not be resolved to return the input. */
    out.setString(pendingInput ? pendingInput.get() : cx->runtime()->emptyString);
    return true;
}

inline bool
RegExpStatics::makeMatch(JSContext* cx, size_t pairNum, MutableHandleValue out)
{
    /* Private function: caller must perform lazy evaluation. */
    MOZ_ASSERT(!pendingLazyEvaluation);

    if (matches.empty() || pairNum >= matches.pairCount() || matches[pairNum].isUndefined()) {
        out.setUndefined();
        return true;
    }

    const MatchPair& pair = matches[pairNum];
    return createDependent(cx, pair.start, pair.limit, out);
}

inline bool
RegExpStatics::createLastMatch(JSContext* cx, MutableHandleValue out)
{
    if (!executeLazy(cx))
        return false;
    return makeMatch(cx, 0, out);
}

inline bool
RegExpStatics::createLastParen(JSContext* cx, MutableHandleValue out)
{
    if (!executeLazy(cx))
        return false;

    if (matches.empty() || matches.pairCount() == 1) {
        out.setString(cx->runtime()->emptyString);
        return true;
    }
    const MatchPair& pair = matches[matches.pairCount() - 1];
    if (pair.start == -1) {
        out.setString(cx->runtime()->emptyString);
        return true;
    }
    MOZ_ASSERT(pair.start >= 0 && pair.limit >= 0);
    MOZ_ASSERT(pair.limit >= pair.start);
    return createDependent(cx, pair.start, pair.limit, out);
}

inline bool
RegExpStatics::createParen(JSContext* cx, size_t pairNum, MutableHandleValue out)
{
    MOZ_ASSERT(pairNum >= 1);
    if (!executeLazy(cx))
        return false;

    if (matches.empty() || pairNum >= matches.pairCount()) {
        out.setString(cx->runtime()->emptyString);
        return true;
    }
    return makeMatch(cx, pairNum, out);
}

inline bool
RegExpStatics::createLeftContext(JSContext* cx, MutableHandleValue out)
{
    if (!executeLazy(cx))
        return false;

    if (matches.empty()) {
        out.setString(cx->runtime()->emptyString);
        return true;
    }
    if (matches[0].start < 0) {
        out.setUndefined();
        return true;
    }
    return createDependent(cx, 0, matches[0].start, out);
}

inline bool
RegExpStatics::createRightContext(JSContext* cx, MutableHandleValue out)
{
    if (!executeLazy(cx))
        return false;

    if (matches.empty()) {
        out.setString(cx->runtime()->emptyString);
        return true;
    }
    if (matches[0].limit < 0) {
        out.setUndefined();
        return true;
    }
    return createDependent(cx, matches[0].limit, matchesInput->length(), out);
}

inline void
RegExpStatics::getParen(size_t pairNum, JSSubString* out) const
{
    MOZ_ASSERT(!pendingLazyEvaluation);

    MOZ_ASSERT(pairNum >= 1 && pairNum < matches.pairCount());
    const MatchPair& pair = matches[pairNum];
    if (pair.isUndefined()) {
        out->initEmpty(matchesInput);
        return;
    }
    out->init(matchesInput, pair.start, pair.length());
}

inline void
RegExpStatics::getLastMatch(JSSubString* out) const
{
    MOZ_ASSERT(!pendingLazyEvaluation);

    if (matches.empty()) {
        out->initEmpty(matchesInput);
        return;
    }
    MOZ_ASSERT(matchesInput);
    MOZ_ASSERT(matches[0].limit >= matches[0].start);
    out->init(matchesInput, matches[0].start, matches[0].length());
}

inline void
RegExpStatics::getLastParen(JSSubString* out) const
{
    MOZ_ASSERT(!pendingLazyEvaluation);

    /* Note: the first pair is the whole match. */
    if (matches.empty() || matches.pairCount() == 1) {
        out->initEmpty(matchesInput);
        return;
    }
    getParen(matches.parenCount(), out);
}

inline void
RegExpStatics::getLeftContext(JSSubString* out) const
{
    MOZ_ASSERT(!pendingLazyEvaluation);

    if (matches.empty()) {
        out->initEmpty(matchesInput);
        return;
    }
    out->init(matchesInput, 0, matches[0].start);
}

inline void
RegExpStatics::getRightContext(JSSubString* out) const
{
    MOZ_ASSERT(!pendingLazyEvaluation);

    if (matches.empty()) {
        out->initEmpty(matchesInput);
        return;
    }
    MOZ_ASSERT(matches[0].limit <= int(matchesInput->length()));
    size_t length = matchesInput->length() - matches[0].limit;
    out->init(matchesInput, matches[0].limit, length);
}

inline void
RegExpStatics::updateLazily(JSContext* cx, JSLinearString* input,
                            RegExpShared* shared, size_t lastIndex)
{
    MOZ_ASSERT(input && shared);

    BarrieredSetPair<JSString, JSLinearString>(cx->zone(),
                                               pendingInput, input,
                                               matchesInput, input);

    lazySource = shared->source;
    lazyFlags = shared->flags;
    lazyIndex = lastIndex;
    pendingLazyEvaluation = 1;
}

inline bool
RegExpStatics::updateFromMatchPairs(JSContext* cx, JSLinearString* input, MatchPairs& newPairs)
{
    MOZ_ASSERT(input);

    /* Unset all lazy state. */
    pendingLazyEvaluation = false;
    this->lazySource = nullptr;
    this->lazyIndex = size_t(-1);

    BarrieredSetPair<JSString, JSLinearString>(cx->zone(),
                                               pendingInput, input,
                                               matchesInput, input);

    if (!matches.initArrayFrom(newPairs)) {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

inline void
RegExpStatics::clear()
{
    matches.forgetArray();
    matchesInput = nullptr;
    lazySource = nullptr;
    lazyFlags = RegExpFlag(0);
    lazyIndex = size_t(-1);
    pendingInput = nullptr;
    flags = RegExpFlag(0);
    pendingLazyEvaluation = false;
}

inline void
RegExpStatics::setPendingInput(JSString* newInput)
{
    pendingInput = newInput;
}

inline void
RegExpStatics::checkInvariants()
{
#ifdef DEBUG
    if (pendingLazyEvaluation) {
        MOZ_ASSERT(lazySource);
        MOZ_ASSERT(matchesInput);
        MOZ_ASSERT(lazyIndex != size_t(-1));
        return;
    }

    if (matches.empty()) {
        MOZ_ASSERT(!matchesInput);
        return;
    }

    /* Pair count is non-zero, so there must be match pairs input. */
    MOZ_ASSERT(matchesInput);
    size_t mpiLen = matchesInput->length();

    /* Both members of the first pair must be non-negative. */
    MOZ_ASSERT(!matches[0].isUndefined());
    MOZ_ASSERT(matches[0].limit >= 0);

    /* Present pairs must be valid. */
    for (size_t i = 0; i < matches.pairCount(); i++) {
        if (matches[i].isUndefined())
            continue;
        const MatchPair& pair = matches[i];
        MOZ_ASSERT(mpiLen >= size_t(pair.limit) && pair.limit >= pair.start && pair.start >= 0);
    }
#endif /* DEBUG */
}

} /* namespace js */

#endif /* vm_RegExpStatics_h */

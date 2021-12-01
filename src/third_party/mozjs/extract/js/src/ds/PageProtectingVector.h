/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_PageProtectingVector_h
#define ds_PageProtectingVector_h

#include "mozilla/Atomics.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Types.h"
#include "mozilla/Vector.h"

#include "ds/MemoryProtectionExceptionHandler.h"
#include "gc/Memory.h"

namespace js {

/*
 * PageProtectingVector is a vector that can only grow or be cleared, restricts
 * access to memory pages that haven't been used yet, and marks all of its fully
 * used memory pages as read-only. It can be used to detect heap corruption in
 * important buffers, since anything that tries to write into its protected
 * pages will crash. On Nightly and Aurora, these crashes will additionally be
 * annotated with a moz crash reason using MemoryProtectionExceptionHandler.
 *
 * PageProtectingVector's protection is limited to full pages. If the front
 * of its buffer is not aligned on a page boundary, elems preceding the first
 * page boundary will not be protected. Similarly, the end of the buffer will
 * not be fully protected unless it is aligned on a page boundary. Altogether,
 * up to two pages of memory may not be protected.
 */
template<typename T,
         size_t MinInlineCapacity = 0,
         class AllocPolicy = mozilla::MallocAllocPolicy,
         bool ProtectUsed = true,
         bool ProtectUnused = true,
         size_t InitialLowerBound = 0,
         bool PoisonUnused = true,
         uint8_t PoisonPattern = 0xe3>
class PageProtectingVector final
{
    mozilla::Vector<T, MinInlineCapacity, AllocPolicy> vector;

    static constexpr size_t toShift(size_t v) { return v <= 1 ? 0 : 1 + toShift(v >> 1); }

    static_assert((sizeof(T) & (sizeof(T) - 1)) == 0, "For performance reasons, "
                  "PageProtectingVector only works with power-of-2 sized elements!");

    static const size_t elemShift = toShift(sizeof(T));
    static const size_t elemSize = 1 << elemShift;
    static const size_t elemMask = elemSize - 1;

    /* We hardcode the page size here to minimize administrative overhead. */
    static const size_t pageShift = 12;
    static const size_t pageSize = 1 << pageShift;
    static const size_t pageMask = pageSize - 1;

    /*
     * The number of elements that can be added before we need to either adjust
     * the active page or resize the buffer. If |elemsUntilTest < 0| we will
     * take the slow paths in the append calls.
     */
    intptr_t elemsUntilTest;

    /*
     * The offset of the currently 'active' page - that is, the page that is
     * currently being written to. If both used and unused bytes are protected,
     * this will be the only (fully owned) page with read and write access.
     */
    size_t currPage;

    /*
     * The first fully owned page. This is the first page that can
     * be protected, but it may not be the first *active* page.
     */
    size_t initPage;

    /*
     * The last fully owned page. This is the last page that can
     * be protected, but it may not be the last *active* page.
     */
    size_t lastPage;

    /*
     * The size in elems that a buffer needs to be before its pages will be
     * protected. This is intended to reduce churn for small vectors while
     * still offering protection when they grow large enough.
     */
    size_t lowerBound;

#ifdef DEBUG
    bool regionUnprotected;
#endif

    bool usable;
    bool enabled;
    bool protectUsedEnabled;
    bool protectUnusedEnabled;

    MOZ_ALWAYS_INLINE void resetTest() {
        MOZ_ASSERT(protectUsedEnabled || protectUnusedEnabled);
        size_t nextPage = (pageSize - (uintptr_t(begin() + length()) & pageMask)) >> elemShift;
        size_t nextResize = capacity() - length();
        if (MOZ_LIKELY(nextPage <= nextResize))
            elemsUntilTest = intptr_t(nextPage);
        else
            elemsUntilTest = intptr_t(nextResize);
    }

    MOZ_ALWAYS_INLINE void setTestInitial() {
        if (MOZ_LIKELY(!protectUsedEnabled && !protectUnusedEnabled))
            elemsUntilTest = intptr_t(capacity() - length());
        else
            resetTest();
    }

    MOZ_ALWAYS_INLINE void resetForNewBuffer() {
        initPage = (uintptr_t(begin() - 1) >> pageShift) + 1;
        currPage = (uintptr_t(begin() + length()) >> pageShift);
        lastPage = (uintptr_t(begin() + capacity()) >> pageShift) - 1;
        protectUsedEnabled = ProtectUsed && usable && enabled && initPage <= lastPage &&
                             (uintptr_t(begin()) & elemMask) == 0 && capacity() >= lowerBound;
        protectUnusedEnabled = ProtectUnused && usable && enabled && initPage <= lastPage &&
                               (uintptr_t(begin()) & elemMask) == 0 && capacity() >= lowerBound;
        setTestInitial();
    }

    MOZ_ALWAYS_INLINE void poisonNewBuffer() {
        if (!PoisonUnused)
            return;
        T* addr = begin() + length();
        size_t toPoison = (capacity() - length()) * sizeof(T);
        memset(addr, PoisonPattern, toPoison);
    }

    MOZ_ALWAYS_INLINE void addExceptionHandler() {
        if (MOZ_UNLIKELY(protectUsedEnabled || protectUnusedEnabled))
            MemoryProtectionExceptionHandler::addRegion(begin(), capacity() << elemShift);
    }

    MOZ_ALWAYS_INLINE void removeExceptionHandler() {
        if (MOZ_UNLIKELY(protectUsedEnabled || protectUnusedEnabled))
            MemoryProtectionExceptionHandler::removeRegion(begin());
    }

    MOZ_ALWAYS_INLINE void protectUsed() {
        if (MOZ_LIKELY(!protectUsedEnabled))
            return;
        if (MOZ_UNLIKELY(currPage <= initPage))
            return;
        T* addr = reinterpret_cast<T*>(initPage << pageShift);
        size_t size = (currPage - initPage) << pageShift;
        gc::MakePagesReadOnly(addr, size);
    }

    MOZ_ALWAYS_INLINE void unprotectUsed() {
        if (MOZ_LIKELY(!protectUsedEnabled))
            return;
        if (MOZ_UNLIKELY(currPage <= initPage))
            return;
        T* addr = reinterpret_cast<T*>(initPage << pageShift);
        size_t size = (currPage - initPage) << pageShift;
        gc::UnprotectPages(addr, size);
    }

    MOZ_ALWAYS_INLINE void protectUnused() {
        if (MOZ_LIKELY(!protectUnusedEnabled))
            return;
        if (MOZ_UNLIKELY(currPage >= lastPage))
            return;
        T* addr = reinterpret_cast<T*>((currPage + 1) << pageShift);
        size_t size = (lastPage - currPage) << pageShift;
        gc::ProtectPages(addr, size);
    }

    MOZ_ALWAYS_INLINE void unprotectUnused() {
        if (MOZ_LIKELY(!protectUnusedEnabled))
            return;
        if (MOZ_UNLIKELY(currPage >= lastPage))
            return;
        T* addr = reinterpret_cast<T*>((currPage + 1) << pageShift);
        size_t size = (lastPage - currPage) << pageShift;
        gc::UnprotectPages(addr, size);
    }

    MOZ_ALWAYS_INLINE void protectNewBuffer() {
        resetForNewBuffer();
        addExceptionHandler();
        poisonNewBuffer();
        protectUsed();
        protectUnused();
    }

    MOZ_ALWAYS_INLINE void unprotectOldBuffer() {
        MOZ_ASSERT(!regionUnprotected);
        unprotectUnused();
        unprotectUsed();
        removeExceptionHandler();
    }

    MOZ_ALWAYS_INLINE void protectUnusedPartial(size_t curr, size_t next) {
        if (MOZ_LIKELY(!protectUnusedEnabled))
            return;
        if (MOZ_UNLIKELY(next > lastPage))
            --next;
        if (MOZ_UNLIKELY(next == curr))
            return;
        void* addr = reinterpret_cast<T*>((curr + 1) << pageShift);
        size_t size = (next - curr) << pageShift;
        gc::ProtectPages(addr, size);
    }

    MOZ_ALWAYS_INLINE void unprotectUnusedPartial(size_t curr, size_t next) {
        if (MOZ_LIKELY(!protectUnusedEnabled))
            return;
        if (MOZ_UNLIKELY(next > lastPage))
            --next;
        if (MOZ_UNLIKELY(next == curr))
            return;
        void* addr = reinterpret_cast<T*>((curr + 1) << pageShift);
        size_t size = (next - curr) << pageShift;
        gc::UnprotectPages(addr, size);
    }

    MOZ_ALWAYS_INLINE void protectUsedPartial(size_t curr, size_t next) {
        if (MOZ_LIKELY(!protectUsedEnabled))
            return;
        if (MOZ_UNLIKELY(curr < initPage))
            ++curr;
        if (MOZ_UNLIKELY(next == curr))
            return;
        void* addr = reinterpret_cast<T*>(curr << pageShift);
        size_t size = (next - curr) << pageShift;
        gc::MakePagesReadOnly(addr, size);
    }

    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool reserveNewBuffer(size_t size) {
        unprotectOldBuffer();
        bool ret = vector.reserve(size);
        protectNewBuffer();
        return ret;
    }

    template<typename U>
    MOZ_ALWAYS_INLINE void infallibleAppendNewPage(const U* values, size_t size) {
        size_t nextPage = uintptr_t(begin() + length() + size) >> pageShift;
        MOZ_ASSERT(currPage < nextPage);
        unprotectUnusedPartial(currPage, nextPage);
        vector.infallibleAppend(values, size);
        protectUsedPartial(currPage, nextPage);
        currPage = nextPage;
        resetTest();
    }

    template<typename U>
    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool appendNewPage(const U* values, size_t size) {
        size_t nextPage = uintptr_t(begin() + length() + size) >> pageShift;
        MOZ_ASSERT(currPage < nextPage);
        unprotectUnusedPartial(currPage, nextPage);
        bool ret = vector.append(values, size);
        if (MOZ_LIKELY(ret)) {
            protectUsedPartial(currPage, nextPage);
            currPage = nextPage;
        } else {
            protectUnusedPartial(currPage, nextPage);
        }
        resetTest();
        return ret;
    }

    template<typename U>
    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool appendNewBuffer(const U* values, size_t size) {
        unprotectOldBuffer();
        bool ret = vector.append(values, size);
        protectNewBuffer();
        return ret;
    }

    MOZ_NEVER_INLINE void unprotectRegionSlow(uintptr_t l, uintptr_t r);
    MOZ_NEVER_INLINE void reprotectRegionSlow(uintptr_t l, uintptr_t r);

    MOZ_NEVER_INLINE MOZ_MUST_USE bool reserveSlow(size_t size);

    template<typename U>
    MOZ_NEVER_INLINE void infallibleAppendSlow(const U* values, size_t size);

    template<typename U>
    MOZ_NEVER_INLINE MOZ_MUST_USE bool appendSlow(const U* values, size_t size);

  public:
    explicit PageProtectingVector(AllocPolicy policy = AllocPolicy())
      : vector(policy),
        elemsUntilTest(0),
        currPage(0),
        initPage(0),
        lastPage(0),
        lowerBound(InitialLowerBound),
#ifdef DEBUG
        regionUnprotected(false),
#endif
        usable(true),
        enabled(true),
        protectUsedEnabled(false),
        protectUnusedEnabled(false)
    {
        if (gc::SystemPageSize() != pageSize)
            usable = false;
        protectNewBuffer();
    }

    ~PageProtectingVector() { unprotectOldBuffer(); }

    void disableProtection() {
        MOZ_ASSERT(enabled);
        unprotectOldBuffer();
        enabled = false;
        resetForNewBuffer();
    }

    void enableProtection() {
        MOZ_ASSERT(!enabled);
        enabled = true;
        protectNewBuffer();
    }

    /*
     * Sets the lower bound on the size, in elems, that this vector's underlying
     * capacity has to be before its used pages will be protected.
     */
    void setLowerBoundForProtection(size_t elems) {
        if (lowerBound != elems) {
            unprotectOldBuffer();
            lowerBound = elems;
            protectNewBuffer();
        }
    }

    /* Disable protection on the smallest containing region. */
    MOZ_ALWAYS_INLINE void unprotectRegion(T* first, size_t size) {
#ifdef DEBUG
        regionUnprotected = true;
#endif
        if (MOZ_UNLIKELY(protectUsedEnabled)) {
            uintptr_t l = uintptr_t(first) >> pageShift;
            uintptr_t r = uintptr_t(first + size - 1) >> pageShift;
            if (r >= initPage && l < currPage)
                unprotectRegionSlow(l, r);
        }
    }

    /* Re-enable protection on the smallest containing region. */
    MOZ_ALWAYS_INLINE void reprotectRegion(T* first, size_t size) {
#ifdef DEBUG
        regionUnprotected = false;
#endif
        if (MOZ_UNLIKELY(protectUsedEnabled)) {
            uintptr_t l = uintptr_t(first) >> pageShift;
            uintptr_t r = uintptr_t(first + size - 1) >> pageShift;
            if (r >= initPage && l < currPage)
                reprotectRegionSlow(l, r);
        }
    }

    MOZ_ALWAYS_INLINE size_t capacity() const { return vector.capacity(); }
    MOZ_ALWAYS_INLINE size_t length() const { return vector.length(); }

    MOZ_ALWAYS_INLINE T* begin() { return vector.begin(); }
    MOZ_ALWAYS_INLINE const T* begin() const { return vector.begin(); }

    void clear() {
        unprotectOldBuffer();
        vector.clear();
        protectNewBuffer();
    }

    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool reserve(size_t size) {
        if (MOZ_LIKELY(size <= capacity()))
            return vector.reserve(size);
        return reserveSlow(size);
    }

    template<typename U>
    MOZ_ALWAYS_INLINE void infallibleAppend(const U* values, size_t size) {
        elemsUntilTest -= size;
        if (MOZ_LIKELY(elemsUntilTest >= 0))
            return vector.infallibleAppend(values, size);
        infallibleAppendSlow(values, size);
    }

    template<typename U>
    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool append(const U* values, size_t size) {
        elemsUntilTest -= size;
        if (MOZ_LIKELY(elemsUntilTest >= 0))
            return vector.append(values, size);
        return appendSlow(values, size);
    }
};

template<typename T, size_t A, class B, bool C, bool D, size_t E, bool F, uint8_t G>
MOZ_NEVER_INLINE void
PageProtectingVector<T, A, B, C, D, E, F, G>::unprotectRegionSlow(uintptr_t l, uintptr_t r)
{
    if (l < initPage)
        l = initPage;
    if (r >= currPage)
        r = currPage - 1;
    T* addr = reinterpret_cast<T*>(l << pageShift);
    size_t size = (r - l + 1) << pageShift;
    gc::UnprotectPages(addr, size);
}

template<typename T, size_t A, class B, bool C, bool D, size_t E, bool F, uint8_t G>
MOZ_NEVER_INLINE void
PageProtectingVector<T, A, B, C, D, E, F, G>::reprotectRegionSlow(uintptr_t l, uintptr_t r)
{
    if (l < initPage)
        l = initPage;
    if (r >= currPage)
        r = currPage - 1;
    T* addr = reinterpret_cast<T*>(l << pageShift);
    size_t size = (r - l + 1) << pageShift;
    gc::MakePagesReadOnly(addr, size);
}

template<typename T, size_t A, class B, bool C, bool D, size_t E, bool F, uint8_t G>
MOZ_NEVER_INLINE MOZ_MUST_USE bool
PageProtectingVector<T, A, B, C, D, E, F, G>::reserveSlow(size_t size)
{
    return reserveNewBuffer(size);
}

template<typename T, size_t A, class B, bool C, bool D, size_t E, bool F, uint8_t G>
template<typename U>
MOZ_NEVER_INLINE void
PageProtectingVector<T, A, B, C, D, E, F, G>::infallibleAppendSlow(const U* values, size_t size)
{
    // Ensure that we're here because we reached a page
    // boundary and not because of a buffer overflow.
    MOZ_RELEASE_ASSERT(MOZ_LIKELY(length() + size <= capacity()),
                       "About to overflow our AssemblerBuffer using infallibleAppend!");
    infallibleAppendNewPage(values, size);
}

template<typename T, size_t A, class B, bool C, bool D, size_t E, bool F, uint8_t G>
template<typename U>
MOZ_NEVER_INLINE MOZ_MUST_USE bool
PageProtectingVector<T, A, B, C, D, E, F, G>::appendSlow(const U* values, size_t size)
{
    if (MOZ_LIKELY(length() + size <= capacity()))
        return appendNewPage(values, size);
    return appendNewBuffer(values, size);
}

} /* namespace js */

#endif /* ds_PageProtectingVector_h */

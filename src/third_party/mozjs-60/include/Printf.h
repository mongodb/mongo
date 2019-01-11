/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Printf-like functions, with canned variants that malloc their result.  */

#ifndef mozilla_Printf_h
#define mozilla_Printf_h

/*
** API for PR printf like routines.
**
** These exist partly for historical reasons -- initially they were in
** NSPR, then forked in tree and modified in js/ -- but now the prime
** motivation is both closer control over the exact formatting (with
** one exception, see below) and also the ability to control where
** exactly the generated results are sent.
**
** It might seem that this could all be dispensed with in favor of a
** wrapper around |vsnprintf| -- except that this implementation
** guarantees that the %s format will accept a NULL pointer, whereas
** with standard functions this is undefined.
**
** This supports the following formats.  It implements a subset of the
** standard formats; due to the use of MOZ_FORMAT_PRINTF, it is not
** permissible to extend the standard, aside from relaxing undefined
** behavior.
**
**      %d - decimal
**      %u - unsigned decimal
**      %x - unsigned hex
**      %X - unsigned uppercase hex
**      %o - unsigned octal
**      %hd, %hu, %hx, %hX, %ho - "short" versions of above
**      %ld, %lu, %lx, %lX, %lo - "long" versions of above
**      %lld, %llu, %llx, %llX, %llo - "long long" versions of above
**      %zd, %zo, %zu, %zx, %zX - size_t versions of above
**      %Id, %Io, %Iu, %Ix, %IX - size_t versions of above (for Windows compat).
**           Note that MSVC 2015 and newer supports the z length modifier so
**           users should prefer using %z instead of %I. We are supporting %I in
**           addition to %z in case third-party code that uses %I gets routed to
**           use this printf implementation.
**      %s - string
**      %S, %ls - wide string, that is wchar_t*
**      %c - character
**      %p - pointer (deals with machine dependent pointer size)
**      %f - float; note that this is actually formatted using the
**           system's native printf, and so the results may vary
**      %g - float; note that this is actually formatted using the
**           system's native printf, and so the results may vary
*/

#include "mozilla/AllocPolicy.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Types.h"
#include "mozilla/UniquePtr.h"

#include <stdarg.h>
#include <string.h>

namespace mozilla {

/*
 * This class may be subclassed to provide a way to get the output of
 * a printf-like call, as the output is generated.
 */
class PrintfTarget
{
public:
    /* The Printf-like interface.  */
    bool MFBT_API print(const char* format, ...) MOZ_FORMAT_PRINTF(2, 3);

    /* The Vprintf-like interface.  */
    bool MFBT_API vprint(const char* format, va_list) MOZ_FORMAT_PRINTF(2, 0);

protected:
    MFBT_API PrintfTarget();
    virtual ~PrintfTarget() { }

    /* Subclasses override this.  It is called when more output is
       available.  It may be called with len==0.  This should return
       true on success, or false on failure.  */
    virtual bool append(const char* sp, size_t len) = 0;

private:

    /* Number of bytes emitted so far.  */
    size_t mEmitted;

    /* The implementation calls this to emit bytes and update
       mEmitted.  */
    bool emit(const char* sp, size_t len) {
        mEmitted += len;
        return append(sp, len);
    }

    bool fill2(const char* src, int srclen, int width, int flags);
    bool fill_n(const char* src, int srclen, int width, int prec, int type, int flags);
    bool cvt_l(long num, int width, int prec, int radix, int type, int flags, const char* hxp);
    bool cvt_ll(int64_t num, int width, int prec, int radix, int type, int flags, const char* hexp);
    bool cvt_f(double d, const char* fmt0, const char* fmt1);
    bool cvt_s(const char* s, int width, int prec, int flags);
};

namespace detail {

template<typename AllocPolicy = mozilla::MallocAllocPolicy>
struct AllocPolicyBasedFreePolicy
{
  void operator()(const void* ptr) {
    AllocPolicy policy;
    policy.free_(const_cast<void*>(ptr));
  }
};

}

// The type returned by Smprintf and friends.
template<typename AllocPolicy>
using SmprintfPolicyPointer = mozilla::UniquePtr<char, detail::AllocPolicyBasedFreePolicy<AllocPolicy>>;

// The default type if no alloc policy is specified.
typedef SmprintfPolicyPointer<mozilla::MallocAllocPolicy> SmprintfPointer;

// Used in the implementation of Smprintf et al.
template<typename AllocPolicy>
class MOZ_STACK_CLASS SprintfState final : private mozilla::PrintfTarget, private AllocPolicy
{
 public:
    explicit SprintfState(char* base)
        : mMaxlen(base ? strlen(base) : 0)
        , mBase(base)
        , mCur(base ? base + mMaxlen : 0)
    {
    }

    ~SprintfState() {
        this->free_(mBase);
    }

    bool vprint(const char* format, va_list ap_list) MOZ_FORMAT_PRINTF(2, 0) {
        // The "" here has a single \0 character, which is what we're
        // trying to append.
        return mozilla::PrintfTarget::vprint(format, ap_list) && append("", 1);
    }

    SmprintfPolicyPointer<AllocPolicy> release() {
        SmprintfPolicyPointer<AllocPolicy> result(mBase);
        mBase = nullptr;
        return result;
    }

 protected:

    bool append(const char* sp, size_t len) override {
        ptrdiff_t off;
        char* newbase;
        size_t newlen;

        off = mCur - mBase;
        if (off + len >= mMaxlen) {
            /* Grow the buffer */
            newlen = mMaxlen + ((len > 32) ? len : 32);
            newbase = static_cast<char*>(this->maybe_pod_realloc(mBase, mMaxlen, newlen));
            if (!newbase) {
                /* Ran out of memory */
                return false;
            }
            mBase = newbase;
            mMaxlen = newlen;
            mCur = mBase + off;
        }

        /* Copy data */
        memcpy(mCur, sp, len);
        mCur += len;
        MOZ_ASSERT(size_t(mCur - mBase) <= mMaxlen);
        return true;
    }

 private:

    size_t mMaxlen;
    char* mBase;
    char* mCur;
};

/*
** sprintf into a malloc'd buffer. Return a pointer to the malloc'd
** buffer on success, nullptr on failure. Call AllocPolicy::free_ to release
** the memory returned.
*/
template<typename AllocPolicy = mozilla::MallocAllocPolicy>
MOZ_FORMAT_PRINTF(1, 2)
SmprintfPolicyPointer<AllocPolicy> Smprintf(const char* fmt, ...)
{
    SprintfState<AllocPolicy> ss(nullptr);
    va_list ap;
    va_start(ap, fmt);
    bool r = ss.vprint(fmt, ap);
    va_end(ap);
    if (!r) {
        return nullptr;
    }
    return ss.release();
}

/*
** "append" sprintf into a malloc'd buffer. "last" is the last value of
** the malloc'd buffer. sprintf will append data to the end of last,
** growing it as necessary using realloc. If last is nullptr, SmprintfAppend
** will allocate the initial string. The return value is the new value of
** last for subsequent calls, or nullptr if there is a malloc failure.
*/
template<typename AllocPolicy = mozilla::MallocAllocPolicy>
MOZ_FORMAT_PRINTF(2, 3)
SmprintfPolicyPointer<AllocPolicy> SmprintfAppend(SmprintfPolicyPointer<AllocPolicy>&& last,
                                                  const char* fmt, ...)
{
    SprintfState<AllocPolicy> ss(last.release());
    va_list ap;
    va_start(ap, fmt);
    bool r = ss.vprint(fmt, ap);
    va_end(ap);
    if (!r) {
        return nullptr;
    }
    return ss.release();
}

/*
** va_list forms of the above.
*/
template<typename AllocPolicy = mozilla::MallocAllocPolicy>
MOZ_FORMAT_PRINTF(1, 0)
SmprintfPolicyPointer<AllocPolicy> Vsmprintf(const char* fmt, va_list ap)
{
    SprintfState<AllocPolicy> ss(nullptr);
    if (!ss.vprint(fmt, ap))
        return nullptr;
    return ss.release();
}

template<typename AllocPolicy = mozilla::MallocAllocPolicy>
MOZ_FORMAT_PRINTF(2, 0)
SmprintfPolicyPointer<AllocPolicy> VsmprintfAppend(SmprintfPolicyPointer<AllocPolicy>&& last,
                                                   const char* fmt, va_list ap)
{
    SprintfState<AllocPolicy> ss(last.release());
    if (!ss.vprint(fmt, ap))
        return nullptr;
    return ss.release();
}

} // namespace mozilla

#endif /* mozilla_Printf_h */

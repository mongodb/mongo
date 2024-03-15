/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MmapFaultHandler_h_
#define MmapFaultHandler_h_

#if defined(XP_WIN)
// Windows

#  ifdef HAVE_SEH_EXCEPTIONS
#    define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd) __try {
#    define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen) __try {
#    define MMAP_FAULT_HANDLER_CATCH(retval)                  \
      }                                                       \
      __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR \
                    ? EXCEPTION_EXECUTE_HANDLER               \
                    : EXCEPTION_CONTINUE_SEARCH) {            \
        NS_WARNING("unexpected EXCEPTION_IN_PAGE_ERROR");     \
        return retval;                                        \
      }
#  else
#    define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd) {
#    define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen) {
#    define MMAP_FAULT_HANDLER_CATCH(retval) }
#  endif

#elif defined(XP_DARWIN)
// MacOS

#  define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd) {
#  define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen) {
#  define MMAP_FAULT_HANDLER_CATCH(retval) }

#elif defined(__wasi__)

#  define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd) {
#  define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen) {
#  define MMAP_FAULT_HANDLER_CATCH(retval) }

#else
// Linux

#  include "mozilla/Attributes.h"
#  include "mozilla/Types.h"
#  include <stdint.h>
#  include <setjmp.h>

class MOZ_RAII MmapAccessScope {
 public:
  MFBT_API MmapAccessScope(void* aBuf, uint32_t aBufLen,
                           const char* aFilename = nullptr);
  MFBT_API ~MmapAccessScope();

  MmapAccessScope(const MmapAccessScope&) = delete;
  MmapAccessScope& operator=(const MmapAccessScope&) = delete;

  void SetThreadLocalScope();
  bool IsInsideBuffer(void* aPtr);
  void CrashWithInfo(void* aPtr);

  // sigsetjmp cannot be called from a method that returns before calling
  // siglongjmp, so the macro must call sigsetjmp directly and mJmpBuf must be
  // public.
  sigjmp_buf mJmpBuf;

 private:
  void* mBuf;
  const char* mFilename;
  uint32_t mBufLen;
  MmapAccessScope* mPreviousScope;
};

// Gets around warnings for null-checking in a macro.
template <typename T>
inline bool ValidFD(T fd) {
  return !!fd;
}

#  define MMAP_FAULT_HANDLER_BEGIN_HANDLE(fd)                  \
    {                                                          \
      void* mmapScopeBuf = nullptr;                            \
      nsCString mmapScopeFilename;                             \
      uint32_t mmapScopeBufLen = 0;                            \
      if (ValidFD(fd) && fd->mMap) {                           \
        mmapScopeBuf = (void*)fd->mFileStart;                  \
        mmapScopeBufLen = fd->mTotalLen;                       \
      }                                                        \
      if (ValidFD(fd) && fd->mFile) {                          \
        nsCOMPtr<nsIFile> file = fd->mFile.GetBaseFile();      \
        if (file) {                                            \
          file->GetNativeLeafName(mmapScopeFilename);          \
        }                                                      \
      }                                                        \
      MmapAccessScope mmapScope(mmapScopeBuf, mmapScopeBufLen, \
                                mmapScopeFilename.get());      \
      if (sigsetjmp(mmapScope.mJmpBuf, 0) == 0) {
#  define MMAP_FAULT_HANDLER_BEGIN_BUFFER(buf, bufLen)   \
    {                                                    \
      MmapAccessScope mmapScope((void*)(buf), (bufLen)); \
      if (sigsetjmp(mmapScope.mJmpBuf, 0) == 0) {
#  define MMAP_FAULT_HANDLER_CATCH(retval)                       \
    }                                                            \
    else {                                                       \
      NS_WARNING("SIGBUS received when accessing mmapped file"); \
      return retval;                                             \
    }                                                            \
    }

#endif

#endif

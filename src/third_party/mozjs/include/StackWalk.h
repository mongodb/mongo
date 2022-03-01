/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* APIs for getting a stack trace of the current thread */

#ifndef mozilla_StackWalk_h
#define mozilla_StackWalk_h

#include "mozilla/Types.h"
#include <stdint.h>
#include <stdio.h>

#ifdef _MSC_VER
#include <intrin.h>  // For _ReturnAddress
#endif

MOZ_BEGIN_EXTERN_C

/**
 * Returns the position of the Program Counter for the caller of the current
 * function. This is meant to be used to feed the aFirstFramePC argument to
 * MozStackWalk or MozWalkTheStack*, and should be used in the last function
 * that should be skipped in the trace, and passed down to MozStackWalk or
 * MozWalkTheStack*, through any intermediaries.
 *
 * THIS DOES NOT 100% RELIABLY GIVE THE CALLER PC, but marking functions
 * calling this macro with MOZ_NEVER_INLINE gets us close. In cases it doesn't
 * give the caller's PC, it may give the caller of the caller, or its caller,
 * etc. depending on tail call optimization.
 *
 * Past versions of stackwalking relied on passing a constant number of frames
 * to skip to MozStackWalk or MozWalkTheStack, which fell short in more cases
 * (inlining of intermediaries, tail call optimization).
 */
#ifndef _MSC_VER
# define CallerPC() __builtin_extract_return_addr(__builtin_return_address(0))
#else
# define CallerPC() _ReturnAddress()
#endif

/**
 * The callback for MozStackWalk and MozStackWalkThread.
 *
 * @param aFrameNumber  The frame number (starts at 1, not 0).
 * @param aPC           The program counter value.
 * @param aSP           The best approximation possible of what the stack
 *                      pointer will be pointing to when the execution returns
 *                      to executing that at aPC. If no approximation can
 *                      be made it will be nullptr.
 * @param aClosure      Extra data passed in from MozStackWalk() or
 *                      MozStackWalkThread().
 */
typedef void (*MozWalkStackCallback)(uint32_t aFrameNumber, void* aPC,
                                     void* aSP, void* aClosure);

/**
 * Call aCallback for each stack frame on the current thread, from
 * the caller of MozStackWalk to main (or above).
 *
 * @param aCallback     Callback function, called once per frame.
 * @param aFirstFramePC Position of the Program Counter where the trace
 *                      starts from. All frames seen before reaching that
 *                      address are skipped. Nullptr means that the first
 *                      callback will be for the caller of MozStackWalk.
 * @param aMaxFrames    Maximum number of frames to trace.  0 means no limit.
 * @param aClosure      Caller-supplied data passed through to aCallback.
 *
 * May skip some stack frames due to compiler optimizations or code
 * generation.
 */
MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure);

typedef struct {
  /*
   * The name of the shared library or executable containing an
   * address and the address's offset within that library, or empty
   * string and zero if unknown.
   */
  char library[256];
  ptrdiff_t loffset;
  /*
   * The name of the file name and line number of the code
   * corresponding to the address, or empty string and zero if
   * unknown.
   */
  char filename[256];
  unsigned long lineno;
  /*
   * The name of the function containing an address and the address's
   * offset within that function, or empty string and zero if unknown.
   */
  char function[256];
  ptrdiff_t foffset;
} MozCodeAddressDetails;

/**
 * For a given pointer to code, fill in the pieces of information used
 * when printing a stack trace.
 *
 * @param aPC         The code address.
 * @param aDetails    A structure to be filled in with the result.
 */
MFBT_API bool MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails);

/**
 * Format the information about a code address in a format suitable for
 * stack traces on the current platform.  When available, this string
 * should contain the function name, source file, and line number.  When
 * these are not available, library and offset should be reported, if
 * possible.
 *
 * Note that this output is parsed by several scripts including the fix*.py and
 * make-tree.pl scripts in tools/rb/. It should only be change with care, and
 * in conjunction with those scripts.
 *
 * @param aBuffer      A string to be filled in with the description.
 *                     The string will always be null-terminated.
 * @param aBufferSize  The size, in bytes, of aBuffer, including
 *                     room for the terminating null.  If the information
 *                     to be printed would be larger than aBuffer, it
 *                     will be truncated so that aBuffer[aBufferSize-1]
 *                     is the terminating null.
 * @param aFrameNumber The frame number.
 * @param aPC          The code address.
 * @param aFunction    The function name. Possibly null or the empty string.
 * @param aLibrary     The library name. Possibly null or the empty string.
 * @param aLOffset     The library offset.
 * @param aFileName    The filename. Possibly null or the empty string.
 * @param aLineNo      The line number. Possibly zero.
 * @return             The minimum number of characters necessary to format
 *                     the frame information, without the terminating null.
 *                     The buffer will have been truncated if the returned
 *                     value is greater than aBufferSize-1.
 */
MFBT_API int MozFormatCodeAddress(char* aBuffer, uint32_t aBufferSize,
                                  uint32_t aFrameNumber, const void* aPC,
                                  const char* aFunction, const char* aLibrary,
                                  ptrdiff_t aLOffset, const char* aFileName,
                                  uint32_t aLineNo);

/**
 * Format the information about a code address in the same fashion as
 * MozFormatCodeAddress.
 *
 * @param aBuffer      A string to be filled in with the description.
 *                     The string will always be null-terminated.
 * @param aBufferSize  The size, in bytes, of aBuffer, including
 *                     room for the terminating null.  If the information
 *                     to be printed would be larger than aBuffer, it
 *                     will be truncated so that aBuffer[aBufferSize-1]
 *                     is the terminating null.
 * @param aFrameNumber The frame number.
 * @param aPC          The code address.
 * @param aDetails     The value filled in by MozDescribeCodeAddress(aPC).
 * @return             The minimum number of characters necessary to format
 *                     the frame information, without the terminating null.
 *                     The buffer will have been truncated if the returned
 *                     value is greater than aBufferSize-1.
 */
MFBT_API int MozFormatCodeAddressDetails(char* aBuffer, uint32_t aBufferSize,
                                         uint32_t aFrameNumber, void* aPC,
                                         const MozCodeAddressDetails* aDetails);

#ifdef __cplusplus
#  define FRAMES_DEFAULT = 0
#else
#  define FRAMES_DEFAULT
#endif
/**
 * Walk the stack and print the stack trace to the given stream.
 *
 * @param aStream       A stdio stream.
 * @param aFirstFramePC Position of the Program Counter where the trace
 *                      starts from. All frames seen before reaching that
 *                      address are skipped. Nullptr means that the first
 *                      callback will be for the caller of MozWalkTheStack.
 * @param aMaxFrames    Maximum number of frames to trace.  0 means no limit.
 */
MFBT_API void MozWalkTheStack(FILE* aStream,
                              const void* aFirstFramePC FRAMES_DEFAULT,
                              uint32_t aMaxFrames FRAMES_DEFAULT);

/**
 * Walk the stack and send each stack trace line to a callback writer.
 * Each line string is null terminated but doesn't contain a '\n' character.
 *
 * @param aWriter       The callback.
 * @param aFirstFramePC Position of the Program Counter where the trace
 *                      starts from. All frames seen before reaching that
 *                      address are skipped. Nullptr means that the first
 *                      callback will be for the caller of
 * MozWalkTheStackWithWriter.
 * @param aMaxFrames    Maximum number of frames to trace.  0 means no limit.
 */
MFBT_API void MozWalkTheStackWithWriter(
    void (*aWriter)(const char*), const void* aFirstFramePC FRAMES_DEFAULT,
    uint32_t aMaxFrames FRAMES_DEFAULT);

#undef FRAMES_DEFAULT

MOZ_END_EXTERN_C

#ifdef __cplusplus
namespace mozilla {

MFBT_API void FramePointerStackWalk(MozWalkStackCallback aCallback,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd);

#  if defined(XP_LINUX) || defined(XP_FREEBSD)
MFBT_API void DemangleSymbol(const char* aSymbol, char* aBuffer, int aBufLen);
#  endif

}  // namespace mozilla
#endif

#endif

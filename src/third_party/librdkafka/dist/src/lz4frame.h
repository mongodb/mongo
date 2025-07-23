/*
   KLZ4 auto-framing library
   Header File
   Copyright (C) 2011-2017, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - KLZ4 source repository : https://github.com/lz4/lz4
   - KLZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/* KLZ4F is a stand-alone API able to create and decode KLZ4 frames
 * conformant with specification v1.6.1 in doc/lz4_Frame_format.md .
 * Generated frames are compatible with `lz4` CLI.
 *
 * KLZ4F also offers streaming capabilities.
 *
 * lz4.h is not required when using lz4frame.h,
 * except to extract common constant such as KLZ4_VERSION_NUMBER.
 * */

#ifndef KLZ4F_H_09782039843
#define KLZ4F_H_09782039843

#if defined (__cplusplus)
extern "C" {
#endif

/* ---   Dependency   --- */
#include <stddef.h>   /* size_t */


/**
  Introduction

  lz4frame.h implements KLZ4 frame specification (doc/lz4_Frame_format.md).
  lz4frame.h provides frame compression functions that take care
  of encoding standard metadata alongside KLZ4-compressed blocks.
*/

/*-***************************************************************
 *  Compiler specifics
 *****************************************************************/
/*  KLZ4_DLL_EXPORT :
 *  Enable exporting of functions when building a Windows DLL
 *  KLZ4FLIB_VISIBILITY :
 *  Control library symbols visibility.
 */
#ifndef KLZ4FLIB_VISIBILITY
#  if defined(__GNUC__) && (__GNUC__ >= 4)
#    define KLZ4FLIB_VISIBILITY __attribute__ ((visibility ("default")))
#  else
#    define KLZ4FLIB_VISIBILITY
#  endif
#endif
#if defined(KLZ4_DLL_EXPORT) && (KLZ4_DLL_EXPORT==1)
#  define KLZ4FLIB_API __declspec(dllexport) KLZ4FLIB_VISIBILITY
#elif defined(KLZ4_DLL_IMPORT) && (KLZ4_DLL_IMPORT==1)
#  define KLZ4FLIB_API __declspec(dllimport) KLZ4FLIB_VISIBILITY
#else
#  define KLZ4FLIB_API KLZ4FLIB_VISIBILITY
#endif

#ifdef KLZ4F_DISABLE_DEPRECATE_WARNINGS
#  define KLZ4F_DEPRECATE(x) x
#else
#  if defined(_MSC_VER)
#    define KLZ4F_DEPRECATE(x) x   /* __declspec(deprecated) x - only works with C++ */
#  elif defined(__clang__) || (defined(__GNUC__) && (__GNUC__ >= 6))
#    define KLZ4F_DEPRECATE(x) x __attribute__((deprecated))
#  else
#    define KLZ4F_DEPRECATE(x) x   /* no deprecation warning for this compiler */
#  endif
#endif


/*-************************************
 *  Error management
 **************************************/
typedef size_t KLZ4F_errorCode_t;

KLZ4FLIB_API unsigned    KLZ4F_isError(KLZ4F_errorCode_t code);   /**< tells when a function result is an error code */
KLZ4FLIB_API const char* KLZ4F_getErrorName(KLZ4F_errorCode_t code);   /**< return error code string; for debugging */


/*-************************************
 *  Frame compression types
 ************************************* */
/* #define KLZ4F_ENABLE_OBSOLETE_ENUMS   // uncomment to enable obsolete enums */
#ifdef KLZ4F_ENABLE_OBSOLETE_ENUMS
#  define KLZ4F_OBSOLETE_ENUM(x) , KLZ4F_DEPRECATE(x) = KLZ4F_##x
#else
#  define KLZ4F_OBSOLETE_ENUM(x)
#endif

/* The larger the block size, the (slightly) better the compression ratio,
 * though there are diminishing returns.
 * Larger blocks also increase memory usage on both compression and decompression sides.
 */
typedef enum {
    KLZ4F_default=0,
    KLZ4F_max64KB=4,
    KLZ4F_max256KB=5,
    KLZ4F_max1MB=6,
    KLZ4F_max4MB=7
    KLZ4F_OBSOLETE_ENUM(max64KB)
    KLZ4F_OBSOLETE_ENUM(max256KB)
    KLZ4F_OBSOLETE_ENUM(max1MB)
    KLZ4F_OBSOLETE_ENUM(max4MB)
} KLZ4F_blockSizeID_t;

/* Linked blocks sharply reduce inefficiencies when using small blocks,
 * they compress better.
 * However, some KLZ4 decoders are only compatible with independent blocks */
typedef enum {
    KLZ4F_blockLinked=0,
    KLZ4F_blockIndependent
    KLZ4F_OBSOLETE_ENUM(blockLinked)
    KLZ4F_OBSOLETE_ENUM(blockIndependent)
} KLZ4F_blockMode_t;

typedef enum {
    KLZ4F_noContentChecksum=0,
    KLZ4F_contentChecksumEnabled
    KLZ4F_OBSOLETE_ENUM(noContentChecksum)
    KLZ4F_OBSOLETE_ENUM(contentChecksumEnabled)
} KLZ4F_contentChecksum_t;

typedef enum {
    KLZ4F_noBlockChecksum=0,
    KLZ4F_blockChecksumEnabled
} KLZ4F_blockChecksum_t;

typedef enum {
    KLZ4F_frame=0,
    KLZ4F_skippableFrame
    KLZ4F_OBSOLETE_ENUM(skippableFrame)
} KLZ4F_frameType_t;

#ifdef KLZ4F_ENABLE_OBSOLETE_ENUMS
typedef KLZ4F_blockSizeID_t blockSizeID_t;
typedef KLZ4F_blockMode_t blockMode_t;
typedef KLZ4F_frameType_t frameType_t;
typedef KLZ4F_contentChecksum_t contentChecksum_t;
#endif

/*! KLZ4F_frameInfo_t :
 *  makes it possible to set or read frame parameters.
 *  Structure must be first init to 0, using memset() or KLZ4F_INIT_FRAMEINFO,
 *  setting all parameters to default.
 *  It's then possible to update selectively some parameters */
typedef struct {
  KLZ4F_blockSizeID_t     blockSizeID;         /* max64KB, max256KB, max1MB, max4MB; 0 == default */
  KLZ4F_blockMode_t       blockMode;           /* KLZ4F_blockLinked, KLZ4F_blockIndependent; 0 == default */
  KLZ4F_contentChecksum_t contentChecksumFlag; /* 1: frame terminated with 32-bit checksum of decompressed data; 0: disabled (default) */
  KLZ4F_frameType_t       frameType;           /* read-only field : KLZ4F_frame or KLZ4F_skippableFrame */
  unsigned long long     contentSize;         /* Size of uncompressed content ; 0 == unknown */
  unsigned               dictID;              /* Dictionary ID, sent by compressor to help decoder select correct dictionary; 0 == no dictID provided */
  KLZ4F_blockChecksum_t   blockChecksumFlag;   /* 1: each block followed by a checksum of block's compressed data; 0: disabled (default) */
} KLZ4F_frameInfo_t;

#define KLZ4F_INIT_FRAMEINFO   { KLZ4F_default, KLZ4F_blockLinked, KLZ4F_noContentChecksum, KLZ4F_frame, 0ULL, 0U, KLZ4F_noBlockChecksum }    /* v1.8.3+ */

/*! KLZ4F_preferences_t :
 *  makes it possible to supply advanced compression instructions to streaming interface.
 *  Structure must be first init to 0, using memset() or KLZ4F_INIT_PREFERENCES,
 *  setting all parameters to default.
 *  All reserved fields must be set to zero. */
typedef struct {
  KLZ4F_frameInfo_t frameInfo;
  int      compressionLevel;    /* 0: default (fast mode); values > KLZ4HC_CLEVEL_MAX count as KLZ4HC_CLEVEL_MAX; values < 0 trigger "fast acceleration" */
  unsigned autoFlush;           /* 1: always flush; reduces usage of internal buffers */
  unsigned favorDecSpeed;       /* 1: parser favors decompression speed vs compression ratio. Only works for high compression modes (>= KLZ4HC_CLEVEL_OPT_MIN) */  /* v1.8.2+ */
  unsigned reserved[3];         /* must be zero for forward compatibility */
} KLZ4F_preferences_t;

#define KLZ4F_INIT_PREFERENCES   { KLZ4F_INIT_FRAMEINFO, 0, 0u, 0u, { 0u, 0u, 0u } }    /* v1.8.3+ */


/*-*********************************
*  Simple compression function
***********************************/

KLZ4FLIB_API int KLZ4F_compressionLevel_max(void);   /* v1.8.0+ */

/*! KLZ4F_compressFrameBound() :
 *  Returns the maximum possible compressed size with KLZ4F_compressFrame() given srcSize and preferences.
 * `preferencesPtr` is optional. It can be replaced by NULL, in which case, the function will assume default preferences.
 *  Note : this result is only usable with KLZ4F_compressFrame().
 *         It may also be used with KLZ4F_compressUpdate() _if no flush() operation_ is performed.
 */
KLZ4FLIB_API size_t KLZ4F_compressFrameBound(size_t srcSize, const KLZ4F_preferences_t* preferencesPtr);

/*! KLZ4F_compressFrame() :
 *  Compress an entire srcBuffer into a valid KLZ4 frame.
 *  dstCapacity MUST be >= KLZ4F_compressFrameBound(srcSize, preferencesPtr).
 *  The KLZ4F_preferences_t structure is optional : you can provide NULL as argument. All preferences will be set to default.
 * @return : number of bytes written into dstBuffer.
 *           or an error code if it fails (can be tested using KLZ4F_isError())
 */
KLZ4FLIB_API size_t KLZ4F_compressFrame(void* dstBuffer, size_t dstCapacity,
                                const void* srcBuffer, size_t srcSize,
                                const KLZ4F_preferences_t* preferencesPtr);


/*-***********************************
*  Advanced compression functions
*************************************/
typedef struct KLZ4F_cctx_s KLZ4F_cctx;   /* incomplete type */
typedef KLZ4F_cctx* KLZ4F_compressionContext_t;   /* for compatibility with previous API version */

typedef struct {
  unsigned stableSrc;    /* 1 == src content will remain present on future calls to KLZ4F_compress(); skip copying src content within tmp buffer */
  unsigned reserved[3];
} KLZ4F_compressOptions_t;

/*---   Resource Management   ---*/

#define KLZ4F_VERSION 100    /* This number can be used to check for an incompatible API breaking change */
KLZ4FLIB_API unsigned KLZ4F_getVersion(void);

/*! KLZ4F_createCompressionContext() :
 * The first thing to do is to create a compressionContext object, which will be used in all compression operations.
 * This is achieved using KLZ4F_createCompressionContext(), which takes as argument a version.
 * The version provided MUST be KLZ4F_VERSION. It is intended to track potential version mismatch, notably when using DLL.
 * The function will provide a pointer to a fully allocated KLZ4F_cctx object.
 * If @return != zero, there was an error during context creation.
 * Object can release its memory using KLZ4F_freeCompressionContext();
 */
KLZ4FLIB_API KLZ4F_errorCode_t KLZ4F_createCompressionContext(KLZ4F_cctx** cctxPtr, unsigned version);
KLZ4FLIB_API KLZ4F_errorCode_t KLZ4F_freeCompressionContext(KLZ4F_cctx* cctx);


/*----    Compression    ----*/

#define KLZ4F_HEADER_SIZE_MIN  7   /* KLZ4 Frame header size can vary, depending on selected paramaters */
#define KLZ4F_HEADER_SIZE_MAX 19

/* Size in bytes of a block header in little-endian format. Highest bit indicates if block data is uncompressed */
#define KLZ4F_BLOCK_HEADER_SIZE 4

/* Size in bytes of a block checksum footer in little-endian format. */
#define KLZ4F_BLOCK_CHECKSUM_SIZE 4

/* Size in bytes of the content checksum. */
#define KLZ4F_CONTENT_CHECKSUM_SIZE 4

/*! KLZ4F_compressBegin() :
 *  will write the frame header into dstBuffer.
 *  dstCapacity must be >= KLZ4F_HEADER_SIZE_MAX bytes.
 * `prefsPtr` is optional : you can provide NULL as argument, all preferences will then be set to default.
 * @return : number of bytes written into dstBuffer for the header
 *           or an error code (which can be tested using KLZ4F_isError())
 */
KLZ4FLIB_API size_t KLZ4F_compressBegin(KLZ4F_cctx* cctx,
                                      void* dstBuffer, size_t dstCapacity,
                                      const KLZ4F_preferences_t* prefsPtr);

/*! KLZ4F_compressBound() :
 *  Provides minimum dstCapacity required to guarantee success of
 *  KLZ4F_compressUpdate(), given a srcSize and preferences, for a worst case scenario.
 *  When srcSize==0, KLZ4F_compressBound() provides an upper bound for KLZ4F_flush() and KLZ4F_compressEnd() instead.
 *  Note that the result is only valid for a single invocation of KLZ4F_compressUpdate().
 *  When invoking KLZ4F_compressUpdate() multiple times,
 *  if the output buffer is gradually filled up instead of emptied and re-used from its start,
 *  one must check if there is enough remaining capacity before each invocation, using KLZ4F_compressBound().
 * @return is always the same for a srcSize and prefsPtr.
 *  prefsPtr is optional : when NULL is provided, preferences will be set to cover worst case scenario.
 *  tech details :
 * @return if automatic flushing is not enabled, includes the possibility that internal buffer might already be filled by up to (blockSize-1) bytes.
 *  It also includes frame footer (ending + checksum), since it might be generated by KLZ4F_compressEnd().
 * @return doesn't include frame header, as it was already generated by KLZ4F_compressBegin().
 */
KLZ4FLIB_API size_t KLZ4F_compressBound(size_t srcSize, const KLZ4F_preferences_t* prefsPtr);

/*! KLZ4F_compressUpdate() :
 *  KLZ4F_compressUpdate() can be called repetitively to compress as much data as necessary.
 *  Important rule: dstCapacity MUST be large enough to ensure operation success even in worst case situations.
 *  This value is provided by KLZ4F_compressBound().
 *  If this condition is not respected, KLZ4F_compress() will fail (result is an errorCode).
 *  KLZ4F_compressUpdate() doesn't guarantee error recovery.
 *  When an error occurs, compression context must be freed or resized.
 * `cOptPtr` is optional : NULL can be provided, in which case all options are set to default.
 * @return : number of bytes written into `dstBuffer` (it can be zero, meaning input data was just buffered).
 *           or an error code if it fails (which can be tested using KLZ4F_isError())
 */
KLZ4FLIB_API size_t KLZ4F_compressUpdate(KLZ4F_cctx* cctx,
                                       void* dstBuffer, size_t dstCapacity,
                                 const void* srcBuffer, size_t srcSize,
                                 const KLZ4F_compressOptions_t* cOptPtr);

/*! KLZ4F_flush() :
 *  When data must be generated and sent immediately, without waiting for a block to be completely filled,
 *  it's possible to call KLZ4_flush(). It will immediately compress any data buffered within cctx.
 * `dstCapacity` must be large enough to ensure the operation will be successful.
 * `cOptPtr` is optional : it's possible to provide NULL, all options will be set to default.
 * @return : nb of bytes written into dstBuffer (can be zero, when there is no data stored within cctx)
 *           or an error code if it fails (which can be tested using KLZ4F_isError())
 *  Note : KLZ4F_flush() is guaranteed to be successful when dstCapacity >= KLZ4F_compressBound(0, prefsPtr).
 */
KLZ4FLIB_API size_t KLZ4F_flush(KLZ4F_cctx* cctx,
                              void* dstBuffer, size_t dstCapacity,
                        const KLZ4F_compressOptions_t* cOptPtr);

/*! KLZ4F_compressEnd() :
 *  To properly finish an KLZ4 frame, invoke KLZ4F_compressEnd().
 *  It will flush whatever data remained within `cctx` (like KLZ4_flush())
 *  and properly finalize the frame, with an endMark and a checksum.
 * `cOptPtr` is optional : NULL can be provided, in which case all options will be set to default.
 * @return : nb of bytes written into dstBuffer, necessarily >= 4 (endMark),
 *           or an error code if it fails (which can be tested using KLZ4F_isError())
 *  Note : KLZ4F_compressEnd() is guaranteed to be successful when dstCapacity >= KLZ4F_compressBound(0, prefsPtr).
 *  A successful call to KLZ4F_compressEnd() makes `cctx` available again for another compression task.
 */
KLZ4FLIB_API size_t KLZ4F_compressEnd(KLZ4F_cctx* cctx,
                                    void* dstBuffer, size_t dstCapacity,
                              const KLZ4F_compressOptions_t* cOptPtr);


/*-*********************************
*  Decompression functions
***********************************/
typedef struct KLZ4F_dctx_s KLZ4F_dctx;   /* incomplete type */
typedef KLZ4F_dctx* KLZ4F_decompressionContext_t;   /* compatibility with previous API versions */

typedef struct {
  unsigned stableDst;    /* pledges that last 64KB decompressed data will remain available unmodified. This optimization skips storage operations in tmp buffers. */
  unsigned reserved[3];  /* must be set to zero for forward compatibility */
} KLZ4F_decompressOptions_t;


/* Resource management */

/*! KLZ4F_createDecompressionContext() :
 *  Create an KLZ4F_dctx object, to track all decompression operations.
 *  The version provided MUST be KLZ4F_VERSION.
 *  The function provides a pointer to an allocated and initialized KLZ4F_dctx object.
 *  The result is an errorCode, which can be tested using KLZ4F_isError().
 *  dctx memory can be released using KLZ4F_freeDecompressionContext();
 *  Result of KLZ4F_freeDecompressionContext() indicates current state of decompressionContext when being released.
 *  That is, it should be == 0 if decompression has been completed fully and correctly.
 */
KLZ4FLIB_API KLZ4F_errorCode_t KLZ4F_createDecompressionContext(KLZ4F_dctx** dctxPtr, unsigned version);
KLZ4FLIB_API KLZ4F_errorCode_t KLZ4F_freeDecompressionContext(KLZ4F_dctx* dctx);


/*-***********************************
*  Streaming decompression functions
*************************************/

#define KLZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH 5

/*! KLZ4F_headerSize() : v1.9.0+
 *  Provide the header size of a frame starting at `src`.
 * `srcSize` must be >= KLZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH,
 *  which is enough to decode the header length.
 * @return : size of frame header
 *           or an error code, which can be tested using KLZ4F_isError()
 *  note : Frame header size is variable, but is guaranteed to be
 *         >= KLZ4F_HEADER_SIZE_MIN bytes, and <= KLZ4F_HEADER_SIZE_MAX bytes.
 */
KLZ4FLIB_API size_t KLZ4F_headerSize(const void* src, size_t srcSize);

/*! KLZ4F_getFrameInfo() :
 *  This function extracts frame parameters (max blockSize, dictID, etc.).
 *  Its usage is optional: user can call KLZ4F_decompress() directly.
 *
 *  Extracted information will fill an existing KLZ4F_frameInfo_t structure.
 *  This can be useful for allocation and dictionary identification purposes.
 *
 *  KLZ4F_getFrameInfo() can work in the following situations :
 *
 *  1) At the beginning of a new frame, before any invocation of KLZ4F_decompress().
 *     It will decode header from `srcBuffer`,
 *     consuming the header and starting the decoding process.
 *
 *     Input size must be large enough to contain the full frame header.
 *     Frame header size can be known beforehand by KLZ4F_headerSize().
 *     Frame header size is variable, but is guaranteed to be >= KLZ4F_HEADER_SIZE_MIN bytes,
 *     and not more than <= KLZ4F_HEADER_SIZE_MAX bytes.
 *     Hence, blindly providing KLZ4F_HEADER_SIZE_MAX bytes or more will always work.
 *     It's allowed to provide more input data than the header size,
 *     KLZ4F_getFrameInfo() will only consume the header.
 *
 *     If input size is not large enough,
 *     aka if it's smaller than header size,
 *     function will fail and return an error code.
 *
 *  2) After decoding has been started,
 *     it's possible to invoke KLZ4F_getFrameInfo() anytime
 *     to extract already decoded frame parameters stored within dctx.
 *
 *     Note that, if decoding has barely started,
 *     and not yet read enough information to decode the header,
 *     KLZ4F_getFrameInfo() will fail.
 *
 *  The number of bytes consumed from srcBuffer will be updated in *srcSizePtr (necessarily <= original value).
 *  KLZ4F_getFrameInfo() only consumes bytes when decoding has not yet started,
 *  and when decoding the header has been successful.
 *  Decompression must then resume from (srcBuffer + *srcSizePtr).
 *
 * @return : a hint about how many srcSize bytes KLZ4F_decompress() expects for next call,
 *           or an error code which can be tested using KLZ4F_isError().
 *  note 1 : in case of error, dctx is not modified. Decoding operation can resume from beginning safely.
 *  note 2 : frame parameters are *copied into* an already allocated KLZ4F_frameInfo_t structure.
 */
KLZ4FLIB_API size_t KLZ4F_getFrameInfo(KLZ4F_dctx* dctx,
                                     KLZ4F_frameInfo_t* frameInfoPtr,
                                     const void* srcBuffer, size_t* srcSizePtr);

/*! KLZ4F_decompress() :
 *  Call this function repetitively to regenerate data compressed in `srcBuffer`.
 *
 *  The function requires a valid dctx state.
 *  It will read up to *srcSizePtr bytes from srcBuffer,
 *  and decompress data into dstBuffer, of capacity *dstSizePtr.
 *
 *  The nb of bytes consumed from srcBuffer will be written into *srcSizePtr (necessarily <= original value).
 *  The nb of bytes decompressed into dstBuffer will be written into *dstSizePtr (necessarily <= original value).
 *
 *  The function does not necessarily read all input bytes, so always check value in *srcSizePtr.
 *  Unconsumed source data must be presented again in subsequent invocations.
 *
 * `dstBuffer` can freely change between each consecutive function invocation.
 * `dstBuffer` content will be overwritten.
 *
 * @return : an hint of how many `srcSize` bytes KLZ4F_decompress() expects for next call.
 *  Schematically, it's the size of the current (or remaining) compressed block + header of next block.
 *  Respecting the hint provides some small speed benefit, because it skips intermediate buffers.
 *  This is just a hint though, it's always possible to provide any srcSize.
 *
 *  When a frame is fully decoded, @return will be 0 (no more data expected).
 *  When provided with more bytes than necessary to decode a frame,
 *  KLZ4F_decompress() will stop reading exactly at end of current frame, and @return 0.
 *
 *  If decompression failed, @return is an error code, which can be tested using KLZ4F_isError().
 *  After a decompression error, the `dctx` context is not resumable.
 *  Use KLZ4F_resetDecompressionContext() to return to clean state.
 *
 *  After a frame is fully decoded, dctx can be used again to decompress another frame.
 */
KLZ4FLIB_API size_t KLZ4F_decompress(KLZ4F_dctx* dctx,
                                   void* dstBuffer, size_t* dstSizePtr,
                                   const void* srcBuffer, size_t* srcSizePtr,
                                   const KLZ4F_decompressOptions_t* dOptPtr);


/*! KLZ4F_resetDecompressionContext() : added in v1.8.0
 *  In case of an error, the context is left in "undefined" state.
 *  In which case, it's necessary to reset it, before re-using it.
 *  This method can also be used to abruptly stop any unfinished decompression,
 *  and start a new one using same context resources. */
KLZ4FLIB_API void KLZ4F_resetDecompressionContext(KLZ4F_dctx* dctx);   /* always successful */



#if defined (__cplusplus)
}
#endif

#endif  /* KLZ4F_H_09782039843 */

#if defined(KLZ4F_STATIC_LINKING_ONLY) && !defined(KLZ4F_H_STATIC_09782039843)
#define KLZ4F_H_STATIC_09782039843

#if defined (__cplusplus)
extern "C" {
#endif

/* These declarations are not stable and may change in the future.
 * They are therefore only safe to depend on
 * when the caller is statically linked against the library.
 * To access their declarations, define KLZ4F_STATIC_LINKING_ONLY.
 *
 * By default, these symbols aren't published into shared/dynamic libraries.
 * You can override this behavior and force them to be published
 * by defining KLZ4F_PUBLISH_STATIC_FUNCTIONS.
 * Use at your own risk.
 */
#ifdef KLZ4F_PUBLISH_STATIC_FUNCTIONS
# define KLZ4FLIB_STATIC_API KLZ4FLIB_API
#else
# define KLZ4FLIB_STATIC_API
#endif


/* ---   Error List   --- */
#define KLZ4F_LIST_ERRORS(ITEM) \
        ITEM(OK_NoError) \
        ITEM(ERROR_GENERIC) \
        ITEM(ERROR_maxBlockSize_invalid) \
        ITEM(ERROR_blockMode_invalid) \
        ITEM(ERROR_contentChecksumFlag_invalid) \
        ITEM(ERROR_compressionLevel_invalid) \
        ITEM(ERROR_headerVersion_wrong) \
        ITEM(ERROR_blockChecksum_invalid) \
        ITEM(ERROR_reservedFlag_set) \
        ITEM(ERROR_allocation_failed) \
        ITEM(ERROR_srcSize_tooLarge) \
        ITEM(ERROR_dstMaxSize_tooSmall) \
        ITEM(ERROR_frameHeader_incomplete) \
        ITEM(ERROR_frameType_unknown) \
        ITEM(ERROR_frameSize_wrong) \
        ITEM(ERROR_srcPtr_wrong) \
        ITEM(ERROR_decompressionFailed) \
        ITEM(ERROR_headerChecksum_invalid) \
        ITEM(ERROR_contentChecksum_invalid) \
        ITEM(ERROR_frameDecoding_alreadyStarted) \
        ITEM(ERROR_maxCode)

#define KLZ4F_GENERATE_ENUM(ENUM) KLZ4F_##ENUM,

/* enum list is exposed, to handle specific errors */
typedef enum { KLZ4F_LIST_ERRORS(KLZ4F_GENERATE_ENUM)
              _KLZ4F_dummy_error_enum_for_c89_never_used } KLZ4F_errorCodes;

KLZ4FLIB_STATIC_API KLZ4F_errorCodes KLZ4F_getErrorCode(size_t functionResult);

KLZ4FLIB_STATIC_API size_t KLZ4F_getBlockSize(unsigned);

/**********************************
 *  Bulk processing dictionary API
 *********************************/

/* A Dictionary is useful for the compression of small messages (KB range).
 * It dramatically improves compression efficiency.
 *
 * KLZ4 can ingest any input as dictionary, though only the last 64 KB are useful.
 * Best results are generally achieved by using Zstandard's Dictionary Builder
 * to generate a high-quality dictionary from a set of samples.
 *
 * Loading a dictionary has a cost, since it involves construction of tables.
 * The Bulk processing dictionary API makes it possible to share this cost
 * over an arbitrary number of compression jobs, even concurrently,
 * markedly improving compression latency for these cases.
 *
 * The same dictionary will have to be used on the decompression side
 * for decoding to be successful.
 * To help identify the correct dictionary at decoding stage,
 * the frame header allows optional embedding of a dictID field.
 */
typedef struct KLZ4F_CDict_s KLZ4F_CDict;

/*! KLZ4_createCDict() :
 *  When compressing multiple messages / blocks using the same dictionary, it's recommended to load it just once.
 *  KLZ4_createCDict() will create a digested dictionary, ready to start future compression operations without startup delay.
 *  KLZ4_CDict can be created once and shared by multiple threads concurrently, since its usage is read-only.
 * `dictBuffer` can be released after KLZ4_CDict creation, since its content is copied within CDict */
KLZ4FLIB_STATIC_API KLZ4F_CDict* KLZ4F_createCDict(const void* dictBuffer, size_t dictSize);
KLZ4FLIB_STATIC_API void        KLZ4F_freeCDict(KLZ4F_CDict* CDict);


/*! KLZ4_compressFrame_usingCDict() :
 *  Compress an entire srcBuffer into a valid KLZ4 frame using a digested Dictionary.
 *  cctx must point to a context created by KLZ4F_createCompressionContext().
 *  If cdict==NULL, compress without a dictionary.
 *  dstBuffer MUST be >= KLZ4F_compressFrameBound(srcSize, preferencesPtr).
 *  If this condition is not respected, function will fail (@return an errorCode).
 *  The KLZ4F_preferences_t structure is optional : you may provide NULL as argument,
 *  but it's not recommended, as it's the only way to provide dictID in the frame header.
 * @return : number of bytes written into dstBuffer.
 *           or an error code if it fails (can be tested using KLZ4F_isError()) */
KLZ4FLIB_STATIC_API size_t KLZ4F_compressFrame_usingCDict(
    KLZ4F_cctx* cctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize,
    const KLZ4F_CDict* cdict,
    const KLZ4F_preferences_t* preferencesPtr);


/*! KLZ4F_compressBegin_usingCDict() :
 *  Inits streaming dictionary compression, and writes the frame header into dstBuffer.
 *  dstCapacity must be >= KLZ4F_HEADER_SIZE_MAX bytes.
 * `prefsPtr` is optional : you may provide NULL as argument,
 *  however, it's the only way to provide dictID in the frame header.
 * @return : number of bytes written into dstBuffer for the header,
 *           or an error code (which can be tested using KLZ4F_isError()) */
KLZ4FLIB_STATIC_API size_t KLZ4F_compressBegin_usingCDict(
    KLZ4F_cctx* cctx,
    void* dstBuffer, size_t dstCapacity,
    const KLZ4F_CDict* cdict,
    const KLZ4F_preferences_t* prefsPtr);


/*! KLZ4F_decompress_usingDict() :
 *  Same as KLZ4F_decompress(), using a predefined dictionary.
 *  Dictionary is used "in place", without any preprocessing.
 *  It must remain accessible throughout the entire frame decoding. */
KLZ4FLIB_STATIC_API size_t KLZ4F_decompress_usingDict(
    KLZ4F_dctx* dctxPtr,
    void* dstBuffer, size_t* dstSizePtr,
    const void* srcBuffer, size_t* srcSizePtr,
    const void* dict, size_t dictSize,
    const KLZ4F_decompressOptions_t* decompressOptionsPtr);

#if defined (__cplusplus)
}
#endif

#endif  /* defined(KLZ4F_STATIC_LINKING_ONLY) && !defined(KLZ4F_H_STATIC_09782039843) */

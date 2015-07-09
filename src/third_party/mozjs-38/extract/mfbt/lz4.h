/*
   LZ4 - Fast LZ compression algorithm
   Header File
   Copyright (C) 2011-2014, Yann Collet.
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
   - LZ4 source repository : http://code.google.com/p/lz4/
   - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
#pragma once

#if defined (__cplusplus)
extern "C" {
#endif


/**************************************
   Version
**************************************/
#define LZ4_VERSION_MAJOR    1    /* for major interface/format changes  */
#define LZ4_VERSION_MINOR    2    /* for minor interface/format changes  */
#define LZ4_VERSION_RELEASE  0    /* for tweaks, bug-fixes, or development */


/**************************************
   Tuning parameter
**************************************/
/*
 * LZ4_MEMORY_USAGE :
 * Memory usage formula : N->2^N Bytes (examples : 10 -> 1KB; 12 -> 4KB ; 16 -> 64KB; 20 -> 1MB; etc.)
 * Increasing memory usage improves compression ratio
 * Reduced memory usage can improve speed, due to cache effect
 * Default value is 14, for 16KB, which nicely fits into Intel x86 L1 cache
 */
#define LZ4_MEMORY_USAGE 14


/**************************************
   Simple Functions
**************************************/

int LZ4_compress        (const char* source, char* dest, int inputSize);
int LZ4_decompress_safe (const char* source, char* dest, int compressedSize, int maxOutputSize);

/*
LZ4_compress() :
    Compresses 'inputSize' bytes from 'source' into 'dest'.
    Destination buffer must be already allocated,
    and must be sized to handle worst cases situations (input data not compressible)
    Worst case size evaluation is provided by function LZ4_compressBound()
    inputSize : Max supported value is LZ4_MAX_INPUT_VALUE
    return : the number of bytes written in buffer dest
             or 0 if the compression fails

LZ4_decompress_safe() :
    compressedSize : is obviously the source size
    maxOutputSize : is the size of the destination buffer, which must be already allocated.
    return : the number of bytes decoded in the destination buffer (necessarily <= maxOutputSize)
             If the destination buffer is not large enough, decoding will stop and output an error code (<0).
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             This function is protected against buffer overflow exploits :
             it never writes outside of output buffer, and never reads outside of input buffer.
             Therefore, it is protected against malicious data packets.
*/


/*
Note :
    Should you prefer to explicitly allocate compression-table memory using your own allocation method,
    use the streaming functions provided below, simply reset the memory area between each call to LZ4_compress_continue()
*/


/**************************************
   Advanced Functions
**************************************/
#define LZ4_MAX_INPUT_SIZE        0x7E000000   /* 2 113 929 216 bytes */
#define LZ4_COMPRESSBOUND(isize)  ((unsigned int)(isize) > (unsigned int)LZ4_MAX_INPUT_SIZE ? 0 : (isize) + ((isize)/255) + 16)

/*
LZ4_compressBound() :
    Provides the maximum size that LZ4 may output in a "worst case" scenario (input data not compressible)
    primarily useful for memory allocation of output buffer.
    macro is also provided when result needs to be evaluated at compilation (such as stack memory allocation).

    isize  : is the input size. Max supported value is LZ4_MAX_INPUT_SIZE
    return : maximum output size in a "worst case" scenario
             or 0, if input size is too large ( > LZ4_MAX_INPUT_SIZE)
*/
int LZ4_compressBound(int isize);


/*
LZ4_compress_limitedOutput() :
    Compress 'inputSize' bytes from 'source' into an output buffer 'dest' of maximum size 'maxOutputSize'.
    If it cannot achieve it, compression will stop, and result of the function will be zero.
    This function never writes outside of provided output buffer.

    inputSize  : Max supported value is LZ4_MAX_INPUT_VALUE
    maxOutputSize : is the size of the destination buffer (which must be already allocated)
    return : the number of bytes written in buffer 'dest'
             or 0 if the compression fails
*/
int LZ4_compress_limitedOutput (const char* source, char* dest, int inputSize, int maxOutputSize);


/*
LZ4_decompress_fast() :
    originalSize : is the original and therefore uncompressed size
    return : the number of bytes read from the source buffer (in other words, the compressed size)
             If the source stream is malformed, the function will stop decoding and return a negative result.
             Destination buffer must be already allocated. Its size must be a minimum of 'originalSize' bytes.
    note : This function is a bit faster than LZ4_decompress_safe()
           It provides fast decompression and fully respect memory boundaries for properly formed compressed data.
           It does not provide full protection against intentionnally modified data stream.
           Use this function in a trusted environment (data to decode comes from a trusted source).
*/
int LZ4_decompress_fast (const char* source, char* dest, int originalSize);


/*
LZ4_decompress_safe_partial() :
    This function decompress a compressed block of size 'compressedSize' at position 'source'
    into output buffer 'dest' of size 'maxOutputSize'.
    The function tries to stop decompressing operation as soon as 'targetOutputSize' has been reached,
    reducing decompression time.
    return : the number of bytes decoded in the destination buffer (necessarily <= maxOutputSize)
       Note : this number can be < 'targetOutputSize' should the compressed block to decode be smaller.
             Always control how many bytes were decoded.
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             This function never writes outside of output buffer, and never reads outside of input buffer. It is therefore protected against malicious data packets
*/
int LZ4_decompress_safe_partial (const char* source, char* dest, int compressedSize, int targetOutputSize, int maxOutputSize);


/***********************************************
   Experimental Streaming Compression Functions
***********************************************/

#define LZ4_STREAMSIZE_U32 ((1 << (LZ4_MEMORY_USAGE-2)) + 8)
#define LZ4_STREAMSIZE     (LZ4_STREAMSIZE_U32 * sizeof(unsigned int))
/*
 * LZ4_stream_t
 * information structure to track an LZ4 stream.
 * important : set this structure content to zero before first use !
 */
typedef struct { unsigned int table[LZ4_STREAMSIZE_U32]; } LZ4_stream_t;

/*
 * If you prefer dynamic allocation methods,
 * LZ4_createStream
 * provides a pointer (void*) towards an initialized LZ4_stream_t structure.
 * LZ4_free just frees it.
 */
void* LZ4_createStream();
int   LZ4_free (void* LZ4_stream);


/*
 * LZ4_loadDict
 * Use this function to load a static dictionary into LZ4_stream.
 * Any previous data will be forgotten, only 'dictionary' will remain in memory.
 * Loading a size of 0 is allowed (same effect as init).
 * Return : 1 if OK, 0 if error
 */
int LZ4_loadDict (void* LZ4_stream, const char* dictionary, int dictSize);

/*
 * LZ4_compress_continue
 * Compress data block 'source', using blocks compressed before as dictionary to improve compression ratio
 * Previous data blocks are assumed to still be present at their previous location.
 */
int LZ4_compress_continue (void* LZ4_stream, const char* source, char* dest, int inputSize);

/*
 * LZ4_compress_limitedOutput_continue
 * Same as before, but also specify a maximum target compressed size (maxOutputSize)
 * If objective cannot be met, compression exits, and returns a zero.
 */
int LZ4_compress_limitedOutput_continue (void* LZ4_stream, const char* source, char* dest, int inputSize, int maxOutputSize);

/*
 * LZ4_saveDict
 * If previously compressed data block is not guaranteed to remain at its previous memory location
 * save it into a safe place (char* safeBuffer)
 * Note : you don't need to call LZ4_loadDict() afterwards,
 *        dictionary is immediately usable, you can therefore call again LZ4_compress_continue()
 * Return : 1 if OK, 0 if error
 * Note : any dictSize > 64 KB will be interpreted as 64KB.
 */
int LZ4_saveDict (void* LZ4_stream, char* safeBuffer, int dictSize);


/************************************************
  Experimental Streaming Decompression Functions
************************************************/

#define LZ4_STREAMDECODESIZE_U32 4
#define LZ4_STREAMDECODESIZE     (LZ4_STREAMDECODESIZE_U32 * sizeof(unsigned int))
/*
 * LZ4_streamDecode_t
 * information structure to track an LZ4 stream.
 * important : set this structure content to zero before first use !
 */
typedef struct { unsigned int table[LZ4_STREAMDECODESIZE_U32]; } LZ4_streamDecode_t;

/*
 * If you prefer dynamic allocation methods,
 * LZ4_createStreamDecode()
 * provides a pointer (void*) towards an initialized LZ4_streamDecode_t structure.
 * LZ4_free just frees it.
 */
void* LZ4_createStreamDecode();
int   LZ4_free (void* LZ4_stream);   /* yes, it's the same one as for compression */

/*
*_continue() :
    These decoding functions allow decompression of multiple blocks in "streaming" mode.
    Previously decoded blocks must still be available at the memory position where they were decoded.
    If it's not possible, save the relevant part of decoded data into a safe buffer,
    and indicate where it stands using LZ4_setDictDecode()
*/
int LZ4_decompress_safe_continue (void* LZ4_streamDecode, const char* source, char* dest, int compressedSize, int maxOutputSize);
int LZ4_decompress_fast_continue (void* LZ4_streamDecode, const char* source, char* dest, int originalSize);

/*
 * LZ4_setDictDecode
 * Use this function to instruct where to find the dictionary.
 * This function can be used to specify a static dictionary,
 * or to instruct where to find some previously decoded data saved into a different memory space.
 * Setting a size of 0 is allowed (same effect as no dictionary).
 * Return : 1 if OK, 0 if error
 */
int LZ4_setDictDecode (void* LZ4_streamDecode, const char* dictionary, int dictSize);


/*
Advanced decoding functions :
*_usingDict() :
    These decoding functions work the same as
    a combination of LZ4_setDictDecode() followed by LZ4_decompress_x_continue()
    all together into a single function call.
    It doesn't use nor update an LZ4_streamDecode_t structure.
*/
int LZ4_decompress_safe_usingDict (const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize);
int LZ4_decompress_fast_usingDict (const char* source, char* dest, int originalSize, const char* dictStart, int dictSize);



#if defined (__cplusplus)
}
#endif

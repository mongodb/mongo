/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/checksums/private/crc_priv.h>

#include <aws/common/cpuid.h>

/* clang-format off */

/* this implementation is only for the x86_64 intel architecture */
#if defined(__x86_64__)
#    if defined(__clang__)
#        pragma clang diagnostic push
#        pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#    endif

/* use local labels, so that linker doesn't think these are functions it can deadstrip */
#    ifdef __APPLE__
#        define LABEL(label) "L_" #label "_%="
#    else
#        define LABEL(label) ".L_" #label "_%="
#    endif

/*
 * Factored out common inline asm for folding crc0,crc1,crc2 stripes in rcx, r11, r10 using
 * the specified Magic Constants K1 and K2.
 * Assumes rcx, r11, r10 contain crc0, crc1, crc2 that need folding
 * Utilizes xmm1, xmm2, xmm3, xmm4 as well as clobbering r8, r9, r11
 * Result is placed in ecx
 */
#    define FOLD_K1K2(K1, K2)                                                                                          \
        "movl             " #K1 ", %%r8d    # Magic K1 constant \n"                                                    \
        "movl             " #K2 ", %%r9d    # Magic K2 constant \n"                                                    \
        "movq              %%rcx, %%xmm1   # crc0 into lower dword of xmm1 \n"                                         \
        "movq               %%r8, %%xmm3   # K1 into lower dword of xmm3 \n"                                           \
        "movq              %%r11, %%xmm2   # crc1 into lower dword of xmm2 \n"                                         \
        "movq               %%r9, %%xmm4   # K2 into lower dword of xmm4 \n"                                           \
        "pclmulqdq $0x00, %%xmm3, %%xmm1   # Multiply crc0 by K1 \n"                                                   \
        "pclmulqdq $0x00, %%xmm4, %%xmm2   # Multiply crc1 by K2 \n"                                                   \
        "xor               %%rcx, %%rcx    # \n"                                                                       \
        "xor               %%r11, %%r11    # \n"                                                                       \
        "movq             %%xmm1, %%r8     # \n"                                                                       \
        "movq             %%xmm2, %%r9     # \n"                                                                       \
        "crc32q             %%r8, %%rcx    # folding crc0 \n"                                                          \
        "crc32q             %%r9, %%r11    # folding crc1 \n"                                                          \
        "xor              %%r10d, %%ecx    # combine crc2 and crc0 \n"                                                 \
        "xor              %%r11d, %%ecx    # combine crc1 and crc0 \n"

/**
 * Private (static) function.
 * Computes the Castagnoli CRC32c (iSCSI) of the specified data buffer using the Intel CRC32Q (quad word) machine
 * instruction by operating on 24-byte stripes in parallel. The results are folded together using CLMUL. This function
 * is optimized for exactly 256 byte blocks that are best aligned on 8-byte memory addresses. It MUST be passed a
 * pointer to input data that is exactly 256 bytes in length. Note: this function does NOT invert bits of the input crc
 * or return value.
 */
static inline uint32_t s_crc32c_sse42_clmul_256(const uint8_t *input, uint32_t crc) {
    __asm__ __volatile__(
        "xor          %%r11, %%r11    # zero all 64 bits in r11, will track crc1 \n"
        "xor          %%r10, %%r10    # zero all 64 bits in r10, will track crc2 \n"

        "crc32q    0(%[in]), %%rcx    # crc0 \n"
        "crc32q   88(%[in]), %%r11    # crc1 \n"
        "crc32q  176(%[in]), %%r10    # crc2 \n"

        "crc32q    8(%[in]), %%rcx    # crc0 \n"
        "crc32q   96(%[in]), %%r11    # crc1 \n"
        "crc32q  184(%[in]), %%r10    # crc2 \n"

        "crc32q   16(%[in]), %%rcx    # crc0 \n"
        "crc32q  104(%[in]), %%r11    # crc1 \n"
        "crc32q  192(%[in]), %%r10    # crc2 \n"

        "crc32q   24(%[in]), %%rcx    # crc0 \n"
        "crc32q  112(%[in]), %%r11    # crc1 \n"
        "crc32q  200(%[in]), %%r10    # crc2 \n"

        "crc32q   32(%[in]), %%rcx    # crc0 \n"
        "crc32q  120(%[in]), %%r11    # crc1 \n"
        "crc32q  208(%[in]), %%r10    # crc2 \n"

        "crc32q   40(%[in]), %%rcx    # crc0 \n"
        "crc32q  128(%[in]), %%r11    # crc1 \n"
        "crc32q  216(%[in]), %%r10    # crc2 \n"

        "crc32q   48(%[in]), %%rcx    # crc0 \n"
        "crc32q  136(%[in]), %%r11    # crc1 \n"
        "crc32q  224(%[in]), %%r10    # crc2 \n"

        "crc32q   56(%[in]), %%rcx    # crc0 \n"
        "crc32q  144(%[in]), %%r11    # crc1 \n"
        "crc32q  232(%[in]), %%r10    # crc2 \n"

        "crc32q   64(%[in]), %%rcx    # crc0 \n"
        "crc32q  152(%[in]), %%r11    # crc1 \n"
        "crc32q  240(%[in]), %%r10    # crc2 \n"

        "crc32q   72(%[in]), %%rcx    # crc0 \n"
        "crc32q  160(%[in]), %%r11    # crc1 \n"
        "crc32q  248(%[in]), %%r10    # crc2 \n"

        "crc32q   80(%[in]), %%rcx    # crc0 \n"
        "crc32q  168(%[in]), %%r11    # crc2 \n"

        FOLD_K1K2($0x1b3d8f29, $0x39d3b296) /* Magic Constants used to fold crc stripes into ecx */

        /* output registers
         [crc] is an input and and output so it is marked read/write (i.e. "+c")*/
        : [ crc ] "+c"(crc)
        /* input registers */
        : [ in ] "d"(input)

        /* additional clobbered registers */
        : "%r8", "%r9", "%r11", "%r10", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "cc");
    return crc;
}

/**
 * Private (static) function.
 * Computes the Castagnoli CRC32c (iSCSI) of the specified data buffer using the Intel CRC32Q (quad word) machine
 * instruction by operating on 3 24-byte stripes in parallel. The results are folded together using CLMUL. This function
 * is optimized for exactly 1024 byte blocks that are best aligned on 8-byte memory addresses. It MUST be passed a
 * pointer to input data that is exactly 1024 bytes in length. Note: this function does NOT invert bits of the input crc
 * or return value.
 */
static inline uint32_t s_crc32c_sse42_clmul_1024(const uint8_t *input, uint32_t crc) {
    __asm__ __volatile__(
        "xor          %%r11, %%r11    # zero all 64 bits in r11, will track crc1 \n"
        "xor          %%r10, %%r10    # zero all 64 bits in r10, will track crc2 \n"

        "movl            $5, %%r8d    # Loop 5 times through 64 byte chunks in 3 parallel stripes \n"

        LABEL(loop_1024) ": \n"

        "prefetcht0  128(%[in])       # \n"
        "prefetcht0  472(%[in])       # \n"
        "prefetcht0  808(%[in])       # \n"

        "crc32q    0(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q  344(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q  680(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q    8(%[in]), %%rcx    # crc0 \n"
        "crc32q  352(%[in]), %%r11    # crc1 \n"
        "crc32q  688(%[in]), %%r10    # crc2 \n"

        "crc32q   16(%[in]), %%rcx    # crc0 \n"
        "crc32q  360(%[in]), %%r11    # crc1 \n"
        "crc32q  696(%[in]), %%r10    # crc2 \n"

        "crc32q   24(%[in]), %%rcx    # crc0 \n"
        "crc32q  368(%[in]), %%r11    # crc1 \n"
        "crc32q  704(%[in]), %%r10    # crc2 \n"

        "crc32q   32(%[in]), %%rcx    # crc0 \n"
        "crc32q  376(%[in]), %%r11    # crc1 \n"
        "crc32q  712(%[in]), %%r10    # crc2 \n"

        "crc32q   40(%[in]), %%rcx    # crc0 \n"
        "crc32q  384(%[in]), %%r11    # crc1 \n"
        "crc32q  720(%[in]), %%r10    # crc2 \n"

        "crc32q   48(%[in]), %%rcx    # crc0 \n"
        "crc32q  392(%[in]), %%r11    # crc1 \n"
        "crc32q  728(%[in]), %%r10    # crc2 \n"

        "crc32q   56(%[in]), %%rcx    # crc0 \n"
        "crc32q  400(%[in]), %%r11    # crc1 \n"
        "crc32q  736(%[in]), %%r10    # crc2 \n"

        "add            $64, %[in]    # \n"
        "sub             $1, %%r8d    # \n"
        "jnz " LABEL(loop_1024) "     # \n"

        "crc32q    0(%[in]), %%rcx    # crc0 \n"
        "crc32q  344(%[in]), %%r11    # crc1 \n"
        "crc32q  680(%[in]), %%r10    # crc2 \n"

        "crc32q    8(%[in]), %%rcx    # crc0 \n"
        "crc32q  352(%[in]), %%r11    # crc1 \n"
        "crc32q  688(%[in]), %%r10    # crc2 \n"

        "crc32q   16(%[in]), %%rcx    # crc0 \n"
        "crc32q  696(%[in]), %%r10    # crc2 \n"

        FOLD_K1K2($0xe417f38a, $0x8f158014) /* Magic Constants used to fold crc stripes into ecx

                            output registers
                            [crc] is an input and and output so it is marked read/write (i.e. "+c")
                            we clobber the register for [input] (via add instruction) so we must also
                            tag it read/write (i.e. "+d") in the list of outputs to tell gcc about the clobber */
        : [ crc ] "+c"(crc), [ in ] "+d"(input)
        :
        /* additional clobbered registers */
        /* "cc" is the flags - we add and sub, so the flags are also clobbered */
        : "%r8", "%r9", "%r11", "%r10", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "cc");
    return crc;
}

/**
 * Private (static) function.
 * Computes the Castagnoli CRC32c (iSCSI) of the specified data buffer using the Intel CRC32Q (quad word) machine
 * instruction by operating on 24-byte stripes in parallel. The results are folded together using CLMUL. This function
 * is optimized for exactly 3072 byte blocks that are best aligned on 8-byte memory addresses. It MUST be passed a
 * pointer to input data that is exactly 3072 bytes in length. Note: this function does NOT invert bits of the input crc
 * or return value.
 */
static inline uint32_t s_crc32c_sse42_clmul_3072(const uint8_t *input, uint32_t crc) {
    __asm__ __volatile__(
        "xor          %%r11, %%r11    # zero all 64 bits in r11, will track crc1 \n"
        "xor          %%r10, %%r10    # zero all 64 bits in r10, will track crc2 \n"

        "movl           $16, %%r8d    # Loop 16 times through 64 byte chunks in 3 parallel stripes \n"

        LABEL(loop_3072) ": \n"

        "prefetcht0  128(%[in])       # \n"
        "prefetcht0 1152(%[in])       # \n"
        "prefetcht0 2176(%[in])       # \n"

        "crc32q    0(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1024(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2048(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q    8(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1032(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2056(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q   16(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1040(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2064(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q   24(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1048(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2072(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q   32(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1056(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2080(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q   40(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1064(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2088(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q   48(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1072(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2096(%[in]), %%r10    # crc2: stripe2 \n"

        "crc32q   56(%[in]), %%rcx    # crc0: stripe0 \n"
        "crc32q 1080(%[in]), %%r11    # crc1: stripe1 \n"
        "crc32q 2104(%[in]), %%r10    # crc2: stripe2 \n"

        "add            $64, %[in]    # \n"
        "sub             $1, %%r8d    # \n"
        "jnz " LABEL(loop_3072) "     # \n"

        FOLD_K1K2(
            $0xa51b6135,
            $0x170076fa) /* Magic Constants used to fold crc stripes into ecx

                            output registers
                            [crc] is an input and and output so it is marked read/write (i.e. "+c")
                            we clobber the register for [input] (via add instruction) so we must also
                            tag it read/write (i.e. "+d") in the list of outputs to tell gcc about the clobber*/
        : [ crc ] "+c"(crc), [ in ] "+d"(input)
        :
        /* additional clobbered registers
          "cc" is the flags - we add and sub, so the flags are also clobbered */
        : "%r8", "%r9", "%r11", "%r10", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "cc");

    return crc;
}

static bool detection_performed = false;
static bool detected_clmul = false;

/*
 * Computes the Castagnoli CRC32c (iSCSI) of the specified data buffer using the Intel CRC32Q (64-bit quad word) and
 * PCLMULQDQ machine instructions (if present).
 * Handles data that isn't 8-byte aligned as well as any trailing data with the CRC32B (byte) instruction.
 * Pass 0 in the previousCrc32 parameter as an initial value unless continuing to update a running CRC in a subsequent
 * call.
 */
uint32_t aws_checksums_crc32c_clmul_sse42(const uint8_t *input, int length, uint32_t previousCrc32) {

    if (AWS_UNLIKELY(!detection_performed)) {
        detected_clmul = aws_cpu_has_feature(AWS_CPU_FEATURE_CLMUL);
        /* Simply setting the flag true to skip HW detection next time
           Not using memory barriers since the worst that can
           happen is a fallback to the non HW accelerated code. */
        detection_performed = true;
    }

    /* this is called by a higher-level shim and previousCRC32 is already ~ */
    uint32_t crc = ~previousCrc32;

    /* For small input, forget about alignment checks - simply compute the CRC32c one byte at a time */
    if (AWS_UNLIKELY(length < 8)) {
        while (length-- > 0) {
            __asm__("CRC32B (%[in]), %[crc]" : [ crc ] "+c"(crc) : [ in ] "r"(input));
            input++;
        }
        return ~crc;
    }

    /* Get the 8-byte memory alignment of our input buffer by looking at the least significant 3 bits */
    int input_alignment = (unsigned long int)input & 0x7;

    /* Compute the number of unaligned bytes before the first aligned 8-byte chunk (will be in the range 0-7) */
    int leading = (8 - input_alignment) & 0x7;

    /* reduce the length by the leading unaligned bytes we are about to process */
    length -= leading;

    /* spin through the leading unaligned input bytes (if any) one-by-one */
    while (leading-- > 0) {
        __asm__("CRC32B (%[in]), %[crc]" : [ crc ] "+c"(crc) : [ in ] "r"(input));
        input++;
    }

    /* Using likely to keep this code inlined */
    if (AWS_LIKELY(detected_clmul)) {

        while (AWS_LIKELY(length >= 3072)) {
            /* Compute crc32c on each block, chaining each crc result */
            crc = s_crc32c_sse42_clmul_3072(input, crc);
            input += 3072;
            length -= 3072;
        }
        while (AWS_LIKELY(length >= 1024)) {
            /* Compute crc32c on each block, chaining each crc result */
            crc = s_crc32c_sse42_clmul_1024(input, crc);
            input += 1024;
            length -= 1024;
        }
        while (AWS_LIKELY(length >= 256)) {
            /* Compute crc32c on each block, chaining each crc result */
            crc = s_crc32c_sse42_clmul_256(input, crc);
            input += 256;
            length -= 256;
        }
    }

    /* Spin through remaining (aligned) 8-byte chunks using the CRC32Q quad word instruction */
    while (AWS_LIKELY(length >= 8)) {
        /* Hardcoding %rcx register (i.e. "+c") to allow use of qword instruction */
        __asm__ __volatile__("CRC32Q (%[in]), %%rcx" : [ crc ] "+c"(crc) : [ in ] "r"(input));
        input += 8;
        length -= 8;
    }

    /* Finish up with any trailing bytes using the CRC32B single byte instruction one-by-one */
    while (length-- > 0) {
        __asm__ __volatile__("CRC32B (%[in]), %[crc]" : [ crc ] "+c"(crc) : [ in ] "r"(input));
        input++;
    }

    return ~crc;
}

#    if defined(__clang__)
#        pragma clang diagnostic pop
#    endif

#endif /* x86_64 */
/* clang-format on */

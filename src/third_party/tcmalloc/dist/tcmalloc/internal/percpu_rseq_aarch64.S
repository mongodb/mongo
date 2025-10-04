/*
 * Copyright 2020 The TCMalloc Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License")
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __aarch64__
#error "percpu_rseq_aarch64.S should only be included for AArch64 builds"
#endif  //  __aarch64__

#include "tcmalloc/internal/percpu.h"

/*
 * API Exposition:
 *
 *   METHOD_abort:  // Emitted as part of START_RSEQ()
 *     START_RSEQ() // Starts critical section between [start,commit)
 *   METHOD_start:  // Emitted as part of START_RSEQ()
 *     FETCH_CPU()  // Reads current CPU
 *     ...
 *     single store // Commits sequence
 *   METHOD_commit:
 *     ...return...
 *
 * This process is assisted by the DEFINE_UPSTREAM_CS macro, which encodes a
 * (rodata) constant table, whose address is used to start the critical
 * section, and the abort trampoline.
 *
 * The trampoline is used because:
 * 1.  Restarts are expected to be rare, so the extra jump when restarting is
 *     expected to be infrequent.
 * 2.  The upstream restartable sequence implementation expects the trailing 4
 *     bytes of the abort PC to be "signed" (to prevent manipulation of the PC
 *     to an arbitrary choice).  For us, this is TCMALLOC_PERCPU_RSEQ_SIGNATURE.
 *     This value is passed to the kernel during configuration of the rseq
 *     syscall.
 *     This would either need to be encoded as a nop (SIGN_ABORT) at the start
 *     of every restartable sequence, increasing instruction cache pressure, or
 *     placed directly before the entry point.
 *
 * The trampoline returns us to METHOD_abort, which is the normal entry point
 * for the restartable sequence.  Upon restart, the (upstream) kernel API
 * clears the per-thread restartable sequence state. We return to METHOD_abort
 * (rather than METHOD_start), as we need to reinitialize this value.
 */

/* Place the code into the google_malloc section. This section is the heaviest
 * user of Rseq code, so it makes sense to co-locate it.
 */

.section google_malloc, "ax"

/* ---------------- start helper macros ----------------  */

// This macro defines a relocation associated with the provided label to keep
// section GC from discarding it independently of label.
#if !defined(__clang_major__) || __clang_major__ >= 9
#define PINSECTION(label) .reloc 0, R_AARCH64_NONE, label
#else
#define PINSECTION(label)
#endif

// A function within a guarded memory region must start with a BTI C
// instruction.
// So per ABI that includes any externally visible code label.
// Using hint to make sure we can use this on targets that support BTI and
// targets that don't. It will behave as a no-op on targets that do not
// support BTI or outside a guarded memory region.
#ifdef __ARM_FEATURE_BTI_DEFAULT
#define BTI_C hint 34
#define TAILCALL(x) mov x16, x; br x16
#else
#define BTI_C
#define TAILCALL(x) br x
#endif

// This macro defines:
// * the rseq_cs instance that we'll use for label's critical section.
// * a trampoline to return to when we abort.  This label_trampoline is
//   distinct from label_start, as the return IP must be "signed" (see
//   SIGN_ABORT()).
//
// __rseq_cs only needs to be writeable to allow for relocations.
#define DEFINE_UPSTREAM_CS(label)                                 \
  .pushsection __rseq_cs, "aw";                                   \
  .balign 32;                                                     \
  .protected __rseq_cs_##label;                                   \
  .type __rseq_cs_##label,@object;                                \
  .size __rseq_cs_##label,32;                                     \
  __rseq_cs_##label:                                              \
  .long TCMALLOC_PERCPU_RSEQ_VERSION, TCMALLOC_PERCPU_RSEQ_FLAGS; \
  .quad .L##label##_start;                                        \
  .quad .L##label##_commit - .L##label##_start;                   \
  .quad label##_trampoline;                                       \
  PINSECTION(.L##label##array);                                   \
  .popsection;                                                    \
  .pushsection __rseq_cs_ptr_array, "aw";                         \
  .L##label##array:                                               \
  .quad __rseq_cs_##label;                                        \
  .popsection;                                                    \
  .pushsection rseq_trampoline, "ax";                             \
  SIGN_ABORT();                                                   \
  .globl label##_trampoline;                                      \
  .type  label##_trampoline, @function;                           \
label##_trampoline:                                               \
  CFI(.cfi_startproc);                                            \
  BTI_C;                                                          \
  b .L##label##_abort;                                            \
  CFI(.cfi_endproc);                                              \
  .size label##_trampoline, . - label##_trampoline;               \
  .popsection;

// This is part of the upstream rseq ABI.  The 4 bytes prior to the abort IP
// must match TCMALLOC_PERCPU_RSEQ_SIGNATURE (as configured by our rseq
// syscall's signature parameter).  This signature is used to annotate valid
// abort IPs (since rseq_cs could live in a user-writable segment).
// We use .inst here instead of a data directive so it works for both small and
// big endian.
#define SIGN_ABORT()           \
  .inst TCMALLOC_PERCPU_RSEQ_SIGNATURE

/*
 * Provide a directive to specify the size of symbol "label", relative to the
 * current location and its start.
 */
#define ENCODE_SIZE(label) .size label, . - label
/* We are assuming small memory model.  */
#if __clang_major__ >= 11 && !defined(__AARCH64_CMODEL_SMALL__)
#error "Memory model not supported!"
#endif

/* FETCH_CPU assumes &__rseq_abi is in x5.  */
#define FETCH_CPU(dest, offset) ldrh dest, [x5, offset]
#define FETCH_SLABS(dest) ldr dest, [x5, TCMALLOC_RSEQ_SLABS_OFFSET]

/* With PIE  have initial-exec TLS, even in the presence of position
   independent code. */
#if !defined(__PIC__) || defined(__PIE__)

#define START_RSEQ(src)                         \
  .L##src##_abort:                              \
  mrs     x5, tpidr_el0;                        \
  adrp    x6, :gottprel:__rseq_abi;             \
  ldr     x6, [x6,:gottprel_lo12:__rseq_abi];   \
  add     x5, x5, x6;                           \
  adrp    x6, __rseq_cs_##src;                  \
  add     x6, x6, :lo12:__rseq_cs_##src;        \
  str     x6, [x5, #8];                         \
  .L##src##_start:

#else  /* !defined(__PIC__) || defined(__PIE__) */

/*
 * In the case where we can't guarantee we have initial-exec TLS we obtain
 * __rseq_abi's TP offset using a TLS descriptor sequence, which we then add to
 * the TP to get __rseq_abi's address.
 * The call to the TLS descriptor can be optimized away by the linker, but since
 * we can not guarantee it will we must save and restore the registers used to
 * store the arguments of our functions. The function with most arguments has 5
 * arguments, so we save x0-x4 and lr.
 * TODO: Add PAC support because we are spiling LR.
 */
#define START_RSEQ(src)                        \
  .L##src##_abort:                             \
  mov     x5, lr;                              \
  stp     x0, x1, [sp, -48]!;                  \
  stp     x2, x3, [sp, #16];                   \
  stp     x4, x5, [sp, #32];                   \
  adrp    x0, :tlsdesc:__rseq_abi;             \
  ldr     x1, [x0, :tlsdesc_lo12:__rseq_abi];  \
  add     x0, x0, :tlsdesc_lo12:__rseq_abi;    \
  .tlsdesccall __rseq_abi;                     \
  blr     x1;                                  \
  ldp     x4, x5, [sp, #32];                   \
  mov     lr, x5;                              \
  mrs     x5, tpidr_el0;                       \
  add     x5, x5, x0;                          \
  ldp     x2, x3, [sp, #16];                   \
  ldp     x0, x1, [sp], #48;                   \
  adrp    x6, __rseq_cs_##src;                 \
  add     x6, x6, :lo12:__rseq_cs_##src;       \
  str     x6, [x5, #8];                        \
  .L##src##_start:

#endif
/* ---------------- end helper macros ---------------- */

/* start of atomic restartable sequences */

/* size_t TcmallocSlab_Internal_PushBatch(
 *     size_t size_class (x0),
 *     void** batch (x1),
 *     size_t len (x2)) {
 *   uint64_t* r8 = tcmalloc_rseq.slabs;
 *   if ((r8 & TCMALLOC_CACHED_SLABS_BIT) == 0) return 0;
 *   r8 &= ~TCMALLOC_CACHED_SLABS_BIT;
 *   Header* hdr = r8 + r0 * 8
 *   uint64_t r9 = hdr->current (zero-extend 16bit)
 *   uint64_t r10 = hdr->end    (zero-extend 16bit)
 *   if (r9 >= r10) return 0
 *   r11 = r2
 *   r10 = r9 + min(len, r10 - r9)
 *   r13 = r9 + r10
 *   r9 = r8 + r9 * 8
 *   r14 = r8 + r13 * 8
 * loop:
 *   r12 = *(r11 -= 8) (pre-index) Pop from Batch
 *   *(r9 += 8) = r12 (post-index) Push to Slab
 *   if (r9 != r14) goto loop
 *   hdr->current = r13 (16bit store)
 *   return r10
 * }
 */
  .p2align 6 /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PushBatch
  .type  TcmallocSlab_Internal_PushBatch, @function
TcmallocSlab_Internal_PushBatch:
  CFI(.cfi_startproc)
  BTI_C
  START_RSEQ(TcmallocSlab_Internal_PushBatch)
  FETCH_SLABS(x8)
  tbz x8, #TCMALLOC_CACHED_SLABS_BIT, .LTcmallocSlab_Internal_PushBatch_no_capacity
  and x8, x8, #~TCMALLOC_CACHED_SLABS_MASK
  add x15, x8, x0, LSL #2    /* r15 = hdr */
  ldrh w9, [x15]             /* r9 = current */
  ldrh w10, [x15, #2]        /* r10 = end */
  cmp w9, w10
  bge .LTcmallocSlab_Internal_PushBatch_no_capacity
  add  x11, x1, x2, LSL #3  /* r11 = batch + len * 8 */
  sub  w10, w10, w9         /* r10 = free capacity */
  cmp w2, w10
  csel w10, w2, w10, ls     /* r10 = min(len, free capacity), amount we are
                               pushing */
  add x13, x9, x10          /* r13 = current + amount we are pushing. */
  add x9, x8, x9, LSL #3    /* r9 = current cpu slab stack */
  add x14, x8, x13, LSL #3  /* r14 = new current address */
  tst w10, #1
  beq .LTcmallocSlab_Internal_PushBatch_loop
  ldr x12, [x11,  #-8]!     /* r12 = [--r11] */
  str x12, [x9], #8         /* [r9++] = r12 */
  cmp w10, #1
  beq .LTcmallocSlab_Internal_PushBatch_store
.LTcmallocSlab_Internal_PushBatch_loop:
  ldr q4, [x11,  #-16]!     /* q4 = [r11 - 2], r11 -= 2 */
  str q4, [x9], #16         /* [r9 += 2] = q4 */
  cmp x9, x14               /* if current cpu slab address == new current
                               address */
  bne .LTcmallocSlab_Internal_PushBatch_loop
.LTcmallocSlab_Internal_PushBatch_store:
  strh w13, [x15] /* store new current index */
.LTcmallocSlab_Internal_PushBatch_commit:
  mov x0, x10
  ret
.LTcmallocSlab_Internal_PushBatch_no_capacity:
  mov x0, #0
  ret
  CFI(.cfi_endproc)
ENCODE_SIZE(TcmallocSlab_Internal_PushBatch)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PushBatch)

/* size_t TcmallocSlab_Internal_PopBatch(
 *     size_t size_class (x0),
 *     void** batch (x1),
 *     size_t len (x2),
 *     std::atomic<uint16_t>* begin_ptr (x3)) {
 *   uint64_t* r8 = tcmalloc_rseq.slabs;
 *   if ((r8 & TCMALLOC_CACHED_SLABS_BIT) == 0) return 0;
 *   r8 &= ~TCMALLOC_CACHED_SLABS_BIT;
 *   Header* hdr = GetHeader(r8, size_class)
 *   uint64_t r9 = hdr->current
 *   uint64_t r10 = *begin_ptr
 *   if (r9 <= r10) return 0
 *   r11 = min(len, r9 - r10)
 *   r13 = r8 + r9 * 8
 *   r9 = r9 - r11
 *   r12 = r1
 *   r14 = r1 + r11 * 8
 * loop:
 *   r10 = *(r13 -= 8) (pre-index) Pop from slab
 *   *(r12 += 8) = r10  (post-index) Push to Batch
 *   if (r12 != r14) goto loop
 *   hdr->current = r9
 *   return r11
 * }
 */
  .p2align 6 /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PopBatch
  .type  TcmallocSlab_Internal_PopBatch, @function
TcmallocSlab_Internal_PopBatch:
  CFI(.cfi_startproc)
  BTI_C
  START_RSEQ(TcmallocSlab_Internal_PopBatch)
  FETCH_SLABS(x8)
  tbz x8, #TCMALLOC_CACHED_SLABS_BIT, .LTcmallocSlab_Internal_PopBatch_no_items
  and x8, x8, #~TCMALLOC_CACHED_SLABS_MASK
  add x15, x8, x0, LSL #2
  ldrh w9, [x15]             /* current */
  ldrh w10, [x3]             /* begin */
  cmp w10, w9
  bhs .LTcmallocSlab_Internal_PopBatch_no_items
  sub w11, w9, w10          /* r11 = available items */
  cmp w2, w11
  csel w11, w2, w11, ls     /* r11 = min(len, available items), amount we are
                               popping */
  add x13, x8, x9, LSL #3   /* r13 = current cpu slab stack  */
  sub x9, x9, x11           /* update new current  */
  mov x12, x1               /* r12 = batch */
  add x14, x1, x11, LSL #3  /* r14 = batch + amount we are popping*8 */
  tst w11, #1
  beq .LTcmallocSlab_Internal_PopBatch_loop
  ldr x10, [x13, #-8]!      /* r10 = [--r13] */
  str x10, [x12], #8        /* [r12++] = r10 */
  cmp w11, #1
  beq .LTcmallocSlab_Internal_PopBatch_store
.LTcmallocSlab_Internal_PopBatch_loop:
  ldr q4, [x13, #-16]!      /* q4 = [r13 - 2], r13 -= 2 */
  str q4, [x12], #16        /* [r12 += 2] = q4 */
  cmp x12, x14              /* if current batch == batch + amount we are
                               popping */
  bne .LTcmallocSlab_Internal_PopBatch_loop
.LTcmallocSlab_Internal_PopBatch_store:
  strh w9, [x15]             /* store new current */
.LTcmallocSlab_Internal_PopBatch_commit:
  mov x0, x11
  ret
.LTcmallocSlab_Internal_PopBatch_no_items:
  mov x0, #0
  ret
  CFI(.cfi_endproc)
ENCODE_SIZE(TcmallocSlab_Internal_PopBatch)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PopBatch)

.section .note.GNU-stack,"",@progbits

/* Add a NT_GNU_PROPERTY_TYPE_0 note.  */
#define GNU_PROPERTY(type, value)       \
  .section .note.gnu.property, "a";     \
  .p2align 3;                           \
  .word 4;                              \
  .word 16;                             \
  .word 5;                              \
  .asciz "GNU";                         \
  .word type;                           \
  .word 4;                              \
  .word value;                          \
  .word 0;

/* Add GNU property note if built with branch protection.  */

#if defined(__ARM_FEATURE_BTI_DEFAULT)
GNU_PROPERTY (0xc0000000, 1)
#endif

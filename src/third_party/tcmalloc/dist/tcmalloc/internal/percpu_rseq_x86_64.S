/*
 * Copyright 2019 The TCMalloc Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
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

#ifndef __x86_64__
#error "percpu_rseq_x86_64.S should only be included for x86-64 builds"
#endif  //  __x86_64__

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
 *     to an arbitrary choice).  For us, this is TCMALLOC_PERCPU_RSEQ_SIGNATURE.  This
 *     value is passed to the kernel during configuration of the rseq syscall.
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
#define PINSECTION(label) .reloc 0, R_X86_64_NONE, label
#else
#define PINSECTION(label)
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
  SIGN_ABORT();                                                   \
  .globl label##_trampoline;                                      \
  .type  label##_trampoline, @function;                           \
label##_trampoline:                                               \
  .cfi_startproc;                                                 \
  jmp .L##label##_abort;                                          \
  .cfi_endproc;                                                   \
  .size label##_trampoline, . - label##_trampoline;

// This is part of the upstream rseq ABI.  The 4 bytes prior to the abort IP
// must match TCMALLOC_PERCPU_RSEQ_SIGNATURE (as configured by our rseq
// syscall's signature parameter).  This signature is used to annotate valid
// abort IPs (since rseq_cs could live in a user-writable segment).
//
// To allow this to be safely executed as a valid instruction, we encode the
// value with a nop.  This is decoded as:
//
//   nopl 0xSIGNATURE(%rip)
//
#define SIGN_ABORT()           \
  .byte 0x0f, 0x1f, 0x05;      \
  .long TCMALLOC_PERCPU_RSEQ_SIGNATURE;

/*
 * Provide a directive to specify the size of symbol "label", relative to the
 * current location and its start.
 */
#define ENCODE_SIZE(label) .size label, . - label;

/* In all non-position independent cases we need to use RIP-relative label
   addresses */
#if !defined(__PIC__)
#define LABEL_ADDR(label) $label
#else
#define LABEL_ADDR(label) label@GOTPCREL(%rip)
#endif /* !defined(__PIC__) */

/* With PIE;  have initial-exec TLS, even in the presence of position
   independent code. */
#if !defined(__PIC__) || defined(__PIE__)
#define FETCH_CPU(dest, offset)         \
  movzwl %fs:__rseq_abi@TPOFF(offset), dest;
/* We can use the redzone to save the register. */
#define SAVE_REG(reg) movq reg, -0x08(%rsp);
#define RESTORE_REG(reg) movq -0x08(%rsp), reg;
#define START_RSEQ(src)                        \
  .L##src##_abort:                             \
  leaq __rseq_cs_##src(%rip), %rax;            \
  movq %rax, %fs:__rseq_abi@TPOFF+8;           \
  .L##src##_start:

#else  /* !defined(__PIC__) || defined(__PIE__) */

/*
 * FETCH_CPU assumes &__rseq_abi is in %rax.  We cannot call
 * tcmalloc_tls_fetch_pic at this point, as we have started our restartable
 * sequence.  If we are prempted there, the kernel will clear rseq_cs as
 * tcmalloc_tls_fetch_pic does not appear in the restartable sequence's address
 * range.
 */
#define FETCH_CPU(dest, offset) movzwl (%rax, offset), dest;
/* We can't use the redzone to save the register because
 * tcmalloc_internal_tls_fetch_pic uses the stack. We also need to use stack. */
#define SAVE_REG(reg)                       \
  push %rbp;                                \
  mov %rsp, %rbp;                           \
  sub $0x08, %rsp;                          \
  movq reg, -0x08(%rbp);
#define RESTORE_REG(reg)                    \
  movq -0x08(%rbp), reg;                    \
  add $0x08, %rsp;                          \
  leave;
#define START_RSEQ(src)                     \
  .L##src##_abort:                          \
  call tcmalloc_internal_tls_fetch_pic@PLT; \
  leaq __rseq_cs_##src(%rip), %r11;    	    \
  movq %r11, 8(%rax);                       \
  .L##src##_start:

/*
 * We can safely call this function from within an RSEQ section as it only
 * generates a thread-local address which will not change across a missed
 * restart.  This must precede the construction of any preparatory state.
 */
  .local tcmalloc_internal_tls_fetch_pic
  .type tcmalloc_internal_tls_fetch_pic, @function
tcmalloc_internal_tls_fetch_pic:
  .cfi_startproc
  push %rbp
  .cfi_def_cfa_offset 16
  .cfi_offset 6, -16
  mov %rsp, %rbp
  .cfi_def_cfa_register 6
  sub $0x30, %rsp
  mov %rsi, -0x08(%rbp)  /* atypical abi: tcmalloc_tls_fetch_pic preserves regs */
  mov %rdi, -0x10(%rbp)
  mov %rdx, -0x18(%rbp)
  mov %rcx, -0x20(%rbp)
  mov %r8,  -0x28(%rbp)
  mov %r9,  -0x30(%rbp)
  /*
   * Below is an optimized relocatable TLS lookup per ELF spec:
   *   http://www.akkadia.org/drepper/tls.pdf
   * When possible, this is replaced at link-time with a call-free variant.
   */
  .byte 0x66;
  leaq __rseq_abi@TLSGD(%rip), %rdi;
  .word 0x6666;
  rex64;
  call __tls_get_addr@PLT;
  mov -0x08(%rbp), %rsi
  mov -0x10(%rbp), %rdi
  mov -0x18(%rbp), %rdx
  mov -0x20(%rbp), %rcx
  mov -0x28(%rbp), %r8
  mov -0x30(%rbp), %r9
  add $0x30, %rsp
  leave
  .cfi_def_cfa_register 7
  .cfi_def_cfa_offset 8
  ret; /* &__rseq_abi in %rax */
  .cfi_endproc
ENCODE_SIZE(tcmalloc_internal_tls_fetch_pic)
#endif  /* !defined(__PIC__) || defined(__PIE__) */

/* ---------------- end helper macros ---------------- */

/* start of atomic restartable sequences */

/*
 * NOTE:  We don't use cmpxchgq in the following functions since this would
   make checking the success of our commit operation dependent on flags (which
 * are in turn clobbered by the restart region) -- furthermore we can't just
 * retry to fill in the flags since the restarted cmpxchg may have actually
 * succeeded; spuriously failing subsequent attempts.
 */

/*
 * int TcmallocSlab_Internal_PerCpuCmpxchg64(
 *     int target_cpu (%rdi), long *p (%rsi), long old_val (%rdx),
 *     long new_val (%rcx), size_t virtual_cpu_id_offset (%r8))
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PerCpuCmpxchg64
  .type  TcmallocSlab_Internal_PerCpuCmpxchg64, @function
TcmallocSlab_Internal_PerCpuCmpxchg64:
  .cfi_startproc
  START_RSEQ(TcmallocSlab_Internal_PerCpuCmpxchg64);
  FETCH_CPU(%eax, %r8)
  cmp %eax, %edi; /* check cpu vs current_cpu */
  jne .LTcmallocSlab_Internal_PerCpuCmpxchg64_commit;
  cmp %rdx, (%rsi); /* verify *p == old */
  jne .LTcmallocSlab_Internal_PerCpuCmpxchg64_value_mismatch;
  mov %rcx, (%rsi);
.LTcmallocSlab_Internal_PerCpuCmpxchg64_commit:
  ret;  /* return current cpu, indicating mismatch OR success */
.LTcmallocSlab_Internal_PerCpuCmpxchg64_value_mismatch:
  mov $-1, %eax;  /* mismatch versus "old" or "check", return -1 */
  ret;
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Internal_PerCpuCmpxchg64)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PerCpuCmpxchg64)

/* size_t TcmallocSlab_Internal_PushBatch(
 *     size_t size_class (%rdi),
 *     void** batch (%rsi),
 *     size_t len (%rdx),
 *     uintptr_t slabs_and_shift (%rcx),
 *     size_t virtual_cpu_id_offset (%r8)) {
 *   uint64_t r12 = __rseq_abi.cpu_id;
 *   uint64_t* r12 = CpuMemoryStart(rcx, r12);
 *   Header* hdr = r12 + rdi * 8;
 *   uint64_t r9 = hdr->current;
 *   uint64_t r10 = hdr->end;
 *   if (r9 >= r10) return 0;
 *   r11 = rdx;
 *   r10 = r9 + min(rdx, r10 - r9);
 * loop:
 *   r11--;
 *   rax = batch[r11];
 *   *(r12 + r9 * 8) = rax;
 *   r9++;
 *   if (r9 != r10) goto loop;
 *   hdr->current = r9;
 *   return rdx - r11;
 * }
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PushBatch
  .type  TcmallocSlab_Internal_PushBatch, @function
TcmallocSlab_Internal_PushBatch:
  .cfi_startproc
  SAVE_REG(%r12);
  START_RSEQ(TcmallocSlab_Internal_PushBatch);
  FETCH_CPU(%r12d, %r8);
  /* r9 = slabs_ptr */
  movq %rcx, %r9;
  and $TCMALLOC_PERCPU_SLABS_MASK, %r9;
  shl %cl, %r12; /* multiply cpu by 2^shift */
  lea (%r9, %r12), %r12;
  movzwq (%r12, %rdi, 8), %r9; /* current */
  movzwq 6(%r12, %rdi, 8), %r10; /* end */
  cmpq %r10, %r9;
  jae .LTcmallocSlab_Internal_PushBatch_full;
  movq %rdx, %r11; /* r11 = copy of len */
  subq %r9, %r10; /* r10 = free capacity */
  cmpq %rdx, %r10;
  cmovaq %rdx, %r10; /* r10 = min(len, free capacity) */
  addq %r9, %r10;
.LTcmallocSlab_Internal_PushBatch_loop:
  decq %r11;
  movq (%rsi, %r11, 8), %rax;
  movq %rax, (%r12, %r9, 8);
  incq %r9;
  cmpq %r9, %r10;
  jne .LTcmallocSlab_Internal_PushBatch_loop
  movw %r9w, (%r12, %rdi, 8);
.LTcmallocSlab_Internal_PushBatch_commit:
  movq %rdx, %rax;
  subq %r11, %rax;
  RESTORE_REG(%r12);
  ret;
.LTcmallocSlab_Internal_PushBatch_full:
  xor %rax, %rax;
  RESTORE_REG(%r12);
  ret;
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Internal_PushBatch)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PushBatch)

/* size_t TcmallocSlab_Internal_PopBatch(
 *     size_t size_class (%rdi),
 *     void** batch (%rsi),
 *     size_t len (%rdx),
 *     uintptr_t slabs_and_shift (%rcx),
 *     size_t virtual_cpu_id_offset (%r8)) {
 *   uint64_t r12 = __rseq_abi.cpu_id;
 *   uint64_t* r12 = CpuMemoryStart(rcx, r12);
 *   Header* hdr = GetHeader(rdi, rax, size_class);
 *   uint64_t r9 = hdr->current;
 *   uint64_t r10 = hdr->begin;
 *   if (r9 <= r10) return 0;
 *   r11 = min(rdx, r9 - r10);
 *   rax = 0;
 * loop:
 *   r9--;
 *   r10 = *(r12 + r9 * 8);
 *   batch[rax] = r10;
 *   rax++;
 *   if (rax != r11) goto loop;
 *   hdr->current = r9;
 *   return rax;
 * }
 */
  .p2align 6; /* aligns to 2^6 with NOP filling */
  .globl TcmallocSlab_Internal_PopBatch
  .type  TcmallocSlab_Internal_PopBatch, @function
TcmallocSlab_Internal_PopBatch:
  .cfi_startproc
  SAVE_REG(%r12);
  START_RSEQ(TcmallocSlab_Internal_PopBatch);
  FETCH_CPU(%r12d, %r8);
  /* r9 = slabs_ptr */
  movq %rcx, %r9;
  and $TCMALLOC_PERCPU_SLABS_MASK, %r9;
  shl %cl, %r12; /* multiply cpu by 2^shift */
  lea (%r9, %r12), %r12;
  movzwq (%r12, %rdi, 8), %r9; /* current */
  movzwq 4(%r12, %rdi, 8), %r10; /* begin */
  cmp %r10, %r9;
  jbe .LTcmallocSlab_Internal_PopBatch_empty;
  movq %r9, %r11;
  subq %r10, %r11; /* r11 = available items */
  cmpq %rdx, %r11;
  cmovaq %rdx, %r11; /* r11 = min(len, available items) */
  xorq %rax, %rax;
.LTcmallocSlab_Internal_PopBatch_loop:
  decq %r9;
  movq (%r12, %r9, 8), %r10;
  movq %r10, (%rsi, %rax, 8);
  incq %rax;
  cmpq %rax, %r11;
  jne .LTcmallocSlab_Internal_PopBatch_loop
  movw %r9w, (%r12, %rdi, 8);
.LTcmallocSlab_Internal_PopBatch_commit:
  RESTORE_REG(%r12);
  ret;
.LTcmallocSlab_Internal_PopBatch_empty:
  xor %rax, %rax;
  RESTORE_REG(%r12);
  ret;
  .cfi_endproc
ENCODE_SIZE(TcmallocSlab_Internal_PopBatch)
DEFINE_UPSTREAM_CS(TcmallocSlab_Internal_PopBatch)

.section .note.GNU-stack,"",@progbits

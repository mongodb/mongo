// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Single file to include target specific implementations for percpu.

#include "tcmalloc/internal/percpu.h"

#ifdef __GCC_HAVE_DWARF2_CFI_ASM
#define CFI(...) __VA_ARGS__
#else
#define CFI(...)
#endif

#if TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM
#if defined(__x86_64__)
#include "tcmalloc/internal/percpu_rseq_x86_64.S"
#elif defined(__aarch64__)
#include "tcmalloc/internal/percpu_rseq_aarch64.S"
#else
#error "RSEQ support expected, but not found."
#endif

// See the comment about data layout in percpu.h for details.
.type tcmalloc_sampler, @object
.type tcmalloc_slabs, @object
.type __rseq_abi, @object
.section .tdata, "awT", @progbits, unique, 1
.globl tcmalloc_sampler
.globl tcmalloc_slabs
.globl __rseq_abi
.p2align 6
.zero 64 + 32 - TCMALLOC_SAMPLER_SIZE - 8
tcmalloc_sampler:
.zero TCMALLOC_SAMPLER_SIZE
// alignment padding (since tcmalloc_slabs is only 4-bytes aligned)
.zero 4
tcmalloc_slabs:
.long 0
__rseq_abi:
.long 0           // cpu_id_start
.long 0xffffffff  // cpu_id (kCpuIdUninitialized)
.quad 0           // rseq_cs
.long 0           // flags
.quad 0           // padding
.short 0xffff     // numa_node_id (kCpuIdUninitialized)
.short 0xffff     // vcpu_id (kCpuIdUninitialized)
.size __rseq_abi, 32
.size tcmalloc_sampler, TCMALLOC_SAMPLER_SIZE
.size tcmalloc_slabs, 8

#endif // TCMALLOC_PERCPU_RSEQ_SUPPORTED_PLATFORM

// We do not need an executable stack. Put this outside the
// architecture-specific region above in order to suppress "missing
// .note.GNU-stack section implies executable stack" errors.
//
// Cf. http://en.chys.info/2010/12/note-gnu-stack/
#if defined(__arm__)
.section .note.GNU-stack, "", %progbits
#else
.section .note.GNU-stack, "", @progbits
#endif  // __arm__


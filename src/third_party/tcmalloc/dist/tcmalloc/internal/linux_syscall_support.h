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

#ifndef TCMALLOC_INTERNAL_LINUX_SYSCALL_SUPPORT_H_
#define TCMALLOC_INTERNAL_LINUX_SYSCALL_SUPPORT_H_

#ifdef __linux__
#include <linux/kernel-page-flags.h>
#endif  // __linux__

/* include/uapi/linux/rseq.h                                                 */

struct kernel_rseq {
  unsigned cpu_id_start;
  unsigned cpu_id;
  unsigned long long rseq_cs;
  unsigned flags;
  unsigned padding[2];
  // This is a prototype extension to the rseq() syscall.  Since a process may
  // run on only a few cores at a time, we can use a dense set of "v(irtual)
  // cpus."  This can reduce cache requirements, as we only need N caches for
  // the cores we actually run on simultaneously, rather than a cache for every
  // physical core.
  union {
    struct {
      short numa_node_id;
      short vcpu_id;
    };
    int vcpu_flat;
  };
} __attribute__((aligned(4 * sizeof(unsigned long long))));

static_assert(sizeof(kernel_rseq) == (4 * sizeof(unsigned long long)),
              "Unexpected size for rseq structure");

struct kernel_rseq_cs {
  // This is aligned, per upstream RSEQ specification.
} __attribute__((aligned(4 * sizeof(unsigned long long))));

static_assert(sizeof(kernel_rseq_cs) == (4 * sizeof(unsigned long long)),
              "Unexpected size for rseq_cs structure");

#if !defined(__NR_rseq)
#if defined(__x86_64__)
#define __NR_rseq 334
#elif defined(__aarch64__)
#define __NR_rseq 293
#elif defined(__PPC__)
#define __NR_rseq 387
#endif
#endif

#ifndef KPF_ZERO_PAGE
#define KPF_ZERO_PAGE 24
#endif

#endif  // TCMALLOC_INTERNAL_LINUX_SYSCALL_SUPPORT_H_

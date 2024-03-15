/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitDump_h
#define jit_JitDump_h

/*
   This file provides the necessary data structures to meet the JitDump
   specification as of
   https://github.com/torvalds/linux/blob/f2906aa863381afb0015a9eb7fefad885d4e5a56/tools/perf/Documentation/jitdump-specification.txt
*/

namespace js {
namespace jit {

// JitDump record types
enum {
  JIT_CODE_LOAD = 0,
  JIT_CODE_MOVE,
  JIT_CODE_DEBUG_INFO,
  JIT_CODE_CLOSE,
  JIT_CODE_UNWINDING_INFO
};

// File header
struct JitDumpHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t total_size;
  uint32_t elf_mach;
  uint32_t pad1;
  uint32_t pid;
  uint64_t timestamp;
  uint64_t flags;
};

// Header for each record
struct JitDumpRecordHeader {
  uint32_t id;
  uint32_t total_size;
  uint64_t timestamp;
};

// Load record
struct JitDumpLoadRecord {
  JitDumpRecordHeader header;

  // Payload
  uint32_t pid;
  uint32_t tid;
  uint64_t vma;
  uint64_t code_addr;
  uint64_t code_size;
  uint64_t code_index;
};

// Debug record
struct JitDumpDebugRecord {
  JitDumpRecordHeader header;

  // Debug header
  uint64_t code_addr;
  uint64_t nr_entry;
};

struct JitDumpDebugEntry {
  uint64_t code_addr;
  uint32_t line;
  uint32_t discrim;
};

}  // namespace jit
}  // namespace js

#endif /* jit_JitDump_h */

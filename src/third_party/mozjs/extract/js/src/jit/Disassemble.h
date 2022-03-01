/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Disassemble_h
#define jit_Disassemble_h

#include <stddef.h>
#include <stdint.h>

namespace js {
namespace jit {

using InstrCallback = void (*)(const char* text);

extern bool HasDisassembler();
extern void Disassemble(uint8_t* code, size_t length, InstrCallback callback);

}  // namespace jit
}  // namespace js

#endif /* jit_Disassemble_h */

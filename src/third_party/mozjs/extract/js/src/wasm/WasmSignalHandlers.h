/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2014 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_signal_handlers_h
#define wasm_signal_handlers_h

#include "js/ProfilingFrameIterator.h"
#include "wasm/WasmProcess.h"

namespace js {
namespace wasm {

using RegisterState = JS::ProfilingFrameIterator::RegisterState;

// This function performs the low-overhead signal handler initialization that we
// want to do eagerly to ensure a more-deterministic global process state. This
// is especially relevant for signal handlers since handler ordering depends on
// installation order: the wasm signal handler must run *before* the other crash
// handlers (breakpad) and since POSIX signal handlers work LIFO, this function
// needs to be called at the end of the startup process, after the other two
// handlers have been installed. Currently, this is achieved by having
// JSRuntime() call this function. There can be multiple JSRuntimes per process
// so this function can thus be called multiple times, having no effect after
// the first call.
void EnsureEagerProcessSignalHandlers();

// Assuming EnsureEagerProcessSignalHandlers() has already been called,
// this function performs the full installation of signal handlers which must
// be performed per-thread/JSContext. This operation may incur some overhead and
// so should be done only when needed to use wasm. Currently, this is done in
// wasm::HasPlatformSupport() which is called when deciding whether to expose
// the 'WebAssembly' object on the global object.
bool EnsureFullSignalHandlers(JSContext* cx);

// Return whether, with the given simulator register state, a memory access to
// 'addr' of size 'numBytes' needs to trap and, if so, where the simulator
// should redirect pc to.
bool MemoryAccessTraps(const RegisterState& regs, uint8_t* addr,
                       uint32_t numBytes, uint8_t** newPC);

// Return whether, with the given simulator register state, an illegal
// instruction fault is expected and, if so, the value of the next PC.
bool HandleIllegalInstruction(const RegisterState& regs, uint8_t** newPC);

}  // namespace wasm
}  // namespace js

#endif  // wasm_signal_handlers_h

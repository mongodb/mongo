/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_WarpCacheIRTranspiler_h
#define jit_WarpCacheIRTranspiler_h

#include <initializer_list>

namespace js {

class BytecodeLocation;

namespace jit {

class CallInfo;
class MDefinition;
class WarpBuilder;
class WarpCacheIR;

// Generate MIR from a Baseline ICStub's CacheIR.
[[nodiscard]] bool TranspileCacheIRToMIR(
    WarpBuilder* builder, BytecodeLocation loc,
    const WarpCacheIR* cacheIRSnapshot,
    std::initializer_list<MDefinition*> inputs,
    CallInfo* maybeCallInfo = nullptr);

}  // namespace jit
}  // namespace js

#endif /* jit_WarpCacheIRTranspiler_h */

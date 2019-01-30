/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCacheIRCompiler_h
#define jit_BaselineCacheIRCompiler_h

#include "gc/Barrier.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"

namespace js {
namespace jit {

class ICFallbackStub;
class ICStub;

enum class BaselineCacheIRStubKind { Regular, Monitored, Updated };

ICStub* AttachBaselineCacheIRStub(JSContext* cx, const CacheIRWriter& writer,
                                  CacheKind kind, BaselineCacheIRStubKind stubKind,
                                  ICStubEngine engine, JSScript* outerScript,
                                  ICFallbackStub* stub, bool* attached);

} // namespace jit
} // namespace js

#endif /* jit_BaselineCacheIRCompiler_h */

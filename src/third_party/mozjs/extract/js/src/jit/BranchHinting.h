/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BranchHinting_h
#define jit_BranchHinting_h

// This file represents the wasm Branch Hinting optimization pass

namespace js::jit {

class MIRGenerator;
class MIRGraph;

[[nodiscard]] bool BranchHinting(const MIRGenerator* mir, MIRGraph& graph);

}  // namespace js::jit

#endif /* jit_BranchHinting_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file declares scalar replacement of objects transformation.
#ifndef jit_ScalarReplacement_h
#define jit_ScalarReplacement_h

namespace js {
namespace jit {

class MIRGenerator;
class MIRGraph;

[[nodiscard]] bool ScalarReplacement(MIRGenerator* mir, MIRGraph& graph);

}  // namespace jit
}  // namespace js

#endif /* jit_ScalarReplacement_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_Instance_inl_h
#define wasm_Instance_inl_h

#include "wasm/WasmInstance.h"

#include "wasm/WasmCode.h"

namespace js {
namespace wasm {

const CodeMetadata& Instance::codeMeta() const { return code_->codeMeta(); }
const CodeTailMetadata& Instance::codeTailMeta() const {
  return code_->codeTailMeta();
}
const CodeMetadataForAsmJS* Instance::codeMetaForAsmJS() const {
  return code_->codeMetaForAsmJS();
}

bool Instance::isAsmJS() const { return codeMeta().isAsmJS(); }

}  // namespace wasm
}  // namespace js

#endif  // wasm_Instance_inl_h

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

const CodeTier& Instance::code(Tier t) const { return code_->codeTier(t); }

uint8_t* Instance::codeBase(Tier t) const { return code_->segment(t).base(); }

const MetadataTier& Instance::metadata(Tier t) const {
  return code_->metadata(t);
}

const Metadata& Instance::metadata() const { return code_->metadata(); }

bool Instance::isAsmJS() const { return metadata().isAsmJS(); }

}  // namespace wasm
}  // namespace js

#endif  // wasm_Instance_inl_h

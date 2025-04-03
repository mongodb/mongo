/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
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

#include "wasm/WasmModuleTypes.h"

#include "mozilla/Range.h"

#include "vm/JSAtom.h"
#include "vm/MallocProvider.h"
#include "wasm/WasmUtility.h"

#include "vm/JSAtom-inl.h"

using namespace js;
using namespace js::wasm;

/* static */
CacheableName CacheableName::fromUTF8Chars(UniqueChars&& utf8Chars) {
  size_t length = strlen(utf8Chars.get());
  UTF8Bytes bytes;
  bytes.replaceRawBuffer(utf8Chars.release(), length, length + 1);
  return CacheableName(std::move(bytes));
}

/* static */
bool CacheableName::fromUTF8Chars(const char* utf8Chars, CacheableName* name) {
  size_t utf8CharsLen = strlen(utf8Chars);
  UTF8Bytes bytes;
  if (!bytes.resizeUninitialized(utf8CharsLen)) {
    return false;
  }
  memcpy(bytes.begin(), utf8Chars, utf8CharsLen);
  *name = CacheableName(std::move(bytes));
  return true;
}

JSAtom* CacheableName::toAtom(JSContext* cx) const {
  return AtomizeUTF8Chars(cx, begin(), length());
}

bool CacheableName::toPropertyKey(JSContext* cx,
                                  MutableHandleId propertyKey) const {
  JSAtom* atom = toAtom(cx);
  if (!atom) {
    return false;
  }
  propertyKey.set(AtomToId(atom));
  return true;
}

UniqueChars CacheableName::toQuotedString(JSContext* cx) const {
  RootedString atom(cx, toAtom(cx));
  if (!atom) {
    return nullptr;
  }
  return QuoteString(cx, atom.get());
}

size_t CacheableName::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return bytes_.sizeOfExcludingThis(mallocSizeOf);
}

size_t Import::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return module.sizeOfExcludingThis(mallocSizeOf) +
         field.sizeOfExcludingThis(mallocSizeOf);
}

Export::Export(CacheableName&& fieldName, uint32_t index, DefinitionKind kind)
    : fieldName_(std::move(fieldName)) {
  pod.kind_ = kind;
  pod.index_ = index;
}

Export::Export(CacheableName&& fieldName, DefinitionKind kind)
    : fieldName_(std::move(fieldName)) {
  pod.kind_ = kind;
  pod.index_ = 0;
}

uint32_t Export::funcIndex() const {
  MOZ_ASSERT(pod.kind_ == DefinitionKind::Function);
  return pod.index_;
}

uint32_t Export::globalIndex() const {
  MOZ_ASSERT(pod.kind_ == DefinitionKind::Global);
  return pod.index_;
}

uint32_t Export::tagIndex() const {
  MOZ_ASSERT(pod.kind_ == DefinitionKind::Tag);
  return pod.index_;
}

uint32_t Export::tableIndex() const {
  MOZ_ASSERT(pod.kind_ == DefinitionKind::Table);
  return pod.index_;
}

size_t Export::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return fieldName_.sizeOfExcludingThis(mallocSizeOf);
}

size_t GlobalDesc::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return initial_.sizeOfExcludingThis(mallocSizeOf);
}

bool TagType::initialize(ValTypeVector&& argTypes) {
  MOZ_ASSERT(argTypes_.empty() && argOffsets_.empty() && size_ == 0);

  argTypes_ = std::move(argTypes);
  if (!argOffsets_.resize(argTypes_.length())) {
    return false;
  }

  StructLayout layout;
  for (size_t i = 0; i < argTypes_.length(); i++) {
    CheckedInt32 offset = layout.addField(FieldType(argTypes_[i].packed()));
    if (!offset.isValid()) {
      return false;
    }
    argOffsets_[i] = offset.value();
  }

  CheckedInt32 size = layout.close();
  if (!size.isValid()) {
    return false;
  }
  this->size_ = size.value();

  return true;
}

size_t TagType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return argTypes_.sizeOfExcludingThis(mallocSizeOf) +
         argOffsets_.sizeOfExcludingThis(mallocSizeOf);
}

size_t TagDesc::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return type->sizeOfExcludingThis(mallocSizeOf);
}

size_t ElemSegment::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return SizeOfMaybeExcludingThis(offsetIfActive, mallocSizeOf) +
         elemFuncIndices.sizeOfExcludingThis(mallocSizeOf);
}

size_t DataSegment::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return SizeOfMaybeExcludingThis(offsetIfActive, mallocSizeOf) +
         bytes.sizeOfExcludingThis(mallocSizeOf);
}

size_t CustomSection::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return name.sizeOfExcludingThis(mallocSizeOf) + sizeof(*payload) +
         payload->sizeOfExcludingThis(mallocSizeOf);
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "irregexp/RegExpShim.h"

#include "mozilla/MemoryReporting.h"

#include <iostream>

#include "irregexp/imported/regexp-macro-assembler.h"
#include "irregexp/imported/regexp-stack.h"

#include "vm/NativeObject-inl.h"

namespace v8 {
namespace internal {

void PrintF(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vprintf(format, arguments);
  va_end(arguments);
}

void PrintF(FILE* out, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vfprintf(out, format, arguments);
  va_end(arguments);
}

StdoutStream::operator std::ostream&() const { return std::cerr; }

template <typename T>
std::ostream& StdoutStream::operator<<(T t) {
  return std::cerr << t;
}

template std::ostream& StdoutStream::operator<<(char const* c);

// Origin:
// https://github.com/v8/v8/blob/855591a54d160303349a5f0a32fab15825c708d1/src/utils/ostreams.cc#L120-L169
// (This is a hand-simplified version.)
// Writes the given character to the output escaping everything outside
// of printable ASCII range.
std::ostream& operator<<(std::ostream& os, const AsUC16& c) {
  base::uc16 v = c.value;
  bool isPrint = 0x20 < v && v <= 0x7e;
  char buf[10];
  const char* format = isPrint ? "%c" : (v <= 0xFF) ? "\\x%02x" : "\\u%04x";
  SprintfLiteral(buf, format, v);
  return os << buf;
}
std::ostream& operator<<(std::ostream& os, const AsUC32& c) {
  int32_t v = c.value;
  if (v <= String::kMaxUtf16CodeUnit) {
    return os << AsUC16(v);
  }
  char buf[13];
  SprintfLiteral(buf, "\\u{%06x}", v);
  return os << buf;
}

HandleScope::HandleScope(Isolate* isolate) : isolate_(isolate) {
  isolate->openHandleScope(*this);
}

HandleScope::~HandleScope() {
  isolate_->closeHandleScope(level_, non_gc_level_);
}

template <typename T>
Handle<T>::Handle(T object, Isolate* isolate)
    : location_(isolate->getHandleLocation(object.value())) {}

template Handle<ByteArray>::Handle(ByteArray b, Isolate* isolate);
template Handle<TrustedByteArray>::Handle(TrustedByteArray b, Isolate* isolate);
template Handle<HeapObject>::Handle(const JS::Value& v, Isolate* isolate);
template Handle<IrRegExpData>::Handle(IrRegExpData re, Isolate* isolate);
template Handle<String>::Handle(String s, Isolate* isolate);

template <typename T>
Handle<T>::Handle(const JS::Value& value, Isolate* isolate)
    : location_(isolate->getHandleLocation(value)) {
  T::cast(Object(value));  // Assert that value has the correct type.
}

JS::Value* Isolate::getHandleLocation(const JS::Value& value) {
  js::AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!handleArena_.Append(value)) {
    oomUnsafe.crash("Irregexp handle allocation");
  }
  return &handleArena_.GetLast();
}

void* Isolate::allocatePseudoHandle(size_t bytes) {
  PseudoHandle<void> ptr;
  ptr.reset(js_malloc(bytes));
  if (!ptr) {
    return nullptr;
  }
  if (!uniquePtrArena_.Append(std::move(ptr))) {
    return nullptr;
  }
  return uniquePtrArena_.GetLast().get();
}

template <typename T>
PseudoHandle<T> Isolate::takeOwnership(void* ptr) {
  PseudoHandle<T> result = maybeTakeOwnership<T>(ptr);
  MOZ_ASSERT(result);
  return result;
}

template <typename T>
PseudoHandle<T> Isolate::maybeTakeOwnership(void* ptr) {
  for (auto iter = uniquePtrArena_.IterFromLast(); !iter.Done(); iter.Prev()) {
    auto& entry = iter.Get();
    if (entry.get() == ptr) {
      PseudoHandle<T> result;
      result.reset(static_cast<T*>(entry.release()));
      return result;
    }
  }
  return PseudoHandle<T>();
}

PseudoHandle<ByteArrayData> ByteArray::maybeTakeOwnership(Isolate* isolate) {
  PseudoHandle<ByteArrayData> result =
      isolate->maybeTakeOwnership<ByteArrayData>(value().toPrivate());
  setValue(JS::PrivateValue(nullptr));
  return result;
}

PseudoHandle<ByteArrayData> ByteArray::takeOwnership(Isolate* isolate) {
  PseudoHandle<ByteArrayData> result = maybeTakeOwnership(isolate);
  MOZ_ASSERT(result);
  return result;
}

void Isolate::trace(JSTracer* trc) {
  js::gc::AssertRootMarkingPhase(trc);

  for (auto iter = handleArena_.Iter(); !iter.Done(); iter.Next()) {
    auto& elem = iter.Get();
    JS::GCPolicy<JS::Value>::trace(trc, &elem, "Isolate handle arena");
  }
}

size_t Isolate::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
  size_t size = mallocSizeOf(this);

  size += mallocSizeOf(regexpStack_);
  size += ExternalReference::SizeOfExcludingThis(mallocSizeOf, regexpStack_);

  size += handleArena_.SizeOfExcludingThis(mallocSizeOf);
  size += uniquePtrArena_.SizeOfExcludingThis(mallocSizeOf);
  return size;
}

/*static*/ Handle<String> String::Flatten(Isolate* isolate,
                                          Handle<String> string) {
  if (string->IsFlat()) {
    return string;
  }
  js::AutoEnterOOMUnsafeRegion oomUnsafe;
  JSLinearString* linear = string->str()->ensureLinear(isolate->cx());
  if (!linear) {
    oomUnsafe.crash("Irregexp String::Flatten");
  }
  return Handle<String>(JS::StringValue(linear), isolate);
}

// This is only used for trace messages printing the source pattern of
// a regular expression. We have to return a unique_ptr, but we don't
// care about the contents, so we return an empty null-terminated string.
std::unique_ptr<char[]> String::ToCString() {
  js::AutoEnterOOMUnsafeRegion oomUnsafe;

  std::unique_ptr<char[]> ptr;
  ptr.reset(static_cast<char*>(js_malloc(1)));
  if (!ptr) {
    oomUnsafe.crash("Irregexp String::ToCString");
  }
  ptr[0] = '\0';

  return ptr;
}

bool Isolate::init() {
  regexpStack_ = js_new<RegExpStack>();
  if (!regexpStack_) {
    return false;
  }
  return true;
}

Isolate::~Isolate() {
  if (regexpStack_) {
    js_delete(regexpStack_);
  }
}

/* static */
const void* ExternalReference::TopOfRegexpStack(Isolate* isolate) {
  return reinterpret_cast<const void*>(
      isolate->regexp_stack()->memory_top_address_address());
}

/* static */
size_t ExternalReference::SizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf, RegExpStack* regexpStack) {
  if (regexpStack->thread_local_.owns_memory_) {
    return mallocSizeOf(regexpStack->thread_local_.memory_);
  }
  return 0;
}

Handle<ByteArray> Isolate::NewByteArray(int length, AllocationType alloc) {
  MOZ_RELEASE_ASSERT(length >= 0);

  js::AutoEnterOOMUnsafeRegion oomUnsafe;

  size_t alloc_size = sizeof(ByteArrayData) + length;
  ByteArrayData* data =
      static_cast<ByteArrayData*>(allocatePseudoHandle(alloc_size));
  if (!data) {
    oomUnsafe.crash("Irregexp NewByteArray");
  }
  new (data) ByteArrayData(length);

  return Handle<ByteArray>(JS::PrivateValue(data), this);
}

Handle<TrustedByteArray> Isolate::NewTrustedByteArray(int length,
                                                      AllocationType alloc) {
  MOZ_RELEASE_ASSERT(length >= 0);

  js::AutoEnterOOMUnsafeRegion oomUnsafe;

  size_t alloc_size = sizeof(ByteArrayData) + length;
  ByteArrayData* data =
      static_cast<ByteArrayData*>(allocatePseudoHandle(alloc_size));
  if (!data) {
    oomUnsafe.crash("Irregexp NewTrustedByteArray");
  }
  new (data) ByteArrayData(length);

  return Handle<TrustedByteArray>(JS::PrivateValue(data), this);
}

Handle<FixedArray> Isolate::NewFixedArray(int length) {
  MOZ_RELEASE_ASSERT(length >= 0);
  js::AutoEnterOOMUnsafeRegion oomUnsafe;
  js::ArrayObject* array = js::NewDenseFullyAllocatedArray(cx(), length);
  if (!array) {
    oomUnsafe.crash("Irregexp NewFixedArray");
  }
  array->ensureDenseInitializedLength(0, length);
  return Handle<FixedArray>(JS::ObjectValue(*array), this);
}

template <typename T>
Handle<FixedIntegerArray<T>> Isolate::NewFixedIntegerArray(uint32_t length) {
  MOZ_RELEASE_ASSERT(length < std::numeric_limits<uint32_t>::max() / sizeof(T));
  js::AutoEnterOOMUnsafeRegion oomUnsafe;

  uint32_t rawLength = length * sizeof(T);
  size_t allocSize = sizeof(ByteArrayData) + rawLength;
  ByteArrayData* data =
      static_cast<ByteArrayData*>(allocatePseudoHandle(allocSize));
  if (!data) {
    oomUnsafe.crash("Irregexp NewFixedIntegerArray");
  }
  new (data) ByteArrayData(rawLength);

  return Handle<FixedIntegerArray<T>>(JS::PrivateValue(data), this);
}

template <typename T>
Handle<FixedIntegerArray<T>> FixedIntegerArray<T>::New(Isolate* isolate,
                                                       uint32_t length) {
  return isolate->NewFixedIntegerArray<T>(length);
}

template class FixedIntegerArray<uint16_t>;

template <typename CharT>
Handle<String> Isolate::InternalizeString(
    const base::Vector<const CharT>& str) {
  js::AutoEnterOOMUnsafeRegion oomUnsafe;
  JSAtom* atom = js::AtomizeChars(cx(), str.begin(), str.length());
  if (!atom) {
    oomUnsafe.crash("Irregexp InternalizeString");
  }
  return Handle<String>(JS::StringValue(atom), this);
}

template Handle<String> Isolate::InternalizeString(
    const base::Vector<const uint8_t>& str);
template Handle<String> Isolate::InternalizeString(
    const base::Vector<const char16_t>& str);

static_assert(JSRegExp::RegistersForCaptureCount(JSRegExp::kMaxCaptures) <=
              RegExpMacroAssembler::kMaxRegisterCount);

// This function implements AdvanceStringIndex and CodePointAt:
//  - https://tc39.es/ecma262/#sec-advancestringindex
//  - https://tc39.es/ecma262/#sec-codepointat
// The semantics are to advance 2 code units for properly paired
// surrogates in unicode mode, and 1 code unit otherwise
// (non-surrogates, unpaired surrogates, or non-unicode mode).
uint64_t RegExpUtils::AdvanceStringIndex(Tagged<String> wrappedString,
                                         uint64_t index, bool unicode) {
  MOZ_ASSERT(index < kMaxSafeIntegerUint64);
  MOZ_ASSERT(wrappedString->IsFlat());
  JSLinearString* string = &wrappedString->str()->asLinear();

  if (unicode && index < string->length()) {
    char16_t first = string->latin1OrTwoByteChar(index);
    if (first >= 0xD800 && first <= 0xDBFF && index + 1 < string->length()) {
      char16_t second = string->latin1OrTwoByteChar(index + 1);
      if (second >= 0xDC00 && second <= 0xDFFF) {
        return index + 2;
      }
    }
  }

  return index + 1;
}

// RegexpMacroAssemblerTracer::GetCode dumps the flags by first converting to
// a String, then into a C string. To avoid allocating while assembling,
// we just return a handle to the well-known atom "flags".
Handle<String> JSRegExp::StringFromFlags(Isolate* isolate, RegExpFlags flags) {
  return Handle<String>(String(isolate->cx()->names().flags), isolate);
}

}  // namespace internal
}  // namespace v8

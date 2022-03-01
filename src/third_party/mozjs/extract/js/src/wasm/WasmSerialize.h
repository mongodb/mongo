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

#ifndef wasm_serialize_h
#define wasm_serialize_h

#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"

#include <type_traits>

#include "js/AllocPolicy.h"
#include "js/Vector.h"

namespace js {
namespace wasm {

using mozilla::MallocSizeOf;

// Factor out common serialization, cloning and about:memory size-computation
// functions for reuse when serializing wasm and asm.js modules.

static inline uint8_t* WriteBytes(uint8_t* dst, const void* src,
                                  size_t nbytes) {
  if (nbytes) {
    memcpy(dst, src, nbytes);
  }
  return dst + nbytes;
}

static inline const uint8_t* ReadBytes(const uint8_t* src, void* dst,
                                       size_t nbytes) {
  if (nbytes) {
    memcpy(dst, src, nbytes);
  }
  return src + nbytes;
}

static inline const uint8_t* ReadBytesChecked(const uint8_t* src,
                                              size_t* remain, void* dst,
                                              size_t nbytes) {
  if (*remain < nbytes) {
    return nullptr;
  }
  memcpy(dst, src, nbytes);
  *remain -= nbytes;
  return src + nbytes;
}

template <class T>
static inline uint8_t* WriteScalar(uint8_t* dst, T t) {
  memcpy(dst, &t, sizeof(t));
  return dst + sizeof(t);
}

template <class T>
static inline const uint8_t* ReadScalar(const uint8_t* src, T* dst) {
  memcpy(dst, src, sizeof(*dst));
  return src + sizeof(*dst);
}

template <class T>
static inline const uint8_t* ReadScalarChecked(const uint8_t* src,
                                               size_t* remain, T* dst) {
  if (*remain < sizeof(*dst)) {
    return nullptr;
  }
  memcpy(dst, src, sizeof(*dst));
  *remain -= sizeof(*dst);
  return src + sizeof(*dst);
}

template <class T, size_t N>
static inline size_t SerializedVectorSize(
    const mozilla::Vector<T, N, SystemAllocPolicy>& vec) {
  size_t size = sizeof(uint32_t);
  for (size_t i = 0; i < vec.length(); i++) {
    size += vec[i].serializedSize();
  }
  return size;
}

template <class T, size_t N>
static inline uint8_t* SerializeVector(
    uint8_t* cursor, const mozilla::Vector<T, N, SystemAllocPolicy>& vec) {
  cursor = WriteScalar<uint32_t>(cursor, vec.length());
  for (size_t i = 0; i < vec.length(); i++) {
    cursor = vec[i].serialize(cursor);
  }
  return cursor;
}

template <class T, size_t N>
static inline const uint8_t* DeserializeVector(
    const uint8_t* cursor, mozilla::Vector<T, N, SystemAllocPolicy>* vec) {
  uint32_t length;
  cursor = ReadScalar<uint32_t>(cursor, &length);
  if (!vec->resize(length)) {
    return nullptr;
  }
  for (size_t i = 0; i < vec->length(); i++) {
    if (!(cursor = (*vec)[i].deserialize(cursor))) {
      return nullptr;
    }
  }
  return cursor;
}

template <class T, size_t N>
static inline size_t SizeOfVectorExcludingThis(
    const mozilla::Vector<T, N, SystemAllocPolicy>& vec,
    MallocSizeOf mallocSizeOf) {
  size_t size = vec.sizeOfExcludingThis(mallocSizeOf);
  for (const T& t : vec) {
    size += t.sizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

template <class T>
static inline size_t SerializedMaybeSize(const mozilla::Maybe<T>& maybe) {
  if (!maybe) {
    return sizeof(uint8_t);
  }
  return sizeof(uint8_t) + maybe->serializedSize();
}

template <class T>
static inline uint8_t* SerializeMaybe(uint8_t* cursor,
                                      const mozilla::Maybe<T>& maybe) {
  cursor = WriteScalar<uint8_t>(cursor, maybe ? 1 : 0);
  if (maybe) {
    cursor = maybe->serialize(cursor);
  }
  return cursor;
}

template <class T>
static inline const uint8_t* DeserializeMaybe(const uint8_t* cursor,
                                              mozilla::Maybe<T>* maybe) {
  uint8_t isSome;
  cursor = ReadScalar<uint8_t>(cursor, &isSome);
  if (!cursor) {
    return nullptr;
  }

  if (isSome == 1) {
    maybe->emplace();
    cursor = (*maybe)->deserialize(cursor);
  } else {
    *maybe = mozilla::Nothing();
  }
  return cursor;
}

template <class T>
static inline size_t SizeOfMaybeExcludingThis(const mozilla::Maybe<T>& maybe,
                                              MallocSizeOf mallocSizeOf) {
  return maybe ? maybe->sizeOfExcludingThis(mallocSizeOf) : 0;
}

template <class T, size_t N>
static inline size_t SerializedPodVectorSize(
    const mozilla::Vector<T, N, SystemAllocPolicy>& vec) {
  return sizeof(uint32_t) + vec.length() * sizeof(T);
}

template <class T, size_t N>
static inline uint8_t* SerializePodVector(
    uint8_t* cursor, const mozilla::Vector<T, N, SystemAllocPolicy>& vec) {
  // This binary format must not change without taking into consideration the
  // constraints in Assumptions::serialize.

  cursor = WriteScalar<uint32_t>(cursor, vec.length());
  cursor = WriteBytes(cursor, vec.begin(), vec.length() * sizeof(T));
  return cursor;
}

template <class T, size_t N>
static inline const uint8_t* DeserializePodVector(
    const uint8_t* cursor, mozilla::Vector<T, N, SystemAllocPolicy>* vec) {
  uint32_t length;
  cursor = ReadScalar<uint32_t>(cursor, &length);
  if (!vec->initLengthUninitialized(length)) {
    return nullptr;
  }
  cursor = ReadBytes(cursor, vec->begin(), length * sizeof(T));
  return cursor;
}

template <class T, size_t N>
static inline const uint8_t* DeserializePodVectorChecked(
    const uint8_t* cursor, size_t* remain,
    mozilla::Vector<T, N, SystemAllocPolicy>* vec) {
  uint32_t length;
  cursor = ReadScalarChecked<uint32_t>(cursor, remain, &length);
  if (!cursor || !vec->initLengthUninitialized(length)) {
    return nullptr;
  }
  cursor = ReadBytesChecked(cursor, remain, vec->begin(), length * sizeof(T));
  return cursor;
}

// To call Vector::shrinkStorageToFit , a type must specialize mozilla::IsPod
// which is pretty verbose to do within js::wasm, so factor that process out
// into a macro.

#define WASM_DECLARE_POD_VECTOR(Type, VectorName)   \
  }                                                 \
  }                                                 \
  namespace mozilla {                               \
  template <>                                       \
  struct IsPod<js::wasm::Type> : std::true_type {}; \
  }                                                 \
  namespace js {                                    \
  namespace wasm {                                  \
  typedef Vector<Type, 0, SystemAllocPolicy> VectorName;

// A wasm Module and everything it contains must support serialization and
// deserialization. Some data can be simply copied as raw bytes and,
// as a convention, is stored in an inline CacheablePod struct. Everything else
// should implement the below methods which are called recusively by the
// containing Module.

#define WASM_DECLARE_SERIALIZABLE(Type)              \
  size_t serializedSize() const;                     \
  uint8_t* serialize(uint8_t* cursor) const;         \
  const uint8_t* deserialize(const uint8_t* cursor); \
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

template <class T>
struct SerializableRefPtr : RefPtr<T> {
  using RefPtr<T>::operator=;

  SerializableRefPtr() = default;

  template <class U>
  MOZ_IMPLICIT SerializableRefPtr(U&& u) : RefPtr<T>(std::forward<U>(u)) {}

  WASM_DECLARE_SERIALIZABLE(SerializableRefPtr)
};

template <class T>
inline size_t SerializableRefPtr<T>::serializedSize() const {
  return (*this)->serializedSize();
}

template <class T>
inline uint8_t* SerializableRefPtr<T>::serialize(uint8_t* cursor) const {
  return (*this)->serialize(cursor);
}

template <class T>
inline const uint8_t* SerializableRefPtr<T>::deserialize(
    const uint8_t* cursor) {
  auto* t = js_new<std::remove_const_t<T>>();
  *this = t;
  return t->deserialize(cursor);
}

template <class T>
inline size_t SerializableRefPtr<T>::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return (*this)->sizeOfExcludingThis(mallocSizeOf);
}

}  // namespace wasm
}  // namespace js

#endif  // wasm_serialize_h

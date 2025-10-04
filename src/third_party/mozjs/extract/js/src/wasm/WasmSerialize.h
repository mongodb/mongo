/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2022 Mozilla Foundation
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

#include "mozilla/CheckedInt.h"
#include "mozilla/MacroForEach.h"
#include "mozilla/Maybe.h"
#include "mozilla/Result.h"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace js {
namespace wasm {

class TypeContext;

// [SMDOC] "Module serialization"
//
// A wasm::Module may be serialized to a binary format that allows for quick
// reloads of a previous compiled wasm binary.
//
// The binary format is optimized for encoding/decoding speed, not size. There
// is no formal specification, and no backwards/forwards compatibility
// guarantees. The prelude of the encoding contains a 'build ID' which must be
// used when reading from a cache entry to determine if it is valid.
//
// Module serialization and deserialization are performed using templated
// functions that allow for (imperfect) abstraction over whether we are decoding
// or encoding the module. It can be viewed as a specialization of the visitor
// pattern.
//
// Each module data structure is visited by a function parameterized by the
// "mode", which may be either:
//  1. MODE_SIZE - We are computing the final encoding size, before encoding it
//  2. MODE_ENCODE - We are actually encoding the module to bytes
//  3. MODE_DECODE - We are decoding the module from bytes
//
// These functions are called "coding" functions, as they are generic to whether
// we are "encoding" or "decoding". The verb tense "code" is used for the
// prefix.
//
// Each coding function takes the item being visited, along with a "Coder"
// which contains the state needed for each mode. This is either a buffer span
// or an accumulated length. The coding function either manipulates the Coder
// directly or delegates to its field's coding functions.
//
// Leaf data types are usually just copied directly to and from memory using a
// generic "CodePod" function. See the "cacheable POD" documentation in this
// file for more information.
//
// Non-leaf data types need an explicit coding function. This function can
// usually be completely generic to decoding/encoding, and delegate to the
// coding functions for each field. Separate decoding/encoding functions may
// be needed when decoding requires initialization logic, such as constructors.
// In this case, it is critical that both functions agree on the fields to be
// coded, and the order they are coded in.
//
// Coding functions are defined as free functions in "WasmSerialize.cpp". When
// they require access to protected state in a type, they may use the
// WASM_DECLARE_FRIEND_SERIALIZE macro.

// Signal an out of memory condition
struct OutOfMemory {};

// The result of serialization, either OK or OOM
using CoderResult = mozilla::Result<mozilla::Ok, OutOfMemory>;

// CoderMode parameterizes the coding functions
enum CoderMode {
  // We are computing the final size of the encoded buffer. This is a discrete
  // pass that runs before encoding.
  MODE_SIZE,
  // We are encoding the module to bytes.
  MODE_ENCODE,
  // We are decoding the module from bytes.
  MODE_DECODE,
};

// Coding functions take a different argument depending on which CoderMode
// they are invoked with:
//   * MODE_SIZE - const T*
//   * MODE_ENCODE - const T*
//   * MODE_DECODE - T*
//
// The CoderArg<mode, T> type alias is used to acquire the proper type for
// coding function arguments.
template <CoderMode mode, typename V>
struct CoderArgT;

template <typename V>
struct CoderArgT<MODE_SIZE, V> {
  using T = const V*;
};

template <typename V>
struct CoderArgT<MODE_DECODE, V> {
  using T = V*;
};

template <typename V>
struct CoderArgT<MODE_ENCODE, V> {
  using T = const V*;
};

template <CoderMode mode, typename T>
using CoderArg = typename CoderArgT<mode, T>::T;

// Coder is the state provided to all coding functions during module traversal.
template <CoderMode mode>
struct Coder;

// A Coder<MODE_SIZE> computes the total encoded size of a module
template <>
struct Coder<MODE_SIZE> {
  explicit Coder(const TypeContext* types) : types_(types), size_(0) {}

  // The types of the module that we're going to encode. This is required in
  // order to encode the original index of types that we encounter.
  const TypeContext* types_;

  // The current size of buffer required to serialize this module.
  mozilla::CheckedInt<size_t> size_;

  // This function shares a signature with MODE_ENCODE to allow functions to be
  // generic across MODE_SIZE/MODE_ENCODE, even though the src pointer is not
  // needed for MODE_SIZE.
  CoderResult writeBytes(const void* unusedSrc, size_t length);
};

// A Coder<MODE_ENCODE> holds the buffer being written to
template <>
struct Coder<MODE_ENCODE> {
  Coder(const TypeContext* types, uint8_t* start, size_t length)
      : types_(types), buffer_(start), end_(start + length) {}

  // The types of the module that we're encoding. This is required in
  // order to encode the original index of types that we encounter.
  const TypeContext* types_;

  // The current position in the buffer we're writing to.
  uint8_t* buffer_;
  // The end position in the buffer we're writing to.
  const uint8_t* end_;

  CoderResult writeBytes(const void* src, size_t length);
};

// A Coder<MODE_DECODE> holds the buffer being read from
template <>
struct Coder<MODE_DECODE> {
  Coder(const uint8_t* start, size_t length)
      : types_(nullptr), buffer_(start), end_(start + length) {}

  // The types of the module that we're decoding. This is null until the types
  // of this module are decoded.
  const TypeContext* types_;

  // The current position in the buffer we're reading from.
  const uint8_t* buffer_;
  // The end position in the buffer we're reading from.
  const uint8_t* end_;

  CoderResult readBytes(void* dest, size_t length);
};

// Macros to help types declare friendship with a coding function

#define WASM_DECLARE_FRIEND_SERIALIZE(TYPE) \
  template <CoderMode mode>                 \
  friend CoderResult Code##TYPE(Coder<mode>&, CoderArg<mode, TYPE>);

#define WASM_DECLARE_FRIEND_SERIALIZE_ARGS(TYPE, ...) \
  template <CoderMode mode>                               \
  friend CoderResult Code##TYPE(Coder<mode>&, CoderArg<mode, TYPE>, __VA_ARGS__);

// [SMDOC] "Cacheable POD"
//
// Module serialization relies on copying simple structs to and from the
// cache format. We need a way to ensure that we only do this on types that are
// "safe". We call this "cacheable POD". Note: this is not the same thing as
// "POD" as that may contain pointers, which are not cacheable.
//
// We define cacheable POD (C-POD) recursively upon types:
//   1. any integer type is C-POD
//   2. any floating point type is C-POD
//   3. any enum type is C-POD
//   4. any mozilla::Maybe<T> with T: C-POD is C-POD
//   5. any T[N] with T: C-POD is C-POD
//   6. any union where all fields are C-POD is C-POD
//   7. any struct with the following conditions must is C-POD
//      * every field's type must be C-POD
//      * the parent type, if it exists, must also be C-POD
//      * there must be no virtual methods
//
// There are no combination of C++ type traits at this time that can
// automatically meet these criteria, so we are rolling our own system.
//
// We define a "IsCacheablePod" type trait, with builtin rules for cases (1-5).
// The complex cases (6-7) are handled using manual declaration and checking
// macros that must be used upon structs and unions that are considered
// cacheable POD.
//
// See the following macros for details:
//   - WASM_DECLARE_CACHEABLE_POD
//   - WASM_CHECK_CACHEABLE_POD[_WITH_PARENT]

// The IsCacheablePod type trait primary template. Contains the rules for
// (cases 1-3).
template <typename T>
struct IsCacheablePod
    : public std::conditional_t<std::is_arithmetic_v<T> || std::is_enum_v<T>,
                                std::true_type, std::false_type> {};

// Partial specialization for (case 4).
template <typename T>
struct IsCacheablePod<mozilla::Maybe<T>>
    : public std::conditional_t<IsCacheablePod<T>::value, std::true_type,
                                std::false_type> {};

// Partial specialization for (case 5).
template <typename T, size_t N>
struct IsCacheablePod<T[N]>
    : public std::conditional_t<IsCacheablePod<T>::value, std::true_type,
                                std::false_type> {};

template <class T>
inline constexpr bool is_cacheable_pod = IsCacheablePod<T>::value;

// Checks if derrived class will not use the structure alignment for its
// next field. It used when pod is a base class.
#define WASM_CHECK_CACHEABLE_POD_PADDING(Type)                \
  class __CHECK_PADING_##Type : public Type {                 \
   public:                                                    \
    char c;                                                   \
  };                                                          \
  static_assert(sizeof(__CHECK_PADING_##Type) > sizeof(Type), \
                #Type " will overlap with next field if inherited");

// Declare the type 'Type' to be cacheable POD. The definition of the type must
// contain a WASM_CHECK_CACHEABLE_POD[_WITH_PARENT] to ensure all fields of the
// type are cacheable POD.
#define WASM_DECLARE_CACHEABLE_POD(Type)                                      \
  static_assert(!std::is_polymorphic_v<Type>,                                 \
                #Type "must not have virtual methods");                       \
  } /* namespace wasm */                                                      \
  } /* namespace js */                                                        \
  template <>                                                                 \
  struct js::wasm::IsCacheablePod<js::wasm::Type> : public std::true_type {}; \
  namespace js {                                                              \
  namespace wasm {

// Helper: check each field's type to be cacheable POD
#define WASM_CHECK_CACHEABLE_POD_FIELD_(Field)                    \
  static_assert(js::wasm::IsCacheablePod<decltype(Field)>::value, \
                #Field " must be cacheable pod");

// Check every field in a type definition to ensure they are cacheable POD.
#define WASM_CHECK_CACHEABLE_POD(...) \
  MOZ_FOR_EACH(WASM_CHECK_CACHEABLE_POD_FIELD_, (), (__VA_ARGS__))

// Check every field in a type definition to ensure they are cacheable POD, and
// check that the parent class is also cacheable POD.
#define WASM_CHECK_CACHEABLE_POD_WITH_PARENT(Parent, ...) \
  static_assert(js::wasm::IsCacheablePod<Parent>::value,        \
                #Parent " must be cacheable pod");              \
  MOZ_FOR_EACH(WASM_CHECK_CACHEABLE_POD_FIELD_, (), (__VA_ARGS__))

// Allow fields that are not cacheable POD but are believed to be safe for
// serialization due to some justification.
#define WASM_ALLOW_NON_CACHEABLE_POD_FIELD(Field, Reason)          \
  static_assert(!js::wasm::IsCacheablePod<decltype(Field)>::value, \
                #Field " is not cacheable due to " Reason);

}  // namespace wasm
}  // namespace js

#endif  // wasm_serialize_h

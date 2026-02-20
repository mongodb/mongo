/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PropertyResult_h
#define vm_PropertyResult_h

#include "mozilla/Assertions.h"

#include "vm/PropertyInfo.h"

namespace js {

class PropertyResult {
  enum class Kind : uint8_t {
    NotFound,
    NativeProperty,
    NonNativeProperty,
    DenseElement,
    TypedArrayElement,
  };
  enum class IgnoreProtoChain : uint8_t {
    No,
    RecursiveResolve,
    TypedArrayOutOfRange,
  };
  union {
    // Set if kind is NativeProperty.
    PropertyInfo propInfo_;
    // Set if kind is DenseElement.
    uint32_t denseIndex_;
    // Set if kind is TypedArrayElement.
    size_t typedArrayIndex_;
  };
  Kind kind_ = Kind::NotFound;
  IgnoreProtoChain ignoreProtoChain_ = IgnoreProtoChain::No;

 public:
  // Note: because PropertyInfo does not have a default constructor, we can't
  // use |= default| here.
  PropertyResult() {}

  // When a property is not found, we may additionally indicate that the
  // prototype chain should be ignored. This occurs for:
  //  - An out-of-range numeric property on a TypedArrayObject.
  //  - A resolve hook recursively calling itself as it sets the property.
  bool isNotFound() const { return kind_ == Kind::NotFound; }
  bool shouldIgnoreProtoChain() const {
    MOZ_ASSERT(isNotFound());
    return ignoreProtoChain_ != IgnoreProtoChain::No;
  }
  bool isTypedArrayOutOfRange() const {
    MOZ_ASSERT(isNotFound());
    return ignoreProtoChain_ == IgnoreProtoChain::TypedArrayOutOfRange;
  }

  bool isFound() const { return kind_ != Kind::NotFound; }
  bool isNonNativeProperty() const { return kind_ == Kind::NonNativeProperty; }
  bool isDenseElement() const { return kind_ == Kind::DenseElement; }
  bool isTypedArrayElement() const { return kind_ == Kind::TypedArrayElement; }
  bool isNativeProperty() const { return kind_ == Kind::NativeProperty; }

  PropertyInfo propertyInfo() const {
    MOZ_ASSERT(isNativeProperty());
    return propInfo_;
  }

  uint32_t denseElementIndex() const {
    MOZ_ASSERT(isDenseElement());
    return denseIndex_;
  }

  size_t typedArrayElementIndex() const {
    MOZ_ASSERT(isTypedArrayElement());
    return typedArrayIndex_;
  }

  void setNotFound() { kind_ = Kind::NotFound; }

  void setNativeProperty(PropertyInfo prop) {
    kind_ = Kind::NativeProperty;
    propInfo_ = prop;
  }

  void setProxyProperty() { kind_ = Kind::NonNativeProperty; }

  void setDenseElement(uint32_t index) {
    kind_ = Kind::DenseElement;
    denseIndex_ = index;
  }

  void setTypedArrayElement(size_t index) {
    kind_ = Kind::TypedArrayElement;
    typedArrayIndex_ = index;
  }

  void setTypedArrayOutOfRange() {
    kind_ = Kind::NotFound;
    ignoreProtoChain_ = IgnoreProtoChain::TypedArrayOutOfRange;
  }
  void setRecursiveResolve() {
    kind_ = Kind::NotFound;
    ignoreProtoChain_ = IgnoreProtoChain::RecursiveResolve;
  }
};

}  // namespace js

#endif /* vm_PropertyResult_h */

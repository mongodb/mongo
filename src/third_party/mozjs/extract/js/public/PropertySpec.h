/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Property descriptors and flags. */

#ifndef js_PropertySpec_h
#define js_PropertySpec_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT{,_IF}

#include <stddef.h>     // size_t
#include <stdint.h>     // uint8_t, uint16_t, int32_t, uint32_t, uintptr_t
#include <type_traits>  // std::enable_if

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CallArgs.h"            // JSNative
#include "js/PropertyDescriptor.h"  // JSPROP_*
#include "js/RootingAPI.h"          // JS::MutableHandle
#include "js/Symbol.h"              // JS::SymbolCode, PropertySpecNameIsSymbol
#include "js/Value.h"               // JS::Value

struct JS_PUBLIC_API JSContext;
class JSJitInfo;

/**
 * Wrapper to relace JSNative for JSPropertySpecs and JSFunctionSpecs. This will
 * allow us to pass one JSJitInfo per function with the property/function spec,
 * without additional field overhead.
 */
struct JSNativeWrapper {
  JSNative op = nullptr;
  const JSJitInfo* info = nullptr;

  JSNativeWrapper() = default;

  JSNativeWrapper(const JSNativeWrapper& other) = default;

  constexpr JSNativeWrapper(JSNative op, const JSJitInfo* info)
      : op(op), info(info) {}
};

/**
 * Description of a property. JS_DefineProperties and JS_InitClass take arrays
 * of these and define many properties at once. JS_PSG, JS_PSGS and JS_PS_END
 * are helper macros for defining such arrays.
 */
struct JSPropertySpec {
  struct SelfHostedWrapper {
    // The same type as JSNativeWrapper's first field, so that the access in
    // JSPropertySpec::checkAccessorsAreSelfHosted become valid.
    JSNative unused = nullptr;

    const char* funname;

    SelfHostedWrapper() = delete;

    explicit constexpr SelfHostedWrapper(const char* funname)
        : funname(funname) {}
  };

  struct ValueWrapper {
    enum class Type : uint8_t { String, Int32, Double };
    Type type;
    union {
      const char* string;
      int32_t int32;
      double double_;
    };

   private:
    ValueWrapper() = delete;

    explicit constexpr ValueWrapper(int32_t n) : type(Type::Int32), int32(n) {}

    explicit constexpr ValueWrapper(const char* s)
        : type(Type::String), string(s) {}

    explicit constexpr ValueWrapper(double d)
        : type(Type::Double), double_(d) {}

   public:
    ValueWrapper(const ValueWrapper& other) = default;

    static constexpr ValueWrapper int32Value(int32_t n) {
      return ValueWrapper(n);
    }

    static constexpr ValueWrapper stringValue(const char* s) {
      return ValueWrapper(s);
    }

    static constexpr ValueWrapper doubleValue(double d) {
      return ValueWrapper(d);
    }
  };

  union Accessor {
    JSNativeWrapper native;
    SelfHostedWrapper selfHosted;

   private:
    Accessor() = delete;

    constexpr Accessor(JSNative op, const JSJitInfo* info) : native(op, info) {}

    explicit constexpr Accessor(const char* funname) : selfHosted(funname) {}

   public:
    Accessor(const Accessor& other) = default;

    static constexpr Accessor nativeAccessor(JSNative op,
                                             const JSJitInfo* info = nullptr) {
      return Accessor(op, info);
    }

    static constexpr Accessor selfHostedAccessor(const char* funname) {
      return Accessor(funname);
    }

    static constexpr Accessor noAccessor() {
      return Accessor(nullptr, nullptr);
    }
  };

  union AccessorsOrValue {
    struct Accessors {
      Accessor getter;
      Accessor setter;

      constexpr Accessors(Accessor getter, Accessor setter)
          : getter(getter), setter(setter) {}
    } accessors;
    ValueWrapper value;

   private:
    AccessorsOrValue() = delete;

    constexpr AccessorsOrValue(Accessor getter, Accessor setter)
        : accessors(getter, setter) {}

    explicit constexpr AccessorsOrValue(ValueWrapper value) : value(value) {}

   public:
    AccessorsOrValue(const AccessorsOrValue& other) = default;

    static constexpr AccessorsOrValue fromAccessors(Accessor getter,
                                                    Accessor setter) {
      return AccessorsOrValue(getter, setter);
    }

    static constexpr AccessorsOrValue fromValue(ValueWrapper value) {
      return AccessorsOrValue(value);
    }
  };

  union Name {
   private:
    const char* string_;
    uintptr_t symbol_;

   public:
    Name() = delete;

    explicit constexpr Name(const char* str) : string_(str) {}
    explicit constexpr Name(JS::SymbolCode symbol)
        : symbol_(uint32_t(symbol) + 1) {}

    explicit operator bool() const { return !!symbol_; }

    bool isSymbol() const { return JS::PropertySpecNameIsSymbol(symbol_); }
    JS::SymbolCode symbol() const {
      MOZ_ASSERT(isSymbol());
      return JS::SymbolCode(symbol_ - 1);
    }

    bool isString() const { return !isSymbol(); }
    const char* string() const {
      MOZ_ASSERT(isString());
      return string_;
    }
  };

  Name name;

 private:
  // JSPROP_* property attributes as defined in PropertyDescriptor.h.
  uint8_t attributes_;

  // Whether AccessorsOrValue below stores a value, JSNative accessors, or
  // self-hosted accessors.
  enum class Kind : uint8_t { Value, SelfHostedAccessor, NativeAccessor };
  Kind kind_;

 public:
  AccessorsOrValue u;

 private:
  JSPropertySpec() = delete;

  constexpr JSPropertySpec(const char* name, uint8_t attributes, Kind kind,
                           AccessorsOrValue u)
      : name(name), attributes_(attributes), kind_(kind), u(u) {}
  constexpr JSPropertySpec(JS::SymbolCode name, uint8_t attributes, Kind kind,
                           AccessorsOrValue u)
      : name(name), attributes_(attributes), kind_(kind), u(u) {}

 public:
  JSPropertySpec(const JSPropertySpec& other) = default;

  static constexpr JSPropertySpec nativeAccessors(
      const char* name, uint8_t attributes, JSNative getter,
      const JSJitInfo* getterInfo, JSNative setter = nullptr,
      const JSJitInfo* setterInfo = nullptr) {
    return JSPropertySpec(
        name, attributes, Kind::NativeAccessor,
        AccessorsOrValue::fromAccessors(
            JSPropertySpec::Accessor::nativeAccessor(getter, getterInfo),
            JSPropertySpec::Accessor::nativeAccessor(setter, setterInfo)));
  }

  static constexpr JSPropertySpec nativeAccessors(
      JS::SymbolCode name, uint8_t attributes, JSNative getter,
      const JSJitInfo* getterInfo, JSNative setter = nullptr,
      const JSJitInfo* setterInfo = nullptr) {
    return JSPropertySpec(
        name, attributes, Kind::NativeAccessor,
        AccessorsOrValue::fromAccessors(
            JSPropertySpec::Accessor::nativeAccessor(getter, getterInfo),
            JSPropertySpec::Accessor::nativeAccessor(setter, setterInfo)));
  }

  static constexpr JSPropertySpec selfHostedAccessors(
      const char* name, uint8_t attributes, const char* getterName,
      const char* setterName = nullptr) {
    return JSPropertySpec(
        name, attributes, Kind::SelfHostedAccessor,
        AccessorsOrValue::fromAccessors(
            JSPropertySpec::Accessor::selfHostedAccessor(getterName),
            setterName
                ? JSPropertySpec::Accessor::selfHostedAccessor(setterName)
                : JSPropertySpec::Accessor::noAccessor()));
  }

  static constexpr JSPropertySpec selfHostedAccessors(
      JS::SymbolCode name, uint8_t attributes, const char* getterName,
      const char* setterName = nullptr) {
    return JSPropertySpec(
        name, attributes, Kind::SelfHostedAccessor,
        AccessorsOrValue::fromAccessors(
            JSPropertySpec::Accessor::selfHostedAccessor(getterName),
            setterName
                ? JSPropertySpec::Accessor::selfHostedAccessor(setterName)
                : JSPropertySpec::Accessor::noAccessor()));
  }

  static constexpr JSPropertySpec int32Value(const char* name,
                                             uint8_t attributes, int32_t n) {
    return JSPropertySpec(name, attributes, Kind::Value,
                          AccessorsOrValue::fromValue(
                              JSPropertySpec::ValueWrapper::int32Value(n)));
  }

  static constexpr JSPropertySpec int32Value(JS::SymbolCode name,
                                             uint8_t attributes, int32_t n) {
    return JSPropertySpec(name, attributes, Kind::Value,
                          AccessorsOrValue::fromValue(
                              JSPropertySpec::ValueWrapper::int32Value(n)));
  }

  static constexpr JSPropertySpec stringValue(const char* name,
                                              uint8_t attributes,
                                              const char* s) {
    return JSPropertySpec(name, attributes, Kind::Value,
                          AccessorsOrValue::fromValue(
                              JSPropertySpec::ValueWrapper::stringValue(s)));
  }

  static constexpr JSPropertySpec stringValue(JS::SymbolCode name,
                                              uint8_t attributes,
                                              const char* s) {
    return JSPropertySpec(name, attributes, Kind::Value,
                          AccessorsOrValue::fromValue(
                              JSPropertySpec::ValueWrapper::stringValue(s)));
  }

  static constexpr JSPropertySpec doubleValue(const char* name,
                                              uint8_t attributes, double d) {
    return JSPropertySpec(name, attributes, Kind::Value,
                          AccessorsOrValue::fromValue(
                              JSPropertySpec::ValueWrapper::doubleValue(d)));
  }

  static constexpr JSPropertySpec sentinel() {
    return JSPropertySpec(nullptr, 0, Kind::NativeAccessor,
                          AccessorsOrValue::fromAccessors(
                              JSPropertySpec::Accessor::noAccessor(),
                              JSPropertySpec::Accessor::noAccessor()));
  }

  unsigned attributes() const { return attributes_; }

  bool isAccessor() const {
    return (kind_ == Kind::NativeAccessor || kind_ == Kind::SelfHostedAccessor);
  }

  JS_PUBLIC_API bool getValue(JSContext* cx,
                              JS::MutableHandle<JS::Value> value) const;

  bool isSelfHosted() const {
    MOZ_ASSERT(isAccessor());
#ifdef DEBUG
    // Verify that our accessors match our Kind.
    if (kind_ == Kind::SelfHostedAccessor) {
      checkAccessorsAreSelfHosted();
    } else {
      checkAccessorsAreNative();
    }
#endif
    return kind_ == Kind::SelfHostedAccessor;
  }

  static_assert(sizeof(SelfHostedWrapper) == sizeof(JSNativeWrapper),
                "JSPropertySpec::getter/setter must be compact");
  static_assert(offsetof(SelfHostedWrapper, unused) ==
                        offsetof(JSNativeWrapper, op) &&
                    offsetof(SelfHostedWrapper, funname) ==
                        offsetof(JSNativeWrapper, info),
                "checkAccessorsAreNative below require that "
                "SelfHostedWrapper::funname overlay "
                "JSNativeWrapper::info and "
                "SelfHostedWrapper::unused overlay "
                "JSNativeWrapper::op");

 private:
  void checkAccessorsAreNative() const {
    // We may have a getter or a setter or both.  And whichever ones we have
    // should not have a SelfHostedWrapper for the accessor.
    MOZ_ASSERT_IF(u.accessors.getter.native.info, u.accessors.getter.native.op);
    MOZ_ASSERT_IF(u.accessors.setter.native.info, u.accessors.setter.native.op);
  }

  void checkAccessorsAreSelfHosted() const {
    MOZ_ASSERT(!u.accessors.getter.selfHosted.unused);
    MOZ_ASSERT(!u.accessors.setter.selfHosted.unused);
  }
};

// There can be many JSPropertySpec instances so verify the size is what we
// expect:
//
// - Name (1 word)
// - attributes_ + isAccessor_ (1 word)
// - AccessorsOrValue (4 words, native + JSJitInfo for both getter and setter)
static_assert(sizeof(JSPropertySpec) == 6 * sizeof(uintptr_t));

template <unsigned Attributes>
constexpr uint8_t CheckAccessorAttrs() {
  static_assert((Attributes & ~(JSPROP_ENUMERATE | JSPROP_PERMANENT)) == 0,
                "Unexpected flag (not JSPROP_ENUMERATE or JSPROP_PERMANENT)");
  return uint8_t(Attributes);
}

#define JS_PSG(name, getter, attributes)                                  \
  JSPropertySpec::nativeAccessors(name, CheckAccessorAttrs<attributes>(), \
                                  getter, nullptr)
#define JS_PSGS(name, getter, setter, attributes)                         \
  JSPropertySpec::nativeAccessors(name, CheckAccessorAttrs<attributes>(), \
                                  getter, nullptr, setter, nullptr)
#define JS_SYM_GET(symbol, getter, attributes)                              \
  JSPropertySpec::nativeAccessors(::JS::SymbolCode::symbol,                 \
                                  CheckAccessorAttrs<attributes>(), getter, \
                                  nullptr)
#define JS_SYM_GETSET(symbol, getter, setter, attributes)                   \
  JSPropertySpec::nativeAccessors(::JS::SymbolCode::symbol,                 \
                                  CheckAccessorAttrs<attributes>(), getter, \
                                  nullptr, setter, nullptr)
#define JS_SELF_HOSTED_GET(name, getterName, attributes)                      \
  JSPropertySpec::selfHostedAccessors(name, CheckAccessorAttrs<attributes>(), \
                                      getterName)
#define JS_SELF_HOSTED_GETSET(name, getterName, setterName, attributes)       \
  JSPropertySpec::selfHostedAccessors(name, CheckAccessorAttrs<attributes>(), \
                                      getterName, setterName)
#define JS_SELF_HOSTED_SYM_GET(symbol, getterName, attributes) \
  JSPropertySpec::selfHostedAccessors(                         \
      ::JS::SymbolCode::symbol, CheckAccessorAttrs<attributes>(), getterName)
#define JS_STRING_PS(name, string, attributes) \
  JSPropertySpec::stringValue(name, attributes, string)
#define JS_STRING_SYM_PS(symbol, string, attributes) \
  JSPropertySpec::stringValue(::JS::SymbolCode::symbol, attributes, string)
#define JS_INT32_PS(name, value, attributes) \
  JSPropertySpec::int32Value(name, attributes, value)
#define JS_DOUBLE_PS(name, value, attributes) \
  JSPropertySpec::doubleValue(name, attributes, value)
#define JS_PS_END JSPropertySpec::sentinel()

/**
 * To define a native function, set call to a JSNativeWrapper. To define a
 * self-hosted function, set selfHostedName to the name of a function
 * compiled during JSRuntime::initSelfHosting.
 */
struct JSFunctionSpec {
  using Name = JSPropertySpec::Name;

  Name name;
  JSNativeWrapper call;
  uint16_t nargs;
  uint16_t flags;
  const char* selfHostedName;

  // JSPROP_* property attributes as defined in PropertyDescriptor.h
  unsigned attributes() const { return flags; }
};

/*
 * Terminating sentinel initializer to put at the end of a JSFunctionSpec array
 * that's passed to JS_DefineFunctions or JS_InitClass.
 */
#define JS_FS_END JS_FN(nullptr, nullptr, 0, 0)

/*
 * Initializer macros for a JSFunctionSpec array element.
 *
 * - JS_FNINFO allows the simple adding of JSJitInfos.
 * - JS_SELF_HOSTED_FN declares a self-hosted function.
 * - JS_INLINABLE_FN allows specifying an InlinableNative enum value for natives
 *   inlined or specialized by the JIT.
 * - JS_TRAMPOLINE_FN allows specifying a TrampolineNative enum value for
 *   natives that have a JitEntry trampoline.
 * - JS_FNSPEC has slots for all the fields.
 *
 * The _SYM variants allow defining a function with a symbol key rather than a
 * string key. For example, use JS_SYM_FN(iterator, ...) to define an
 * @@iterator method.
 */
#define JS_FN(name, call, nargs, flags) \
  JS_FNSPEC(name, call, nullptr, nargs, flags, nullptr)
#define JS_INLINABLE_FN(name, call, nargs, flags, native) \
  JS_FNSPEC(name, call, &js::jit::JitInfo_##native, nargs, flags, nullptr)
#define JS_TRAMPOLINE_FN(name, call, nargs, flags, native) \
  JS_FNSPEC(name, call, &js::jit::JitInfo_##native, nargs, flags, nullptr)
#define JS_SYM_FN(symbol, call, nargs, flags) \
  JS_SYM_FNSPEC(symbol, call, nullptr, nargs, flags, nullptr)
#define JS_FNINFO(name, call, info, nargs, flags) \
  JS_FNSPEC(name, call, info, nargs, flags, nullptr)
#define JS_SELF_HOSTED_FN(name, selfHostedName, nargs, flags) \
  JS_FNSPEC(name, nullptr, nullptr, nargs, flags, selfHostedName)
#define JS_SELF_HOSTED_SYM_FN(symbol, selfHostedName, nargs, flags) \
  JS_SYM_FNSPEC(symbol, nullptr, nullptr, nargs, flags, selfHostedName)
#define JS_SYM_FNSPEC(symbol, call, info, nargs, flags, selfHostedName) \
  JS_FNSPEC(::JS::SymbolCode::symbol, call, info, nargs, flags, selfHostedName)
#define JS_FNSPEC(name, call, info, nargs, flags, selfHostedName) \
  {JSFunctionSpec::Name(name), {call, info}, nargs, flags, selfHostedName}

#endif  // js_PropertySpec_h

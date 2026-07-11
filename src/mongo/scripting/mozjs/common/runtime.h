// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <jsapi.h>

#include <js/Id.h>

namespace mongo {
class Status;
}  // namespace mongo

namespace mongo::mozjs {

template <typename T>
class WrapType;

enum class InternedString;

struct BinDataInfo;
struct BSONInfo;
struct CodeInfo;
struct DBPointerInfo;
struct DBRefInfo;
struct ErrorInfo;
struct MaxKeyInfo;
struct MinKeyInfo;
struct MongoStatusInfo;
struct NativeFunctionInfo;
struct NumberDecimalInfo;
struct NumberIntInfo;
struct NumberLongInfo;
struct OIDInfo;
struct RegExpInfo;
struct TimestampInfo;

/**
 * Abstract interface for the JS runtime services shared by both the shell
 * (MozJSImplScope) and the WASM sandbox (wasm::MozJSScriptEngine).
 *
 * Provides access to common BSON type prototypes, interned strings, GC,
 * status propagation, and ASAN pointer tracking.
 *
 * The concrete shell scope composes
 * the hierarchy as:
 *
 *   MozJSImplScope : public Scope,
 *                    public MozJSShellRuntimeInterface,
 *                    private MozJSCommonRuntimeInterface
 *
 * A pointer to this interface is stored in JSContext private data and
 * retrieved via getCommonRuntime(cx).
 */
struct [[MONGO_MOD_PUBLIC]] MozJSCommonRuntimeInterface {
    virtual ~MozJSCommonRuntimeInterface() = default;

    virtual void gc() = 0;

    virtual void sleep(Milliseconds ms) = 0;

    virtual std::size_t getGeneration() const = 0;

    virtual JS::HandleId getInternedStringId(InternedString name) = 0;

    virtual std::int64_t* trackedNewInt64(std::int64_t value) = 0;

    virtual WrapType<NumberLongInfo>& numberLongProto() = 0;
    virtual WrapType<NumberIntInfo>& numberIntProto() = 0;
    virtual WrapType<NumberDecimalInfo>& numberDecimalProto() = 0;
    virtual WrapType<OIDInfo>& oidProto() = 0;
    virtual WrapType<BinDataInfo>& binDataProto() = 0;
    virtual WrapType<TimestampInfo>& timestampProto() = 0;
    virtual WrapType<MaxKeyInfo>& maxKeyProto() = 0;
    virtual WrapType<MinKeyInfo>& minKeyProto() = 0;
    virtual WrapType<CodeInfo>& codeProto() = 0;
    virtual WrapType<DBPointerInfo>& dbPointerProto() = 0;
    virtual WrapType<NativeFunctionInfo>& nativeFunctionProto() = 0;
    virtual WrapType<ErrorInfo>& errorProto() = 0;
    virtual WrapType<MongoStatusInfo>& mongoStatusProto() = 0;
    virtual WrapType<BSONInfo>& bsonProto() = 0;
    virtual WrapType<DBRefInfo>& dbRefProto() = 0;
    virtual WrapType<RegExpInfo>& regExpProto() = 0;

    virtual void setStatus(Status status) = 0;

    virtual bool isJavaScriptProtectionEnabled() const = 0;

    virtual bool requiresOwnedObjects() const = 0;

    virtual void newFunction(std::string_view code, JS::MutableHandleValue out) = 0;

    // Pointer tracking for ASAN
    virtual void trackNewPointer(void* ptr) = 0;
    virtual void trackDeletePointer(void* ptr) = 0;
};

template <typename T>
WrapType<T>& getProto(MozJSCommonRuntimeInterface* runtime);

template <>
inline WrapType<NumberLongInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->numberLongProto();
}

template <>
inline WrapType<NumberIntInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->numberIntProto();
}

template <>
inline WrapType<NumberDecimalInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->numberDecimalProto();
}

template <>
inline WrapType<OIDInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->oidProto();
}

template <>
inline WrapType<BinDataInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->binDataProto();
}

template <>
inline WrapType<TimestampInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->timestampProto();
}

template <>
inline WrapType<MaxKeyInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->maxKeyProto();
}

template <>
inline WrapType<MinKeyInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->minKeyProto();
}

template <>
inline WrapType<CodeInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->codeProto();
}

template <>
inline WrapType<DBPointerInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->dbPointerProto();
}

template <>
inline WrapType<NativeFunctionInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->nativeFunctionProto();
}

template <>
inline WrapType<ErrorInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->errorProto();
}

template <>
inline WrapType<MongoStatusInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->mongoStatusProto();
}

template <>
inline WrapType<BSONInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->bsonProto();
}

template <>
inline WrapType<DBRefInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->dbRefProto();
}

template <>
inline WrapType<RegExpInfo>& getProto(MozJSCommonRuntimeInterface* runtime) {
    return runtime->regExpProto();
}

//
// Tracked memory allocation helpers
// These ensure proper ASAN tracking of dynamically allocated objects.
//

template <typename T, typename... Args>
T* trackedNew(MozJSCommonRuntimeInterface* runtime, Args&&... args) {
    T* t = new T(std::forward<Args>(args)...);
    runtime->trackNewPointer(t);
    return t;
}

template <typename T>
void trackedDelete(MozJSCommonRuntimeInterface* runtime, T* t) {
    runtime->trackDeletePointer(t);
    delete t;
}

[[MONGO_MOD_PUBLIC]] inline MozJSCommonRuntimeInterface* getCommonRuntime(JSContext* cx) {
    return static_cast<MozJSCommonRuntimeInterface*>(JS_GetContextPrivate(cx));
}

}  // namespace mongo::mozjs

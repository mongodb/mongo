/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <js/Id.h>

struct JSContext;

namespace mongo::mozjs {

template <typename T>
class WrapType;

enum class InternedString;

struct BinDataInfo;
struct BSONInfo;
struct CodeInfo;
struct CursorHandleInfo;
struct CursorInfo;
struct DBPointerInfo;
struct DBRefInfo;
struct ErrorInfo;
struct MaxKeyInfo;
struct MinKeyInfo;
struct MongoExternalInfo;
struct MongoStatusInfo;
struct NativeFunctionInfo;
struct NumberDecimalInfo;
struct NumberIntInfo;
struct NumberLongInfo;
struct OIDInfo;
struct RegExpInfo;
struct SessionInfo;
struct TimestampInfo;
struct URIInfo;

/**
 * MozJS-specific scope base class.
 *
 * This class extends the engine-agnostic Scope class with MozJS-specific
 * functionality needed by code in `scripting/mozjs/common/`.
 *
 * Intended inheritance hierarchy:
 *   Scope (scripting/engine.h)
 *     └── MozJSScopeBase
 *           └── MozJSImplScope (shell/implscope.h)
 *           └── MozJSWasmScope (wasm/implscope.h)
 */
class MONGO_MOD_PUB MozJSScopeBase : public Scope {
public:
    ~MozJSScopeBase() override = default;

    virtual JS::HandleId getInternedStringId(InternedString name) = 0;

    // Helpers used by `common/valuereader.cpp`.
    virtual bool isJavaScriptProtectionEnabled() const = 0;
    virtual void newFunction(StringData code, JS::MutableHandleValue out) = 0;
    virtual std::int64_t* trackedNewInt64(std::int64_t value) = 0;

    // Scope generation tracking (used by `common/types/bson.cpp`).
    virtual std::size_t getGeneration() const = 0;

    // Whether this scope requires all bound BSON objects to be owned.
    virtual bool requiresOwnedObjects() const = 0;

    // Store a "current status" on the scope (used by `common/exception.cpp`).
    virtual void setStatus(Status status) = 0;

    // Sleep for the given duration. Used by the JS `sleep()` function.
    virtual void sleep(Milliseconds ms) = 0;

    // Pointer tracking hooks for ASANHandles. Implementations may no-op these when not needed.
    virtual void trackNewPointer(void* ptr) = 0;
    virtual void trackDeletePointer(void* ptr) = 0;

    // --- Prototype accessors ---
    virtual WrapType<BinDataInfo>& binDataProto() = 0;
    virtual WrapType<BSONInfo>& bsonProto() = 0;
    virtual WrapType<CodeInfo>& codeProto() = 0;
    virtual WrapType<CursorHandleInfo>& cursorHandleProto() = 0;
    virtual WrapType<CursorInfo>& cursorProto() = 0;
    virtual WrapType<DBPointerInfo>& dbPointerProto() = 0;
    virtual WrapType<DBRefInfo>& dbRefProto() = 0;
    virtual WrapType<ErrorInfo>& errorProto() = 0;
    virtual WrapType<MaxKeyInfo>& maxKeyProto() = 0;
    virtual WrapType<MinKeyInfo>& minKeyProto() = 0;
    virtual WrapType<MongoExternalInfo>& mongoExternalProto() = 0;
    virtual WrapType<MongoStatusInfo>& mongoStatusProto() = 0;
    virtual WrapType<NativeFunctionInfo>& nativeFunctionProto() = 0;
    virtual WrapType<NumberDecimalInfo>& numberDecimalProto() = 0;
    virtual WrapType<NumberIntInfo>& numberIntProto() = 0;
    virtual WrapType<NumberLongInfo>& numberLongProto() = 0;
    virtual WrapType<OIDInfo>& oidProto() = 0;
    virtual WrapType<RegExpInfo>& regExpProto() = 0;
    virtual WrapType<SessionInfo>& sessionProto() = 0;
    virtual WrapType<TimestampInfo>& timestampProto() = 0;
    virtual WrapType<URIInfo>& uriProto() = 0;
};

MONGO_MOD_PUB MozJSScopeBase* getMozJSScope(JSContext* cx);

template <typename T>
WrapType<T>& getProto(MozJSScopeBase* scope);

template <>
inline WrapType<BinDataInfo>& getProto(MozJSScopeBase* scope) {
    return scope->binDataProto();
}

template <>
inline WrapType<BSONInfo>& getProto(MozJSScopeBase* scope) {
    return scope->bsonProto();
}

template <>
inline WrapType<CodeInfo>& getProto(MozJSScopeBase* scope) {
    return scope->codeProto();
}

template <>
inline WrapType<CursorHandleInfo>& getProto(MozJSScopeBase* scope) {
    return scope->cursorHandleProto();
}

template <>
inline WrapType<CursorInfo>& getProto(MozJSScopeBase* scope) {
    return scope->cursorProto();
}

template <>
inline WrapType<DBPointerInfo>& getProto(MozJSScopeBase* scope) {
    return scope->dbPointerProto();
}

template <>
inline WrapType<DBRefInfo>& getProto(MozJSScopeBase* scope) {
    return scope->dbRefProto();
}

template <>
inline WrapType<ErrorInfo>& getProto(MozJSScopeBase* scope) {
    return scope->errorProto();
}

template <>
inline WrapType<MaxKeyInfo>& getProto(MozJSScopeBase* scope) {
    return scope->maxKeyProto();
}

template <>
inline WrapType<MinKeyInfo>& getProto(MozJSScopeBase* scope) {
    return scope->minKeyProto();
}

template <>
inline WrapType<MongoExternalInfo>& getProto(MozJSScopeBase* scope) {
    return scope->mongoExternalProto();
}

template <>
inline WrapType<MongoStatusInfo>& getProto(MozJSScopeBase* scope) {
    return scope->mongoStatusProto();
}

template <>
inline WrapType<NativeFunctionInfo>& getProto(MozJSScopeBase* scope) {
    return scope->nativeFunctionProto();
}

template <>
inline WrapType<NumberDecimalInfo>& getProto(MozJSScopeBase* scope) {
    return scope->numberDecimalProto();
}

template <>
inline WrapType<NumberIntInfo>& getProto(MozJSScopeBase* scope) {
    return scope->numberIntProto();
}

template <>
inline WrapType<NumberLongInfo>& getProto(MozJSScopeBase* scope) {
    return scope->numberLongProto();
}

template <>
inline WrapType<OIDInfo>& getProto(MozJSScopeBase* scope) {
    return scope->oidProto();
}

template <>
inline WrapType<RegExpInfo>& getProto(MozJSScopeBase* scope) {
    return scope->regExpProto();
}

template <>
inline WrapType<SessionInfo>& getProto(MozJSScopeBase* scope) {
    return scope->sessionProto();
}

template <>
inline WrapType<TimestampInfo>& getProto(MozJSScopeBase* scope) {
    return scope->timestampProto();
}

template <>
inline WrapType<URIInfo>& getProto(MozJSScopeBase* scope) {
    return scope->uriProto();
}

template <typename T, typename... Args>
T* trackedNew(MozJSScopeBase* scope, Args&&... args) {
    T* t = new T(std::forward<Args>(args)...);
    scope->trackNewPointer(t);
    return t;
}

template <typename T>
void trackedDelete(MozJSScopeBase* scope, T* t) {
    scope->trackDeletePointer(t);
    delete (t);
}

}  // namespace mongo::mozjs

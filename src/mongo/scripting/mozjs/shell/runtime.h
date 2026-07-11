// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/util/modules.h"

struct JSContext;

namespace mongo::mozjs {

struct CursorHandleInfo;
struct CursorInfo;
struct MongoExternalInfo;
struct SessionInfo;
struct URIInfo;

/**
 * Shell-specific prototype accessors for JS types that only exist in the
 * full mongo shell (cursors, connections, sessions, URIs).
 */
class [[MONGO_MOD_PUBLIC]] MozJSShellRuntimeInterface {
public:
    virtual ~MozJSShellRuntimeInterface() = default;

    virtual WrapType<CursorHandleInfo>& cursorHandleProto() = 0;
    virtual WrapType<CursorInfo>& cursorProto() = 0;
    virtual WrapType<MongoExternalInfo>& mongoExternalProto() = 0;
    virtual WrapType<SessionInfo>& sessionProto() = 0;
    virtual WrapType<URIInfo>& uriProto() = 0;
};

/**
 * Get the MozJSShellRuntimeInterface from a JSContext.
 * Use this only when shell-specific features are needed.
 * For common code, use getCommonRuntime() from scope.h instead.
 */
MozJSShellRuntimeInterface* getShellRuntime(JSContext* cx);

}  // namespace mongo::mozjs

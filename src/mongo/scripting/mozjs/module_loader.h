/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/filesystem.hpp>
#include <boost/optional.hpp>

#include <jsapi.h>

#include "mongo/base/string_data.h"

namespace mongo {
namespace mozjs {

class ModuleLoader {
public:
    bool init(JSContext* ctx, const std::string& loadPath);
    JSObject* loadRootModuleFromPath(JSContext* cx, const std::string& path);
    JSObject* loadRootModuleFromSource(JSContext* cx, const std::string& path, StringData source);

private:
    static std::string resolveBaseUrl(JSContext* cx, const std::string& loadPath);
    static JSString* fileAsString(JSContext* cx, JS::HandleString pathnameStr);
    static JSObject* moduleResolveHook(JSContext* cx,
                                       JS::HandleValue referencingPrivate,
                                       JS::HandleObject moduleRequest);
    static bool dynamicModuleImportHook(JSContext* cx,
                                        JS::HandleValue referencingPrivate,
                                        JS::HandleObject moduleRequest,
                                        JS::HandleObject promise);

    JSObject* loadRootModule(JSContext* cx,
                             const std::string& path,
                             boost::optional<StringData> source);
    JSObject* resolveImportedModule(JSContext* cx,
                                    JS::HandleValue referencingPrivate,
                                    JS::HandleObject moduleRequest);
    bool importModuleDynamically(JSContext* cx,
                                 JS::HandleValue referencingPrivate,
                                 JS::HandleObject moduleRequest,
                                 JS::HandleObject promise);
    JSObject* loadAndParse(JSContext* cx,
                           JS::HandleString path,
                           JS::HandleValue referencingPrivate);
    bool lookUpModuleInRegistry(JSContext* cx,
                                JS::HandleString path,
                                JS::MutableHandleObject moduleOut);
    bool addModuleToRegistry(JSContext* cx, JS::HandleString path, JS::HandleObject module);
    JSString* resolveAndNormalize(JSContext* cx,
                                  JS::HandleObject moduleRequestArg,
                                  JS::HandleValue referencingInfo);
    JSObject* getOrCreateModuleRegistry(JSContext* cx);
    JSString* fetchSource(JSContext* cx, JS::HandleString path, JS::HandleValue referencingPrivate);
    bool getScriptPath(JSContext* cx,
                       JS::HandleValue privateValue,
                       JS::MutableHandleString pathOut);

    JSObject* createScriptPrivateInfo(JSContext* cx,
                                      JS::Handle<JSString*> path,
                                      JS::Handle<JSString*> source);

    std::string _baseUrl;
};

}  // namespace mozjs
}  // namespace mongo

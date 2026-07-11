// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <jsapi.h>

#include <boost/filesystem.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

class ModuleLoader {
public:
    bool init(JSContext* ctx, const std::string& loadPath);
    JSObject* loadRootModuleFromPath(JSContext* cx, const std::string& path);
    JSObject* loadRootModuleFromSource(JSContext* cx,
                                       const std::string& path,
                                       std::string_view source);
    std::string getBaseURL() const {
        return _baseUrl;
    };

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
                             boost::optional<std::string_view> source);
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
    bool preloadInternalModules(JSContext* cx);
    bool getScriptPath(JSContext* cx,
                       JS::HandleValue privateValue,
                       JS::MutableHandleString pathOut);

    JSObject* createScriptPrivateInfo(JSContext* cx,
                                      JS::Handle<JSString*> path,
                                      boost::optional<std::string_view> source = boost::none);

    std::string _baseUrl;
    std::vector<std::string> _searchPaths;
};

}  // namespace mozjs
}  // namespace mongo

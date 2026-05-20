/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <js/JSON.h>
#include <js/Modules.h>
#include <js/SourceText.h>
#include <js/StableStringChars.h>
// IWYU pragma: no_include "boost/system/detail/errc.hpp"
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"
#include "mongo/logv2/log.h"
#include "mongo/scripting/mongo_path_util.h"
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/scripting/mozjs/shell/internal_module_registry.h"
#include "mongo/scripting/mozjs/shell/module_loader.h"
#include "mongo/util/file.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <jsapi.h>
#include <jscustomallocator.h>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <js/CallArgs.h>
#include <js/CharacterEncoding.h>
#include <js/CompileOptions.h>
#include <js/Context.h>
#include <js/ErrorReport.h>
#include <js/GlobalObject.h>
#include <js/MapAndSet.h>
#include <js/Object.h>
#include <js/PropertyAndElement.h>
#include <js/PropertyDescriptor.h>
#include <js/RootingAPI.h>
#include <js/ScriptPrivate.h>
#include <js/String.h>
#include <js/TypeDecls.h>
#include <js/Utility.h>
#include <js/Value.h>
#include <mozilla/Range.h>
#include <mozilla/RangedPtr.h>
#include <mozilla/UniquePtr.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace mozjs {
namespace {
constexpr const char* kStdModulePrefix = "std:";

enum GlobalAppSlot {
    GlobalAppSlotModuleRegistry,
    GlobalAppSlotInternalBindingsRegistry,
    GlobalAppSlotCount
};

bool startsWithPrefix(const char* value, const char* prefix) {
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool getOrCreateGlobalMapInSlot(JSContext* cx, GlobalAppSlot slot, JS::MutableHandleObject mapOut) {
    mapOut.set(nullptr);
    JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));
    if (!global) {
        return false;
    }

    JS::RootedValue value(cx, JS::GetReservedSlot(global, slot));
    if (!value.isUndefined()) {
        mapOut.set(&value.toObject());
        return true;
    }

    JS::RootedObject map(cx, JS::NewMapObject(cx));
    if (!map) {
        return false;
    }

    JS::SetReservedSlot(global, slot, JS::ObjectValue(*map));
    mapOut.set(map);
    return true;
}

bool getOrCreateInternalModuleBindingsRegistry(JSContext* cx,
                                               JS::MutableHandleObject bindingsRegistryOut) {
    return getOrCreateGlobalMapInSlot(
        cx, GlobalAppSlotInternalBindingsRegistry, bindingsRegistryOut);
}

bool registerInternalModuleBinding(JSContext* cx,
                                   const char* moduleName,
                                   JS::HandleObject bindingObject) {
    JS::RootedObject bindingsRegistry(cx);
    if (!getOrCreateInternalModuleBindingsRegistry(cx, &bindingsRegistry)) {
        return false;
    }

    JS::RootedString moduleNameString(cx, JS_NewStringCopyZ(cx, moduleName));
    if (!moduleNameString) {
        return false;
    }

    JS::RootedValue moduleNameValue(cx, JS::StringValue(moduleNameString));
    JS::RootedValue bindingValue(cx, JS::ObjectValue(*bindingObject));
    return JS::MapSet(cx, bindingsRegistry, moduleNameValue, bindingValue);
}

bool lookUpInternalModuleBinding(JSContext* cx,
                                 JS::HandleString moduleName,
                                 JS::MutableHandleObject bindingOut) {
    bindingOut.set(nullptr);

    JS::RootedObject bindingsRegistry(cx);
    if (!getOrCreateInternalModuleBindingsRegistry(cx, &bindingsRegistry)) {
        return false;
    }

    JS::RootedValue moduleNameValue(cx, JS::StringValue(moduleName));
    JS::RootedValue bindingValue(cx);
    if (!JS::MapGet(cx, bindingsRegistry, moduleNameValue, &bindingValue)) {
        return false;
    }

    if (!bindingValue.isUndefined()) {
        bindingOut.set(&bindingValue.toObject());
    }

    return true;
}

bool internalModuleFunction(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (args.length() < 1 || !args[0].isString()) {
        JS_ReportErrorASCII(cx, "internalModule requires a string module name");
        return false;
    }

    JS::RootedValue callerPrivate(cx, JS::GetScriptedCallerPrivate(cx));
    if (!callerPrivate.isObject()) {
        JS_ReportErrorASCII(cx, "internalModule is restricted to std:* modules");
        return false;
    }

    JS::RootedObject callerInfo(cx, &callerPrivate.toObject());
    JS::RootedValue callerPathValue(cx);
    if (!JS_GetProperty(cx, callerInfo, "path", &callerPathValue)) {
        return false;
    }
    if (!callerPathValue.isString()) {
        JS_ReportErrorASCII(cx, "internalModule is restricted to std:* modules");
        return false;
    }

    JS::RootedString callerPath(cx, callerPathValue.toString());
    JS::UniqueChars callerPathChars = JS_EncodeStringToUTF8(cx, callerPath);
    if (!callerPathChars) {
        return false;
    }
    if (!startsWithPrefix(callerPathChars.get(), kStdModulePrefix)) {
        JS_ReportErrorUTF8(cx,
                           "internalModule is restricted to std:* modules (called from %s)",
                           callerPathChars.get());
        return false;
    }

    JS::RootedString moduleName(cx, args[0].toString());
    JS::RootedObject binding(cx);
    if (!lookUpInternalModuleBinding(cx, moduleName, &binding)) {
        return false;
    }
    if (!binding) {
        JS::UniqueChars moduleNameChars = JS_EncodeStringToUTF8(cx, moduleName);
        if (!moduleNameChars) {
            return false;
        }
        JS_ReportErrorUTF8(cx, "No such internal module '%s'", moduleNameChars.get());
        return false;
    }

    args.rval().setObject(*binding);
    return true;
}
}  // namespace

bool ModuleLoader::init(JSContext* cx, const std::string& loadPath) {
    _baseUrl = resolveBaseUrl(cx, loadPath);
    LOGV2_DEBUG(716281, 2, "Resolved module base url.", "baseUrl"_attr = _baseUrl.c_str());

    // If MONGO_PATH is not set, use the resolved baseUrl as the default.
    _searchPaths = parseMongoPath(_baseUrl);

    LOGV2_DEBUG(99745619,
                2,
                "Initialized module search paths.",
                "numPaths"_attr = _searchPaths.size(),
                "baseUrl"_attr = _baseUrl.c_str());

    JSRuntime* rt = JS_GetRuntime(cx);
    JS::SetModuleResolveHook(rt, ModuleLoader::moduleResolveHook);
    JS::SetModuleDynamicImportHook(rt, ModuleLoader::dynamicModuleImportHook);

    JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));
    if (!global) {
        return false;
    }
    if (!JS_DefineFunction(
            cx, global, "internalModule", internalModuleFunction, 1, JSPROP_PERMANENT)) {
        return false;
    }

    return preloadInternalModules(cx);
}

JSObject* ModuleLoader::loadRootModuleFromPath(JSContext* cx, const std::string& path) {
    return loadRootModule(cx, path, boost::none);
}

JSObject* ModuleLoader::loadRootModuleFromSource(JSContext* cx,
                                                 const std::string& path,
                                                 StringData source) {
    return loadRootModule(cx, path, source);
}

JSObject* ModuleLoader::loadRootModule(JSContext* cx,
                                       const std::string& path,
                                       boost::optional<StringData> source) {
    JS::RootedString baseUrl(cx, JS_NewStringCopyN(cx, _baseUrl.c_str(), _baseUrl.size()));
    if (!baseUrl) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create baseUrl");
    }
    JS::RootedObject info(cx, createScriptPrivateInfo(cx, baseUrl, source));
    if (!info) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create info");
    }

    JS::RootedValue referencingPrivate(cx, JS::ObjectValue(*info));
    JS::RootedString specifier(cx, JS_NewStringCopyN(cx, path.c_str(), path.size()));
    if (!specifier) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create specifier");
    }
    JS::RootedObject moduleRequest(
        cx, JS::CreateModuleRequest(cx, specifier, JS::ModuleType::JavaScript));
    if (!moduleRequest) {
        return nullptr;
    }

    return resolveImportedModule(cx, referencingPrivate, moduleRequest);
}

bool ModuleLoader::preloadInternalModules(JSContext* cx) {
    for (const auto& registration : listRegisteredInternalModules()) {
        JS::RootedObject binding(cx, JS_NewPlainObject(cx));
        if (!binding) {
            return false;
        }
        if (!registration.initialize(cx, binding)) {
            return false;
        }
        if (!registerInternalModuleBinding(cx, registration.moduleName.c_str(), binding)) {
            return false;
        }

        if (registration.setupFile) {
            JS::RootedObject setupModule(cx,
                                         loadRootModuleFromSource(cx,
                                                                  registration.setupFile->name,
                                                                  registration.setupFile->source));
            if (!setupModule) {
                return false;
            }
        }
    }

    return true;
}

// static
JSObject* ModuleLoader::moduleResolveHook(JSContext* cx,
                                          JS::HandleValue referencingPrivate,
                                          JS::HandleObject moduleRequest) {

    auto scope = getScope(cx);
    return scope->getModuleLoader()->resolveImportedModule(cx, referencingPrivate, moduleRequest);
}

JSObject* ModuleLoader::resolveImportedModule(JSContext* cx,
                                              JS::HandleValue referencingPrivate,
                                              JS::HandleObject moduleRequest) {
    JS::Rooted<JSString*> path(cx, resolveAndNormalize(cx, moduleRequest, referencingPrivate));
    if (!path) {
        return nullptr;
    }

    return loadAndParse(cx, path, referencingPrivate);
}

// static
bool ModuleLoader::dynamicModuleImportHook(JSContext* cx,
                                           JS::HandleValue referencingPrivate,
                                           JS::HandleObject moduleRequest,
                                           JS::HandleObject promise) {
    auto scope = getScope(cx);
    return scope->getModuleLoader()->importModuleDynamically(
        cx, referencingPrivate, moduleRequest, promise);
}

bool ModuleLoader::importModuleDynamically(JSContext* cx,
                                           JS::HandleValue referencingPrivate,
                                           JS::HandleObject moduleRequest,
                                           JS::HandleObject promise) {
    JS::RootedString baseUrl(cx, JS_NewStringCopyN(cx, _baseUrl.c_str(), _baseUrl.size()));
    if (!baseUrl) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create baseUrl");
    }
    JS::RootedObject info(cx, createScriptPrivateInfo(cx, baseUrl));
    if (!info) {
        uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create info");
    }
    JS::RootedValue newReferencingPrivate(cx, JS::ObjectValue(*info));

    // The dynamic `import` method returns a Promise, and thus allows us to perform module loading
    // dynamically in the engine. The test runner is single threaded, so there is no benefit to us
    // loading asynchronously. We will continue to return a Promise (per the contract), but perform
    // loading synchronously.
    JS::RootedValue rval(cx);
    bool ok = [&]() {
        JS::Rooted<JSString*> path(cx,
                                   resolveAndNormalize(cx, moduleRequest, newReferencingPrivate));
        if (!path) {
            LOGV2_ERROR(716284, "Failed to resolve and normalize path");
            return false;
        }

        JS::RootedObject module(cx, loadAndParse(cx, path, newReferencingPrivate));
        if (!module) {
            LOGV2_ERROR(716285, "Failed to load and parse module");
            return false;
        }

        if (!JS::ModuleLink(cx, module)) {
            LOGV2_ERROR(716286, "Failed to link module");
            return false;
        }

        return JS::ModuleEvaluate(cx, module, &rval);
    }();

    JSObject* evaluationObject = ok ? &rval.toObject() : nullptr;
    JS::RootedObject evaluationPromise(cx, evaluationObject);
    if (!evaluationPromise) {
        LOGV2_ERROR(716287, "Failed to create evaluation promise");
        return false;
    }
    return JS::FinishDynamicModuleImport(
        cx, evaluationPromise, newReferencingPrivate, moduleRequest, promise);
}

/**
 * A few things to note about module resolution:
 *   - A "specifier" refers to the name of the imported module (e.g. `import {x} from 'specifier'`)
 *   - Specifiers with relative paths are always relative to their referencing module. The
 *     referencing module for the root module is the file `mongo` was run on, or the mongo binary
 *     itself when run as a REPL. In practice this means relative paths in scripts behave as you
 *     would expect relative paths to work on the command line.
 *   - If we already have source for a specifier we are trying to load (this is only the case when
 *     executing the root module), we will skip normalization and reading the source again.
 */
JSString* ModuleLoader::resolveAndNormalize(JSContext* cx,
                                            JS::HandleObject moduleRequestArg,
                                            JS::HandleValue referencingInfo) {
    JS::Rooted<JSString*> specifierString(cx, JS::GetModuleRequestSpecifier(cx, moduleRequestArg));
    if (!specifierString) {
        return nullptr;
    }

    if (referencingInfo.isUndefined()) {
        JS_ReportErrorASCII(cx, "No referencing module for relative import");
        return nullptr;
    }

    // Root modules loaded from in-memory source (via execSetup) carry a source payload in the
    // referencing info. For those loads, keep the existing behavior and bypass file-system lookup.
    bool hasSource{false};
    JS::RootedObject referencingInfoObject(cx, &referencingInfo.toObject());
    if (!JS_HasProperty(cx, referencingInfoObject, "source", &hasSource)) {
        return nullptr;
    }
    if (hasSource) {
        return specifierString;
    }

    // Check if this specifier is already in the in-memory module registry.
    JS::Rooted<JSString*> path(cx, specifierString);
    if (!path) {
        return nullptr;
    }
    JS::RootedObject module(cx);
    if (!lookUpModuleInRegistry(cx, path, &module)) {
        return nullptr;
    }
    if (module) {
        return specifierString;
    }

    JS::UniqueChars specifierChars = JS_EncodeStringToUTF8(cx, specifierString);
    uassert(ErrorCodes::JSInterpreterFailure,
            "Failed to UTF-8 encode module specifier",
            specifierChars);

    // STD modules are identified by module specifier and don't map to filesystem paths.
    if (startsWithPrefix(specifierChars.get(), kStdModulePrefix)) {
        return specifierString;
    }

    JS::RootedString refPath(cx);
    if (!getScriptPath(cx, referencingInfo, &refPath)) {
        return nullptr;
    }
    if (!refPath) {
        JS_ReportErrorASCII(cx, "No path set for referencing module");
        return nullptr;
    }

    JS::UniqueChars refPathChars = JS_EncodeStringToUTF8(cx, refPath);
    uassert(ErrorCodes::JSInterpreterFailure,
            "Failed to UTF-8 encode referencing module path",
            refPathChars);

    // otherwise try to read content from the file system
    boost::filesystem::path specifierPath(specifierChars.get());
    boost::filesystem::path refAbsPath(refPathChars.get());

    if (is_directory(specifierPath)) {
        JS_ReportErrorUTF8(cx,
                           "Directory import '%s' is not supported, imported from %s",
                           specifierPath.string().c_str(),
                           refAbsPath.string().c_str());
        return nullptr;
    }

    if (!specifierPath.is_relative()) {
        return specifierString;
    }

    // Search through all configured search paths (MONGO_PATH + base URL)
    boost::system::error_code ec;
    std::string lastErrorMsg;

    for (const auto& searchPath : _searchPaths) {
        auto fullPath =
            boost::filesystem::canonical(specifierPath, searchPath, ec).lexically_normal().string();

        if (!ec && boost::filesystem::exists(fullPath)) {
            // Found the module in this search path
            LOGV2_DEBUG(99745618,
                        3,
                        "Resolved module import.",
                        "specifier"_attr = specifierPath.string(),
                        "searchPath"_attr = searchPath,
                        "fullPath"_attr = fullPath);
            return JS_NewStringCopyN(cx, fullPath.c_str(), fullPath.size());
        }

        // Track the last error for reporting
        if (ec && ec.value() != boost::system::errc::no_such_file_or_directory) {
            lastErrorMsg = ec.message();
        }
    }

    // Module not found in any search path
    if (!lastErrorMsg.empty()) {
        // Report a non-ENOENT error if we encountered one
        JS_ReportErrorUTF8(cx, "%s", lastErrorMsg.c_str());
    } else {
        // Report not found error with search path info
        JS_ReportErrorUTF8(cx,
                           "Cannot find module '%s' imported from %s (searched %zu path%s)",
                           specifierPath.string().c_str(),
                           refAbsPath.string().c_str(),
                           _searchPaths.size(),
                           _searchPaths.size() == 1 ? "" : "s");
    }

    return nullptr;
}

bool ModuleLoader::getScriptPath(JSContext* cx,
                                 JS::HandleValue privateValue,
                                 JS::MutableHandleString pathOut) {
    pathOut.set(nullptr);

    JS::RootedObject infoObj(cx, &privateValue.toObject());
    JS::RootedValue pathValue(cx);
    if (!JS_GetProperty(cx, infoObj, "path", &pathValue)) {
        return false;
    }

    if (pathValue.isUndefined()) {
        return true;
    }

    JS::RootedString path(cx, pathValue.toString());
    pathOut.set(path);
    return pathOut;
}

JSObject* ModuleLoader::loadAndParse(JSContext* cx,
                                     JS::HandleString pathArg,
                                     JS::HandleValue referencingPrivate) {
    JS::Rooted<JSString*> path(cx, pathArg);
    if (!path) {
        return nullptr;
    }

    JS::RootedObject module(cx);
    if (!lookUpModuleInRegistry(cx, path, &module)) {
        return nullptr;
    }

    if (module) {
        return module;
    }

    JS::RootedString source(cx, fetchSource(cx, path, referencingPrivate));
    if (!source) {
        return nullptr;
    }

    JS::UniqueChars filename = JS_EncodeStringToLatin1(cx, path);
    if (!filename) {
        return nullptr;
    }

    JS::RootedObject info(cx, createScriptPrivateInfo(cx, path));
    if (!info) {
        return nullptr;
    }

    JS::AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, source)) {
        return nullptr;
    }

    const char16_t* chars = stableChars.twoByteRange().begin().get();
    JS::SourceText<char16_t> srcBuf;
    if (!srcBuf.init(cx, chars, JS::GetStringLength(source), JS::SourceOwnership::Borrowed)) {
        return nullptr;
    }

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.get(), 1);
    module = JS::CompileModule(cx, options, srcBuf);
    if (!module) {
        return nullptr;
    }

    JS::SetModulePrivate(module, JS::ObjectValue(*info));

    if (!addModuleToRegistry(cx, path, module)) {
        return nullptr;
    }

    return module;
}

JSString* ModuleLoader::fetchSource(JSContext* cx,
                                    JS::HandleString pathArg,
                                    JS::HandleValue referencingPrivate) {
    JS::RootedObject infoObj(cx, &referencingPrivate.toObject());
    JS::RootedValue sourceValue(cx);
    if (!JS_GetProperty(cx, infoObj, "source", &sourceValue)) {
        return nullptr;
    }

    if (!sourceValue.isUndefined()) {
        return sourceValue.toString();
    }

    JS::RootedString resolvedPath(cx, pathArg);
    if (!resolvedPath) {
        return nullptr;
    }

    return fileAsString(cx, resolvedPath);
}

JSObject* ModuleLoader::getOrCreateModuleRegistry(JSContext* cx) {
    JS::RootedObject registry(cx);
    if (!getOrCreateGlobalMapInSlot(cx, GlobalAppSlotModuleRegistry, &registry)) {
        return nullptr;
    }

    return registry;
}

bool ModuleLoader::lookUpModuleInRegistry(JSContext* cx,
                                          JS::HandleString path,
                                          JS::MutableHandleObject moduleOut) {
    moduleOut.set(nullptr);

    JS::RootedObject registry(cx, getOrCreateModuleRegistry(cx));
    if (!registry) {
        return false;
    }

    JS::RootedValue pathValue(cx, StringValue(path));
    JS::RootedValue moduleValue(cx);
    if (!JS::MapGet(cx, registry, pathValue, &moduleValue)) {
        return false;
    }

    if (!moduleValue.isUndefined()) {
        moduleOut.set(&moduleValue.toObject());
    }

    return true;
}

bool ModuleLoader::addModuleToRegistry(JSContext* cx,
                                       JS::HandleString path,
                                       JS::HandleObject module) {
    JS::RootedObject registry(cx, getOrCreateModuleRegistry(cx));
    if (!registry) {
        return false;
    }

    JS::RootedValue pathValue(cx, StringValue(path));
    JS::RootedValue moduleValue(cx, JS::ObjectValue(*module));
    return JS::MapSet(cx, registry, pathValue, moduleValue);
}

// 2 GB is the largest support Javascript file size.
const fileofs kMaxJsFileLength = fileofs(2) * 1024 * 1024 * 1024;
JSString* ModuleLoader::fileAsString(JSContext* cx, JS::HandleString pathnameStr) {
    JS::UniqueChars pathname = JS_EncodeStringToLatin1(cx, pathnameStr);
    if (!pathname) {
        return nullptr;
    }

    File file;
    file.open(pathname.get(), true);
    if (!file.is_open() || file.bad()) {
        JS_ReportErrorUTF8(cx, "can't open for reading %s", pathname.get());
        return nullptr;
    }

    fileofs fo = file.len();
    if (fo > kMaxJsFileLength) {
        JS_ReportErrorUTF8(cx, "file contents too large reading %s", pathname.get());
        return nullptr;
    }

    size_t len = static_cast<size_t>(fo);
    JS::UniqueChars buf(js_pod_malloc<char>(len));
    if (!buf) {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    file.read(0, buf.get(), len);
    if (file.bad()) {
        JS_ReportErrorUTF8(cx, "failed to read file %s", pathname.get());
        return nullptr;
    }

    int offset = 0;
    if (len > 2 && buf[0] == '#' && buf[1] == '!') {
        const char* newline = reinterpret_cast<const char*>(memchr(buf.get(), '\n', len));
        if (newline) {
            offset = newline - buf.get();
        } else {
            // file of just shebang treated same as empty file
            offset = len;
        }
    }

    JS::UniqueTwoByteChars ucbuf(
        JS::LossyUTF8CharsToNewTwoByteCharsZ(
            cx, JS::UTF8Chars(buf.get() + offset, len), &len, js::MallocArena)
            .get());
    if (!ucbuf) {
        pathname = JS_EncodeStringToUTF8(cx, pathnameStr);
        if (!pathname) {
            return nullptr;
        }

        JS_ReportErrorUTF8(cx, "invalid UTF-8 in file '%s'", pathname.get());
        return nullptr;
    }

    return JS_NewUCStringCopyN(cx, ucbuf.get(), len);
}

JSObject* ModuleLoader::createScriptPrivateInfo(JSContext* cx,
                                                JS::Handle<JSString*> path,
                                                boost::optional<StringData> source) {
    JS::Rooted<JSObject*> info(cx, JS_NewPlainObject(cx));
    if (!info) {
        return nullptr;
    }

    if (path) {
        JS::Rooted<JS::Value> pathValue(cx, JS::StringValue(path));
        if (!JS_DefineProperty(cx, info, "path", pathValue, JSPROP_ENUMERATE)) {
            return nullptr;
        }
    }

    if (source) {
        size_t len = source->size();
        JS::UniqueTwoByteChars ucbuf(
            JS::LossyUTF8CharsToNewTwoByteCharsZ(
                cx, JS::UTF8Chars(source->data(), len), &len, js::MallocArena)
                .get());
        if (!ucbuf) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create ucbuf");
        }

        JS::RootedString sourceValue(cx, JS_NewUCStringCopyN(cx, ucbuf.get(), len));

        if (!sourceValue) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create sourceValue");
        }


        if (!JS_DefineProperty(cx, info, "source", sourceValue, JSPROP_ENUMERATE)) {
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to create source property");
        }
    }

    return info;
}

/*
The rules for baseUrl resolution are as follows:
    1. At process start determine a "loadPath" which is either the parent directory of the first
      file passed in to execute, or the current working directory.
    2. Search the loadPath for a file called "jsconfig.json", and attempt to read it as a JSON file.
    3. If found, try to find a property "compilerOptions.baseUrl" in that file and resolve that URL
      relative to the location of "jsconfig.json".
    4. If not found, loadPath is now the parent directory of loadPath, repeat steps #2-3 until
       either the baseUrl is resolved or we reach the root directory.
    5. If no baseUrl is resolved, return the current working directory
*/
static const char* const kJSConfigJsonFileName = "jsconfig.json";
static const char* const kCompileOptionsPropertyName = "compilerOptions";
static const char* const kBaseUrlPropertyName = "baseUrl";
std::string ModuleLoader::resolveBaseUrl(JSContext* cx, const std::string& loadPath) {
    auto path = boost::filesystem::path(loadPath);
    while (true) {
        const boost::filesystem::directory_iterator end;
        const auto it = std::find_if(boost::filesystem::directory_iterator(path),
                                     end,
                                     [&](const boost::filesystem::directory_entry& e) {
                                         return e.path().filename() == kJSConfigJsonFileName;
                                     });
        if (it != end) {
            auto jsConfigPath = it->path().string();
            JS::RootedString jsConfigPathString(cx, JS_NewStringCopyZ(cx, jsConfigPath.c_str()));
            JS::RootedString jsConfigSource(cx, fileAsString(cx, jsConfigPathString));
            if (!jsConfigSource) {
                break;
            }

            JS::RootedValue jsConfig(cx);
            if (!JS_ParseJSON(cx, jsConfigSource, &jsConfig)) {
                LOGV2_ERROR(716282, "Unable to parse JSON.", "jsonConfigPath"_attr = jsConfigPath);
                break;
            }

            JS::RootedObject jsConfigObject(cx, &jsConfig.toObject());
            JS::RootedValue compilerOptionsValue(cx);
            if (!JS_GetProperty(
                    cx, jsConfigObject, kCompileOptionsPropertyName, &compilerOptionsValue)) {
                break;
            }

            JS::RootedObject compilerOptionsObject(cx, &compilerOptionsValue.toObject());
            JS::RootedValue baseUrlValue(cx);
            if (!JS_GetProperty(cx, compilerOptionsObject, kBaseUrlPropertyName, &baseUrlValue) ||
                !baseUrlValue.isString()) {
                break;
            }

            JS::RootedString baseUrlString(cx, baseUrlValue.toString());
            JS::UniqueChars baseUrlChars = JS_EncodeStringToUTF8(cx, baseUrlString);
            uassert(ErrorCodes::JSInterpreterFailure,
                    "Failed to UTF-8 encode baseUrl from jsconfig.json",
                    baseUrlChars);  // returns nullptr on OOM/encoding errors
            std::string baseUrl{baseUrlChars.get()};

            boost::system::error_code ec;
            auto resolvedPath =
                boost::filesystem::canonical(it->path().parent_path() / baseUrl, ec);
            if (ec) {
                LOGV2_ERROR(
                    716283, "Unable to resolve parsed baseUrl.", "error"_attr = ec.to_string());
                break;
            }

            return resolvedPath.string();
        }

        if (!path.has_parent_path()) {
            break;
        }

        path = path.parent_path();
    }

    return boost::filesystem::current_path().string();
}

}  // namespace mozjs
}  // namespace mongo

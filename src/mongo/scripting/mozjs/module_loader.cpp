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

#include <boost/filesystem.hpp>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/module_loader.h"
#include "mongo/util/file.h"

#include <js/Modules.h>
#include <js/SourceText.h>
#include <js/StableStringChars.h>

namespace mongo {
namespace mozjs {

bool ModuleLoader::init(JSContext* cx, const boost::filesystem::path& loadPath) {
    invariant(loadPath.is_absolute());
    _loadPath = loadPath.string();

    JS::SetModuleResolveHook(JS_GetRuntime(cx), ModuleLoader::moduleResolveHook);
    return true;
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
    JS::RootedString loadPath(cx, JS_NewStringCopyN(cx, _loadPath.c_str(), _loadPath.size()));
    JS::RootedObject info(cx, [&]() {
        if (source) {
            JS::RootedString src(cx, JS_NewStringCopyN(cx, source->rawData(), source->size()));
            return createScriptPrivateInfo(cx, loadPath, src);
        }

        return createScriptPrivateInfo(cx, loadPath, nullptr);
    }());

    if (!info) {
        return nullptr;
    }

    JS::RootedValue referencingPrivate(cx, JS::ObjectValue(*info));
    JS::RootedString specifier(cx, JS_NewStringCopyN(cx, path.c_str(), path.size()));
    JS::RootedObject moduleRequest(cx, JS::CreateModuleRequest(cx, specifier));
    if (!moduleRequest) {
        return nullptr;
    }

    return resolveImportedModule(cx, referencingPrivate, moduleRequest);
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

    bool hasSource;
    JS::RootedObject referencingInfoObject(cx, &referencingInfo.toObject());
    if (!JS_HasProperty(cx, referencingInfoObject, "source", &hasSource)) {
        return nullptr;
    }

    if (hasSource) {
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

    boost::filesystem::path specifierPath(JS_EncodeStringToUTF8(cx, specifierString).get());
    auto refAbsPath = boost::filesystem::path(JS_EncodeStringToUTF8(cx, refPath).get());
    if (is_directory(specifierPath)) {
        JS_ReportErrorUTF8(cx,
                           "Directory import '%s' is not supported, imported from %s",
                           specifierPath.c_str(),
                           refAbsPath.c_str());
        return nullptr;
    }

    if (!specifierPath.is_relative()) {
        return specifierString;
    }

    boost::system::error_code ec;
    auto fullPath = [&]() {
        if (!boost::filesystem::is_directory(refAbsPath)) {
            return boost::filesystem::canonical(specifierPath, refAbsPath.parent_path(), ec)
                .lexically_normal()
                .string();
        }

        return boost::filesystem::canonical(specifierPath, refAbsPath, ec)
            .lexically_normal()
            .string();
    }();

    if (ec) {
        if (ec.value() == boost::system::errc::no_such_file_or_directory) {
            JS_ReportErrorUTF8(cx,
                               "Cannot find module '%s' imported from %s",
                               specifierPath.c_str(),
                               refAbsPath.c_str());
        } else {
            JS_ReportErrorUTF8(cx, "%s", ec.message().c_str());
        }

        return nullptr;
    }

    return JS_NewStringCopyN(cx, fullPath.c_str(), fullPath.size());
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

    JS::UniqueChars filename = JS_EncodeStringToLatin1(cx, path);
    if (!filename) {
        return nullptr;
    }

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.get(), 1);

    JS::RootedString source(cx, fetchSource(cx, path, referencingPrivate));
    if (!source) {
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

    module = JS::CompileModule(cx, options, srcBuf);
    if (!module) {
        return nullptr;
    }

    JS::RootedObject info(cx, createScriptPrivateInfo(cx, path, nullptr));
    if (!info) {
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

enum GlobalAppSlot { GlobalAppSlotModuleRegistry, GlobalAppSlotCount };
JSObject* ModuleLoader::getOrCreateModuleRegistry(JSContext* cx) {
    JSObject* global = JS::CurrentGlobalOrNull(cx);
    if (!global) {
        return nullptr;
    }

    JS::RootedValue value(cx, JS::GetReservedSlot(global, GlobalAppSlotModuleRegistry));
    if (!value.isUndefined()) {
        return &value.toObject();
    }

    JSObject* registry = JS::NewMapObject(cx);
    if (!registry) {
        return nullptr;
    }

    JS::SetReservedSlot(global, GlobalAppSlotModuleRegistry, JS::ObjectValue(*registry));
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
    if (len < 0) {
        JS_ReportErrorUTF8(cx, "can't read length of %s", pathname.get());
        return nullptr;
    }

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
                                                JS::Handle<JSString*> source) {
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
        JS::Rooted<JS::Value> sourceValue(cx, JS::StringValue(source));
        if (!JS_DefineProperty(cx, info, "source", sourceValue, JSPROP_ENUMERATE)) {
            return nullptr;
        }
    }

    return info;
}

}  // namespace mozjs
}  // namespace mongo

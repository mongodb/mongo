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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <dlfcn.h>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

SharedLibrary::~SharedLibrary() {
    if (_handle) {
        if (dlclose(_handle) != 0) {
            LOGV2_DEBUG(
                22612, 2, "Load Library close failed {dlerror}", "dlerror"_attr = dlerror());
        }
    }
}

StatusWith<std::unique_ptr<SharedLibrary>> SharedLibrary::create(
    const boost::filesystem::path& full_path) {
    LOGV2_DEBUG(
        22613, 1, "Loading library: {full_path_c_str}", "full_path_c_str"_attr = full_path.c_str());

    void* handle = dlopen(full_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Load library failed: " << dlerror());
    }

    return StatusWith<std::unique_ptr<SharedLibrary>>(
        std::unique_ptr<SharedLibrary>(new SharedLibrary(handle)));
}

StatusWith<void*> SharedLibrary::getSymbol(StringData name) {
    // Clear dlerror() before calling dlsym,
    // see man dlerror(3) or dlerror(3p) on any POSIX system for details
    // Ignore return
    dlerror();

    // StringData is not assued to be null-terminated
    std::string symbolName = std::string{name};

    void* symbol = dlsym(_handle, symbolName.c_str());

    char* error_msg = dlerror();
    if (error_msg != nullptr) {
        return StatusWith<void*>(ErrorCodes::InternalError,
                                 str::stream() << "dlsym failed for symbol " << name
                                               << " with error message: " << error_msg);
    }

    return StatusWith<void*>(symbol);
}

}  // namespace mongo

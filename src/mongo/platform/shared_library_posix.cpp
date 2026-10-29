// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <string_view>

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

StatusWith<void*> SharedLibrary::getSymbol(std::string_view name) {
    // Clear dlerror() before calling dlsym,
    // see man dlerror(3) or dlerror(3p) on any POSIX system for details
    // Ignore return
    dlerror();

    // std::string_view is not assued to be null-terminated
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

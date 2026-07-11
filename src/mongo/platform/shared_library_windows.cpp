// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/logv2/log.h"
#include "mongo/platform/shared_library.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

#include <string_view>

#include <boost/filesystem.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

SharedLibrary::~SharedLibrary() {
    if (_handle) {
        if (FreeLibrary(static_cast<HMODULE>(_handle)) == 0) {
            auto ec = lastSystemError();
            LOGV2_DEBUG(22614,
                        2,
                        "Load library close failed: {errnoWithDescription_lasterror}",
                        "errnoWithDescription_lasterror"_attr = errorMessage(ec));
        }
    }
}

StatusWith<std::unique_ptr<SharedLibrary>> SharedLibrary::create(
    const boost::filesystem::path& full_path) {
    LOGV2_DEBUG(22615,
                1,
                "Loading library: {toUtf8String_full_path_c_str}",
                "toUtf8String_full_path_c_str"_attr = toUtf8String(full_path.c_str()));

    HMODULE handle = LoadLibraryW(full_path.c_str());
    if (handle == nullptr) {
        auto ec = lastSystemError();
        return StatusWith<std::unique_ptr<SharedLibrary>>(ErrorCodes::InternalError,
                                                          str::stream() << "Load library failed: "
                                                                        << errorMessage(ec));
    }

    return StatusWith<std::unique_ptr<SharedLibrary>>(
        std::unique_ptr<SharedLibrary>(new SharedLibrary(handle)));
}

StatusWith<void*> SharedLibrary::getSymbol(std::string_view name) {
    // std::string_view is not assued to be null-terminated
    std::string symbolName{name};

    void* function = GetProcAddress(static_cast<HMODULE>(_handle), symbolName.c_str());

    if (function == nullptr) {
        DWORD gle = GetLastError();
        if (gle != ERROR_PROC_NOT_FOUND) {
            return StatusWith<void*>(ErrorCodes::InternalError,
                                     str::stream() << "GetProcAddress failed for symbol: "
                                                   << errorMessage(systemError(gle)));
        }
    }

    return StatusWith<void*>(function);
}

}  // namespace mongo

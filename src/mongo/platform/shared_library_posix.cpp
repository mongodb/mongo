/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/platform/shared_library.h"

#include <boost/filesystem.hpp>
#include <dlfcn.h>

#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

SharedLibrary::~SharedLibrary() {
    if (_handle) {
        if (dlclose(_handle) != 0) {
            LOG(2) << "Load Library close failed " << dlerror();
        }
    }
}

StatusWith<std::unique_ptr<SharedLibrary>> SharedLibrary::create(
    const boost::filesystem::path& full_path) {
    LOG(1) << "Loading library: " << full_path.c_str();

    void* handle = dlopen(full_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
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
    std::string symbolName = name.toString();

    void* symbol = dlsym(_handle, symbolName.c_str());

    char* error_msg = dlerror();
    if (error_msg != nullptr) {
        return StatusWith<void*>(ErrorCodes::InternalError,
                                 str::stream() << "dlsym failed for symbol " << name
                                               << " with error message: "
                                               << error_msg);
    }

    return StatusWith<void*>(symbol);
}

}  // namespace mongo

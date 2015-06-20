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
#pragma once

#include <boost/filesystem.hpp>
#include <memory>

#include "mongo/base/status_with.h"

namespace mongo {

/**
 * Loads shared library or DLL at runtime
 * Provides functionality to resolve symols and functions at runtime.
 * Note: shared library is released by destructor
 */
class SharedLibrary {
public:
    /**
     * Releases reference to shared library on destruction.
     *
     * May unload the shared library.
     * May invalidate all symbol pointers, depends on OS implementation.
     */
    ~SharedLibrary();

    /*
     * Loads the shared library
     *
     * Returns a handle to a SharedLibrary on success otherwise StatusWith contains the
     * appropriate error.
     */
    static StatusWith<std::unique_ptr<SharedLibrary>> create(
        const boost::filesystem::path& full_path);

    /**
     * Retrieves the public symbol of a shared library specified in the name parameter.
     *
     * Returns a pointer to the symbol if it exists with Status::OK,
     * returns NULL if the symbol does not exist with Status::OK,
     * otherwise returns an error if the underlying OS infrastructure returns an error.
     */
    StatusWith<void*> getSymbol(StringData name);

    /**
     * A generic function version of getSymbol, see notes in getSymbol for more information
     * Callers should use getFunctionAs.
     */
    StatusWith<void (*)()> getFunction(StringData name);

    /**
     * A type-safe version of getFunction, see notes in getSymbol for more information
     */
    template <typename FuncT>
    StatusWith<FuncT> getFunctionAs(StringData name) {
        StatusWith<void (*)()> s = getFunction(name);

        if (!s.isOK()) {
            return StatusWith<FuncT>(s.getStatus());
        }

        return StatusWith<FuncT>(reinterpret_cast<FuncT>(s.getValue()));
    }

private:
    SharedLibrary(void* handle);

private:
    void* const _handle;
};

}  // namespace mongo

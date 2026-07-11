// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    StatusWith<void*> getSymbol(std::string_view name);

    /**
     * A generic function version of getSymbol, see notes in getSymbol for more information
     * Callers should use getFunctionAs.
     */
    StatusWith<void (*)()> getFunction(std::string_view name);

    /**
     * A type-safe version of getFunction, see notes in getSymbol for more information
     */
    template <typename FuncT>
    StatusWith<FuncT> getFunctionAs(std::string_view name) {
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

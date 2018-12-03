
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

#include "mongo/platform/basic.h"

#include "stitch_support/stitch_support.h"

#include "mongo/base/initializer.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

#include <iostream>
#include <string>

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#else
#define MONGO_API_CALL
#endif

struct stitch_support_v1_status {
    stitch_support_v1_status() noexcept = default;
    stitch_support_v1_status(const stitch_support_v1_error e, const int ec, std::string w)
        : error(e), exception_code(ec), what(std::move(w)) {}

    void clean() noexcept {
        error = STITCH_SUPPORT_V1_SUCCESS;
    }

    stitch_support_v1_error error = STITCH_SUPPORT_V1_SUCCESS;
    int exception_code = 0;
    std::string what;
};

namespace mongo {
namespace {

class StitchSupportException : public std::exception {
public:
    explicit StitchSupportException(const stitch_support_v1_error code, std::string m)
        : _mesg(std::move(m)), _code(code) {}

    stitch_support_v1_error statusCode() const noexcept {
        return this->_code;
    }

    const char* what() const noexcept final {
        return this->_mesg.c_str();
    }

private:
    std::string _mesg;
    stitch_support_v1_error _code;
};

stitch_support_v1_status translateException() try {
    throw;
} catch (const StitchSupportException& ex) {
    return {ex.statusCode(), mongo::ErrorCodes::InternalError, ex.what()};
} catch (const DBException& ex) {
    return {STITCH_SUPPORT_V1_ERROR_EXCEPTION, ex.code(), ex.what()};
} catch (const std::bad_alloc& ex) {
    return {STITCH_SUPPORT_V1_ERROR_ENOMEM, mongo::ErrorCodes::InternalError, ex.what()};
} catch (const std::exception& ex) {
    return {STITCH_SUPPORT_V1_ERROR_UNKNOWN, mongo::ErrorCodes::InternalError, ex.what()};
} catch (...) {
    return {STITCH_SUPPORT_V1_ERROR_UNKNOWN,
            mongo::ErrorCodes::InternalError,
            "Unknown error encountered in performing requested stitch_support_v1 operation"};
}

std::nullptr_t handleException(stitch_support_v1_status& status) noexcept {
    try {
        status = translateException();
    } catch (...) {
        status.error = STITCH_SUPPORT_V1_ERROR_IN_REPORTING_ERROR;

        try {
            status.exception_code = -1;

            status.what.clear();

            // Expected to be small enough to fit in the capacity that string always has.
            const char severeErrorMessage[] = "Severe Error";

            if (status.what.capacity() > sizeof(severeErrorMessage)) {
                status.what = severeErrorMessage;
            }
        } catch (...) /* Ignore any errors at this point. */
        {
        }
    }
    return nullptr;
}

ServiceContext* initialize() {
    srand(static_cast<unsigned>(curTimeMicros64()));

    // The global initializers can take arguments, which would normally be supplied on the command
    // line, but we assume that clients of this library will never want anything other than the
    // defaults for all configuration that would be controlled by these parameters.
    Status status =
        mongo::runGlobalInitializers(0 /* argc */, nullptr /* argv */, nullptr /* envp */);
    uassertStatusOKWithContext(status, "Global initialization failed");
    setGlobalServiceContext(ServiceContext::make());

    return getGlobalServiceContext();
}

struct ServiceContextDestructor {
    /**
     * This destructor gets called when the Stitch Library gets torn down, either by a call to
     * stitch_support_v1_fini() or when the process exits.
     */
    void operator()(mongo::ServiceContext* const serviceContext) const noexcept {
        Status status = mongo::runGlobalDeinitializers();
        uassertStatusOKWithContext(status, "Global deinitilization failed");

        setGlobalServiceContext(nullptr);
    }
};

using EmbeddedServiceContextPtr = std::unique_ptr<mongo::ServiceContext, ServiceContextDestructor>;

}  // namespace
}  // namespace mongo

struct stitch_support_v1_lib {
    stitch_support_v1_lib() : serviceContext(mongo::initialize()) {}

    stitch_support_v1_lib(const stitch_support_v1_lib&) = delete;
    void operator=(const stitch_support_v1_lib&) = delete;

    mongo::EmbeddedServiceContextPtr serviceContext;
};

namespace mongo {
namespace {

std::unique_ptr<stitch_support_v1_lib> library;

stitch_support_v1_lib* stitch_lib_init() {
    if (library) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_LIBRARY_ALREADY_INITIALIZED,
            "Cannot initialize the Stitch Support Library when it is already initialized."};
    }

    library = std::make_unique<stitch_support_v1_lib>();

    return library.get();
}

void stitch_lib_fini(stitch_support_v1_lib* const lib, stitch_support_v1_status& status) {
    if (!lib) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
            "Cannot close a `NULL` pointer referencing a Stitch Support Library Instance"};
    }

    if (!library) {
        throw StitchSupportException{
            STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED,
            "Cannot close the Stitch Support Library when it is not initialized"};
    }

    if (library.get() != lib) {
        throw StitchSupportException{STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE,
                                     "Invalid Stitch Support Library handle."};
    }

    library.reset();
}

template <typename Function,
          typename ReturnType =
              decltype(std::declval<Function>()(*std::declval<stitch_support_v1_status*>()))>
struct enterCXXImpl;

template <typename Function>
struct enterCXXImpl<Function, void> {
    template <typename Callable>
    static int call(Callable&& function, stitch_support_v1_status& status) noexcept {
        try {
            function(status);
        } catch (...) {
            handleException(status);
        }
        return status.error;
    }
};

template <typename Function, typename Pointer>
struct enterCXXImpl<Function, Pointer*> {
    template <typename Callable>
    static Pointer* call(Callable&& function, stitch_support_v1_status& status) noexcept try {
        return function(status);
    } catch (...) {
        return handleException(status);
    }
};

int capi_status_get_error(const stitch_support_v1_status* const status) noexcept {
    invariant(status);
    return status->error;
}

const char* capi_status_get_what(const stitch_support_v1_status* const status) noexcept {
    invariant(status);
    return status->what.c_str();
}

int capi_status_get_code(const stitch_support_v1_status* const status) noexcept {
    invariant(status);
    return status->exception_code;
}

}  // namespace
}  // namespace mongo

namespace {

struct StatusGuard {
private:
    stitch_support_v1_status* status;
    stitch_support_v1_status fallback;

public:
    explicit StatusGuard(stitch_support_v1_status* const statusPtr) noexcept : status(statusPtr) {
        if (status)
            status->clean();
    }

    stitch_support_v1_status& get() noexcept {
        return status ? *status : fallback;
    }

    const stitch_support_v1_status& get() const noexcept {
        return status ? *status : fallback;
    }

    operator stitch_support_v1_status&() & noexcept {
        return this->get();
    }
    operator stitch_support_v1_status&() && noexcept {
        return this->get();
    }
};

template <typename Callable>
auto enterCXX(stitch_support_v1_status* const statusPtr, Callable&& c) noexcept
    -> decltype(mongo::enterCXXImpl<Callable>::call(std::forward<Callable>(c), *statusPtr)) {
    StatusGuard status(statusPtr);
    return mongo::enterCXXImpl<Callable>::call(std::forward<Callable>(c), status);
}

}  // namespace

extern "C" {

stitch_support_v1_lib* MONGO_API_CALL stitch_support_v1_init(stitch_support_v1_status* status) {
    return enterCXX(status,
                    [&](stitch_support_v1_status& status) { return mongo::stitch_lib_init(); });
}

int MONGO_API_CALL stitch_support_v1_fini(stitch_support_v1_lib* const lib,
                                          stitch_support_v1_status* const status) {
    return enterCXX(status, [&](stitch_support_v1_status& status) {
        return mongo::stitch_lib_fini(lib, status);
    });
}

int MONGO_API_CALL
stitch_support_v1_status_get_error(const stitch_support_v1_status* const status) {
    return mongo::capi_status_get_error(status);
}

const char* MONGO_API_CALL
stitch_support_v1_status_get_explanation(const stitch_support_v1_status* const status) {
    return mongo::capi_status_get_what(status);
}

int MONGO_API_CALL stitch_support_v1_status_get_code(const stitch_support_v1_status* const status) {
    return mongo::capi_status_get_code(status);
}

stitch_support_v1_status* MONGO_API_CALL stitch_support_v1_status_create(void) {
    return new stitch_support_v1_status;
}

void MONGO_API_CALL stitch_support_v1_status_destroy(stitch_support_v1_status* const status) {
    delete status;
}

}  // extern "C"

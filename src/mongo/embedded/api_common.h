// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <string>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A status object for use with C APIs that call into C++ code. When using StatusForAPI for error
 * reporting, call any C++ within an enterCXX() block, which can translate exceptions into
 * StatusForApi objects by way of handleException().
 *
 * The ErrorEnum type parameter is a C enum with error codes that will be exposed to the clients of
 * the C interface. This enum _must_ include a value that maps to 0, which will be the default value
 * for a clean Status object (usually indicating success).
 */
template <typename ErrorEnum>
struct StatusForAPI {
    StatusForAPI() noexcept = default;
    StatusForAPI(const ErrorEnum e, const int ec, std::string w)
        : error(e), exception_code(ec), what(std::move(w)) {}

    void clean() noexcept {
        error = {};
        exception_code = {};
        what.clear();
    }

    ErrorEnum error = {};
    int exception_code = {};
    std::string what;
};

/**
 * Used to set a fallback error message when it is not safe to allocate memory.
 */
inline void setErrorMessageNoAlloc(std::string& errorString) {
    errorString.clear();

    // Expected to be small enough to fit in the capacity that string always has.
    const char severeErrorMessage[] = "Severe Error";

    if (errorString.capacity() > sizeof(severeErrorMessage)) {
        errorString = severeErrorMessage;
    }
}

template <typename ErrorEnum>
class ExceptionForAPI : public std::exception {
public:
    explicit ExceptionForAPI(const ErrorEnum code, std::string m)
        : _mesg(std::move(m)), _code(code) {}
    ~ExceptionForAPI() override {}

    ErrorEnum statusCode() const noexcept {
        return this->_code;
    }

    const char* what() const noexcept override {
        return this->_mesg.c_str();
    }

private:
    std::string _mesg;
    ErrorEnum _code;
};

template <typename ErrorEnum>
std::nullptr_t handleException(StatusForAPI<ErrorEnum>& status) noexcept {
    try {
        status = translateException(std::type_identity<StatusForAPI<ErrorEnum>>());
    } catch (...) {
        try {
            translateExceptionFallback(status);
        } catch (...) /* Ignore any errors at this point. */
        {
        }
    }
    return nullptr;
}

class ReentrancyGuard {
public:
    ReentrancyGuard() {
        uassert(
            ErrorCodes::ReentrancyNotAllowed, "Reentry into library is not allowed", !_inLibrary);
        _inLibrary = true;
    }

    ~ReentrancyGuard() {
        _inLibrary = false;
    }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;

private:
    static thread_local inline bool _inLibrary = false;
};

template <typename Status,
          typename Function,
          typename ReturnType = typename std::invoke_result<Function>::type>
struct enterCXXImpl;

template <typename Status, typename Function>
struct enterCXXImpl<Status, Function, void> {
    template <typename Callable>
    static int call(Callable&& function, Status& status, const ReentrancyGuard&& = {}) noexcept {
        try {
            function();
        } catch (...) {
            handleException(status);
        }
        return status.error;
    }
};


template <typename Status, typename Function, typename Pointer>
struct enterCXXImpl<Status, Function, Pointer*> {
    template <typename Callable>
    static Pointer* call(Callable&& function, Status& status, const ReentrancyGuard&& = {}) noexcept
        try {
        return function();
    } catch (...) {
        return handleException(status);
    }
};

template <typename Status>
struct StatusGuard {
private:
    Status fallback;
    Status* status;

public:
    explicit StatusGuard(Status* const statusPtr) noexcept
        : status(statusPtr ? statusPtr : &fallback) {
        if (status == statusPtr) {
            status->clean();
        }
    }

    StatusGuard(StatusGuard const&) = delete;
    StatusGuard& operator=(StatusGuard const&) = delete;

    operator Status&() noexcept {
        return *status;
    }
};

template <typename Status, typename Callable>
auto enterCXX(Status* const statusPtr, Callable&& c) noexcept
    -> decltype(mongo::enterCXXImpl<Status, Callable>::call(std::forward<Callable>(c),
                                                            *statusPtr)) {
    return mongo::enterCXXImpl<Status, Callable>::call(std::forward<Callable>(c),
                                                       StatusGuard<Status>(statusPtr));
}

}  // namespace mongo

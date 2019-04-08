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

#include <string>

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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
    virtual ~ExceptionForAPI() {}

    ErrorEnum statusCode() const noexcept {
        return this->_code;
    }

    const char* what() const noexcept {
        return this->_mesg.c_str();
    }

private:
    std::string _mesg;
    ErrorEnum _code;
};

template <typename ErrorEnum>
std::nullptr_t handleException(StatusForAPI<ErrorEnum>& status) noexcept {
    try {
        status = translateException(stdx::type_identity<StatusForAPI<ErrorEnum>>());
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
private:
    // This templated "holder" struct stores a thread-local static variable that doesn't need a
    // corresponding definition in a .cpp file, allowing us to use this same header from multiple
    // libraries that do not link common objects.
    // TODO: When we switch to C++17, we can use simply use the "inline" keyword instead of relying
    // on templates.
    template <typename T = void>
    struct GuardHolder {
        thread_local static bool inLibrary;
    };

    bool& inLibrary() {
        return GuardHolder<>::inLibrary;
    }

public:
    ReentrancyGuard() {
        uassert(ErrorCodes::ReentrancyNotAllowed,
                str::stream() << "Reentry into library is not allowed",
                !inLibrary());
        inLibrary() = true;
    }

    ~ReentrancyGuard() {
        inLibrary() = false;
    }

    ReentrancyGuard(ReentrancyGuard const&) = delete;
    ReentrancyGuard& operator=(ReentrancyGuard const&) = delete;
};

template <typename T>
thread_local bool ReentrancyGuard::GuardHolder<T>::inLibrary = false;

template <typename Status,
          typename Function,
          typename ReturnType = typename std::result_of<Function()>::type>
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
    static Pointer* call(Callable&& function,
                         Status& status,
                         const ReentrancyGuard&& = {}) noexcept try {
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

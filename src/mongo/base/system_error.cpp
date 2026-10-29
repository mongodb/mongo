// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/system_error.h"

#include <string>

namespace mongo {

namespace {

/**
 * A std::error_category for the codes in the named ErrorCodes space.
 */
class MongoErrorCategoryImpl final : public std::error_category {
public:
    MongoErrorCategoryImpl() = default;

    const char* name() const noexcept override {
        return "mongo";
    }

    std::string message(int ev) const override {
        return ErrorCodes::errorString(ErrorCodes::Error(ev));
    }

    // We don't really want to override this function, but to override the second we need to,
    // otherwise there will be issues with overload resolution.
    bool equivalent(const int code, const std::error_condition& condition) const noexcept override {
        return std::error_category::equivalent(code, condition);
    }

    bool equivalent(const std::error_code& code, int condition) const noexcept override {
        switch (ErrorCodes::Error(condition)) {
            case ErrorCodes::OK:
                // Make ErrorCodes::OK to be equivalent to the default constructed error code.
                return code == std::error_code();
            default:
                return false;
        }
    }
};

}  // namespace

const std::error_category& mongoErrorCategory() {
    // TODO: Remove this static, and make a constexpr instance when we move to C++14.
    static const MongoErrorCategoryImpl instance{};
    return instance;
}

std::error_code make_error_code(ErrorCodes::Error code) {
    return std::error_code(ErrorCodes::Error(code), mongoErrorCategory());
}

std::error_condition make_error_condition(ErrorCodes::Error code) {
    return std::error_condition(ErrorCodes::Error(code), mongoErrorCategory());
}

}  // namespace mongo

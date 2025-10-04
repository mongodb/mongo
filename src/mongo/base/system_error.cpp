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

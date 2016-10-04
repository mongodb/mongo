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

#include "mongo/platform/basic.h"

#include <boost/config.hpp>
#include <string>

#include "mongo/base/system_error.h"

namespace mongo {

namespace {

/**
 * A std::error_category for the codes in the named ErrorCodes space.
 */
class MongoErrorCategoryImpl final : public std::error_category {
public:
    MongoErrorCategoryImpl() = default;

    const char* name() const BOOST_NOEXCEPT override {
        return "mongo";
    }

    std::string message(int ev) const override {
        return ErrorCodes::errorString(ErrorCodes::fromInt(ev));
    }

    // We don't really want to override this function, but to override the second we need to,
    // otherwise there will be issues with overload resolution. Additionally, the use of
    // BOOST_NOEXCEPT is necessitated by the libc++/libstdc++ STL having 'noexcept' on the
    // overridden methods, but not the Dinkumware STL as of MSVC 2013.
    bool equivalent(const int code,
                    const std::error_condition& condition) const BOOST_NOEXCEPT override {
        return std::error_category::equivalent(code, condition);
    }

    bool equivalent(const std::error_code& code, int condition) const BOOST_NOEXCEPT override {
        switch (ErrorCodes::fromInt(condition)) {
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
    return std::error_code(ErrorCodes::fromInt(code), mongoErrorCategory());
}

std::error_condition make_error_condition(ErrorCodes::Error code) {
    return std::error_condition(ErrorCodes::fromInt(code), mongoErrorCategory());
}

}  // namespace mongo

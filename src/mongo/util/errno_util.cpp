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

#include "mongo/util/errno_util.h"

#include <cerrno>
#include <fmt/format.h>
#include <system_error>

#ifndef _WIN32
#include <netdb.h>
#endif

namespace mongo {

using namespace fmt::literals;

class AddrInfoErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "getaddrinfo";
    }

    std::string message(int e) const override {
#ifdef _WIN32
        return systemError(e).message();
#else
        return gai_strerror(e);
#endif
    }
};

const std::error_category& addrInfoCategory() {
    static auto p = new AddrInfoErrorCategory;
    return *p;
}

std::string errorMessage(std::error_code ec) {
    std::string r = ec.message();
    bool vague = false;
#if defined(_WIN32)
    vague = (r == "unknown error"_sd);
#elif defined(_LIBCPP_VERSION)
    vague = StringData{r}.startsWith("unspecified"_sd);
#endif
    if (vague)
        return "Unknown error {}"_format(ec.value());
    return r;
}

}  // namespace mongo

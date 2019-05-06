
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

#ifndef MONGO_UTIL_DNS_QUERY_PLATFORM_INCLUDE_WHITELIST
#error Do not include the DNS Query platform implementation headers.  Please use "mongo/util/dns_query.h" instead.
#endif

#include <stdexcept>
#include <string>
#include <vector>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace dns {
namespace {

enum class DNSQueryClass { kInternet };

enum class DNSQueryType { kSRV, kTXT, kAddress };

[[noreturn]] void throwNotSupported() {
    uasserted(ErrorCodes::BadValue, "srv_nsearch not supported on android");
}

class ResourceRecord {
public:
    explicit ResourceRecord() = default;

    std::vector<std::string> txtEntry() const {
        throwNotSupported();
    }

    std::string addressEntry() const {
        throwNotSupported();
    }

    SRVHostEntry srvHostEntry() const {
        throwNotSupported();
    }
};

using DNSResponse = std::vector<ResourceRecord>;

class DNSQueryState {
public:
    DNSResponse lookup(const std::string&, const DNSQueryClass, const DNSQueryType) {
        throwNotSupported();
    }

    DNSQueryState() {
        throwNotSupported();
    }
};
}  // namespace
}  // namespace dns
}  // namespace mongo

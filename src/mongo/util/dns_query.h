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

#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/noncopyable.hpp>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace dns {
/**
 * An `SRVHostEntry` object represents the information received from a DNS lookup of an SRV record.
 * NOTE: An SRV DNS record has several fields, such as priority and weight.  This structure lacks
 * those fields at this time.  They should be safe to add in the future.  The code using this
 * structure does not need access to those fields at this time.
 */
struct SRVHostEntry {
    std::string host;
    std::uint16_t port;

    SRVHostEntry(std::string initialHost, const std::uint16_t initialPort)
        : host(std::move(initialHost)), port(initialPort) {}

    inline auto makeRelopsLens() const {
        return std::tie(host, port);
    }

    inline std::string toString() const {
        return std::string{host} + ":" + std::to_string(port);
    }

    inline friend std::ostream& operator<<(std::ostream& os, const SRVHostEntry& entry) {
        return os << entry.host << ':' << entry.port;
    }

    inline friend bool operator==(const SRVHostEntry& lhs, const SRVHostEntry& rhs) {
        return lhs.makeRelopsLens() == rhs.makeRelopsLens();
    }

    inline friend bool operator!=(const SRVHostEntry& lhs, const SRVHostEntry& rhs) {
        return !(lhs == rhs);
    }

    inline friend bool operator<(const SRVHostEntry& lhs, const SRVHostEntry& rhs) {
        return lhs.makeRelopsLens() < rhs.makeRelopsLens();
    }
};

/**
 * Returns a vector containing SRVHost entries for the specified `service`.
 * THROWS: `DBException` with `ErrorCodes::DNSHostNotFound` as the status value if the entry is not
 * found and `ErrorCodes::DNSProtocolError` as the status value if the DNS lookup fails, for any
 * other reason
 */
std::vector<SRVHostEntry> lookupSRVRecords(const std::string& service);

/**
 * Returns a group of strings containing text from DNS TXT entries for a specified service.
 * THROWS: `DBException` with `ErrorCodes::DNSHostNotFound` as the status value if the entry is not
 * found and `ErrorCodes::DNSProtocolError` as the status value if the DNS lookup fails, for any
 * other reason
 */
std::vector<std::string> lookupTXTRecords(const std::string& service);

/**
 * Returns a group of strings containing text from DNS TXT entries for a specified service.
 * If the lookup fails because the record doesn't exist, an empty vector is returned.
 * THROWS: `DBException` with `ErrorCodes::DNSProtocolError` as the status value if the DNS lookup
 * fails for any other reason.
 */
std::vector<std::string> getTXTRecords(const std::string& service);

/**
 * Returns a group of strings containing Address entries for a specified service.
 * THROWS: `DBException` with `ErrorCodes::DNSHostNotFound` as the status value if the entry is not
 * found and `ErrorCodes::DNSProtocolError` as the status value if the DNS lookup fails, for any
 * other reason.
 * NOTE: This function mostly exists to provide an easy test of the OS DNS APIs in our test driver.
 */
std::vector<std::string> lookupARecords(const std::string& service);
}  // namespace dns
}  // namespace mongo

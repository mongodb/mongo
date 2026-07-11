// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <array>
#include <compare>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/noncopyable.hpp>

[[MONGO_MOD_PUBLIC]];

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
std::vector<std::pair<SRVHostEntry, Seconds>> lookupSRVRecords(const std::string& service);

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
 * Returns a vector of pairs (string, Seconds), where each pair is: a string containing address
 * entries for a specified servie, and the corresponding records time to live for a record.

 * Returns a group of strings containing Address entries for a specified service.

 * THROWS: `DBException` with `ErrorCodes::DNSHostNotFound` as the status value if the entry is not
 * found and `ErrorCodes::DNSProtocolError` as the status value if the DNS lookup fails, for any
 * other reason.
 * NOTE: This function mostly exists to provide an easy test of the OS DNS APIs in our test driver.
 */
std::vector<std::pair<std::string, Seconds>> lookupARecords(const std::string& service);
}  // namespace dns
}  // namespace mongo

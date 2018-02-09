/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
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

#include "mongo/util/dns_query.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/noncopyable.hpp>

#include "mongo/bson/util/builder.h"

// It is safe to include the implementation "headers" in an anonymous namespace, as the code is
// meant to live in a single TU -- this one.  Include one of these headers last.
#define MONGO_UTIL_DNS_QUERY_PLATFORM_INCLUDE_WHITELIST
#ifdef WIN32
#include "mongo/util/dns_query_windows-impl.h"
#else
#include "mongo/util/dns_query_posix-impl.h"
#endif
#undef MONGO_UTIL_DNS_QUERY_PLATFORM_INCLUDE_WHITELIST

using std::begin;
using std::end;
using namespace std::literals::string_literals;

namespace mongo {

/**
 * Returns a string with the IP address or domain name listed...
 */
std::vector<std::string> dns::lookupARecords(const std::string& service) {
    DNSQueryState dnsQuery;
    auto response = dnsQuery.lookup(service, DNSQueryClass::kInternet, DNSQueryType::kAddress);

    std::vector<std::string> rv;

    for (const auto& entry : response) {
        try {
            rv.push_back(entry.addressEntry());
        } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
        }
    }

    if (rv.empty()) {
        StringBuilder oss;
        oss << "Looking up " << service << " A record yielded ";
        if (response.size() == 0) {
            oss << "no results.";
        } else {
            oss << "no A records but " << response.size() << " other records";
        }
        uasserted(ErrorCodes::DNSProtocolError, oss.str());
    }

    return rv;
}

std::vector<dns::SRVHostEntry> dns::lookupSRVRecords(const std::string& service) {
    DNSQueryState dnsQuery;

    auto response = dnsQuery.lookup(service, DNSQueryClass::kInternet, DNSQueryType::kSRV);

    std::vector<SRVHostEntry> rv;

    for (const auto& entry : response) {
        try {
            rv.push_back(entry.srvHostEntry());
        } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
        }
    }

    if (rv.empty()) {
        StringBuilder oss;
        oss << "Looking up " << service << " SRV record yielded ";
        if (response.size() == 0) {
            oss << "no results.";
        } else {
            oss << "no SRV records but " << response.size() << " other records";
        }
        uasserted(ErrorCodes::DNSProtocolError, oss.str());
    }

    return rv;
}

std::vector<std::string> dns::lookupTXTRecords(const std::string& service) {
    DNSQueryState dnsQuery;

    auto response = dnsQuery.lookup(service, DNSQueryClass::kInternet, DNSQueryType::kTXT);

    std::vector<std::string> rv;

    for (auto& entry : response) {
        try {
            auto txtEntry = entry.txtEntry();
            rv.insert(end(rv),
                      std::make_move_iterator(begin(txtEntry)),
                      std::make_move_iterator(end(txtEntry)));
        } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
        }
    }

    return rv;
}

std::vector<std::string> dns::getTXTRecords(const std::string& service) try {
    return lookupTXTRecords(service);
} catch (const ExceptionFor<ErrorCodes::DNSHostNotFound>&) {
    return {};
}
}  // namespace mongo

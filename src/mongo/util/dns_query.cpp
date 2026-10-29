// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/dns_query.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"

#include <iterator>
#include <string>
#include <vector>

// It is safe to include the implementation "headers" in an anonymous namespace, as the code is
// meant to live in a single TU -- this one.  Include one of these headers last.
#define MONGO_ALLOW_INCLUDE_UTIL_DNS_QUERY_PLATFORM
#ifdef WIN32
#include "mongo/util/dns_query_windows-impl.h"
#else
#include "mongo/util/dns_query_posix-impl.h"
#endif
#undef MONGO_ALLOW_INCLUDE_UTIL_DNS_QUERY_PLATFORM

using namespace std::literals::string_literals;

namespace mongo {

/**
 * Returns a string with the IP address or domain name listed...
 */
std::vector<std::pair<std::string, Seconds>> dns::lookupARecords(const std::string& service) {
    auto response =
        DNSQueryState().lookup(service, DNSQueryClass::kInternet, DNSQueryType::kAddress);

    std::vector<std::pair<std::string, Seconds>> res;

    for (const auto& entry : response) {
        try {
            if (entry.getType() == DNSQueryType::kCNAME) {
                return lookupARecords(entry.cnameEntry());
            }
            res.emplace_back(entry.addressEntry(), entry.getTtl());
        } catch (const ExceptionFor<ErrorCodes::DNSRecordTypeMismatch>&) {
        }
    }

    if (res.empty()) {
        StringBuilder oss;
        oss << "Looking up " << service << " A record yielded ";
        if (response.size() == 0) {
            oss << "no results.";
        } else {
            oss << "no A records but " << response.size() << " other records";
        }
        uasserted(ErrorCodes::DNSProtocolError, oss.str());
    }

    return res;
}

std::vector<std::pair<dns::SRVHostEntry, Seconds>> dns::lookupSRVRecords(
    const std::string& service) {
    DNSQueryState dnsQuery;

    auto response = dnsQuery.lookup(service, DNSQueryClass::kInternet, DNSQueryType::kSRV);

    std::vector<std::pair<dns::SRVHostEntry, Seconds>> rv;

    for (const auto& entry : response) {
        try {
            rv.push_back({entry.srvHostEntry(), entry.getTtl()});
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
            using std::begin;
            using std::end;
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

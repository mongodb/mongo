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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/restriction.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/cidr.h"

#include <memory>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#if !defined(_WIN32) && !defined(_WIN64)
#include <net/if.h>
#endif

namespace mongo {

namespace address_restriction_detail {

struct ClientSource {
    static constexpr auto label = "Client source "_sd;
    static constexpr auto field = "clientSource"_sd;
    static auto addr(const RestrictionEnvironment& environment) {
        return environment.getClientSource();
    }
};
struct ServerAddress {
    static constexpr auto label = "Server address "_sd;
    static constexpr auto const field = "serverAddress"_sd;
    static auto addr(const RestrictionEnvironment& environment) {
        return environment.getServerAddress();
    }
};

// Represents a restriction based on client or server address
template <typename T>
class AddressRestriction : public NamedRestriction {
public:
    /**
     * Construct an empty AddressRestriction.
     * Note that an empty AddressRestriciton will not validate
     * against any addresses, since nothing has been allowlisted.
     */
    AddressRestriction() = default;

    /**
     * Construct an AddressRestriction based on a human readable subnet spec
     */
    explicit AddressRestriction(const StringData range) : _ranges({CIDR(range)}) {}

    /**
     * Construct an AddressRestriction based on several human readable subnet specs
     */
    template <typename StringType>
    explicit AddressRestriction(const std::vector<StringType>& ranges) {
        for (auto const& range : ranges) {
            _ranges.emplace_back(range);
        }
    }

    /**
     * Convert a string to an int, returning 0 on failure
     */
    unsigned int convertToInteger(std::string str) const {
        try {
            unsigned long val = std::stoul(str, nullptr, 10);
            if (val > std::numeric_limits<unsigned int>::max()) {
                return 0;
            }
            return static_cast<unsigned int>(val);
        } catch (const std::exception&) {
            return 0;
        }
    }

    /**
     * Given a string representing a network interface name or a numeric scope ID,
     * return the numeric scope ID.
     * @param ifName The network interface name or numeric scope ID.
     * On Windows, only the numeric scope ID in string format is supported.
     * On non-Windows platforms, ifName is first looked up as a network interface name.
     * If that fails, it is then interpreted as a numeric scope ID in string format.
     * Returns 0 on failure.
     */
    unsigned int getScopeID(std::string ifName) const {
#if defined(_WIN32) || defined(_WIN64)
        // On Windows, we only support the ifName as the scope ID number in string format
        return convertToInteger(ifName);
#else
        unsigned int my_scope_id = if_nametoindex(ifName.c_str());
        if (my_scope_id == 0) {
            // If the interface is not found, try to interpret the ifName as a number
            my_scope_id = convertToInteger(ifName);
        }
        return my_scope_id;
#endif
    }
    /**
     * Returns true if the Environment's client/server's address
     * satisfies this restriction set.
     */
    Status validate(const RestrictionEnvironment& environment) const override {
        auto const addr = T::addr(environment);
        if (addr.getType() == AF_UNSPEC) {
            // GRPCTransportLayer doesn't know server local address.
            return {ErrorCodes::AuthenticationRestrictionUnmet,
                    fmt::format("{} restriction can not be verified when address is unknown",
                                T::label)};
        }

        if (!addr.isIP()) {
            std::ostringstream s;
            s << T::label << " is not an IP address: " << addr.getAddr();
            return {ErrorCodes::AuthenticationRestrictionUnmet, s.str()};
        }

        // The addr.getAddr() contains the address we want to validate. For IPv6 link-local
        // addresses, this may contain a zone index. Example: fe80::8ff:ecff:fee6:fd4d%ens5.
        // The CIDR class strips out the zone index if present, and places it in the scopeStr
        // field. The scope ID is the numeric equivalent of the zone index, and is needed
        // when binding to a link-local address. Sample CIDR address for the above example:
        // fe80::8ff:ecff:fee6:fd4d/128. The scopeStr would be "ens5" and the scopeID would be
        // the numeric equivalent of ens5.
        const CIDR address(addr.getAddr());

        // Example range for a single IPv6 link-local address: fe80::8ff:c4ff:fe74:9d09/128
        for (auto const& range : _ranges) {
            if (address.getLinkLocal() && (address.getScopeStr().length() > 0) &&
                range.getLinkLocal() && range.getScopeStr().length() > 0) {
                // If the restriction contains a scope ID, we need to match it
                // with the address scope ID.
                unsigned int address_scopeID = getScopeID(address.getScopeStr());
                unsigned int range_scopeID = getScopeID(range.getScopeStr());
                if (range_scopeID != address_scopeID) {
                    std::ostringstream s;
                    s << "Scope for IPv6 address " << addr.getAddr() << "(" << address.getScopeStr()
                      << ") does not match scope in restriction " << range << "("
                      << range.getScopeStr() << ")";
                    serialize(s);
                    return {ErrorCodes::AuthenticationRestrictionUnmet, s.str()};
                }
            }
            if (range.contains(address)) {
                return Status::OK();
            }
        }

        std::ostringstream s;
        s << addr.getAddr() << " does not fall within: ";
        serialize(s);
        return {ErrorCodes::AuthenticationRestrictionUnmet, s.str()};
    }

    /**
     * Append to builder an array element with the human-readable CIDR ranges.
     */
    void appendToBuilder(BSONObjBuilder* builder) const override {
        BSONArrayBuilder b{builder->subarrayStart(T::field)};
        for (auto const& range : _ranges) {
            b.append(range.toString());
        }
    }

    friend bool operator==(const AddressRestriction& lhs, const AddressRestriction& rhs) {
        return lhs._ranges == rhs._ranges;
    }

    friend void appendToBson(BSONObjBuilder& bob,
                             StringData fieldName,
                             const AddressRestriction& ar) {
        BSONObjBuilder sub{bob.subobjStart(fieldName)};
        ar.appendToBuilder(&sub);
    }

private:
    void serialize(std::ostream& os) const override {
        os << "{\"" << T::field << "\": [";
        auto sz = _ranges.size() - 1;
        for (auto const& range : _ranges) {
            os << '"' << range << '"';
            if (sz--) {
                os << ", ";
            }
        }
        os << "]}";
    }

    std::vector<CIDR> _ranges;
};
}  // namespace address_restriction_detail

using ClientSourceRestriction =
    address_restriction_detail::AddressRestriction<address_restriction_detail::ClientSource>;
using ServerAddressRestriction =
    address_restriction_detail::AddressRestriction<address_restriction_detail::ServerAddress>;

/**
 * Parse a set of clientSource, serverAddress, or both
 * and return a RestrictionSet on success.
 */
StatusWith<RestrictionSet<>> parseAddressRestrictionSet(const BSONObj& obj);

/**
 * Parse a BSON representation of an array of RestrictionSet<AddressRestriction<T>>s
 * and return a SharedRestrictionDocument on success.
 */
StatusWith<SharedRestrictionDocument> parseAuthenticationRestriction(const BSONArray& arr);

/**
 * Parse and validate a BSONArray containing AuthenticationRestrictions
 * and return a new BSONArray representing a sanitized portion thereof.
 */
StatusWith<BSONArray> getRawAuthenticationRestrictions(const BSONArray& arr);

}  // namespace mongo

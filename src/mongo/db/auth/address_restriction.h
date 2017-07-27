/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/restriction.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/util/net/cidr.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

namespace address_restriction_detail {
using mongo::operator""_sd;
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
     * against any addresses, since nothing has been whitelisted.
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
     * Returns true if the Environment's client/server's address
     * satisfies this restriction set.
     */
    Status validate(const RestrictionEnvironment& environment) const override {
        auto const addr = T::addr(environment);
        if (!addr.isIP()) {
            std::ostringstream s;
            s << T::label << " is not an IP address: " << addr.getAddr();
            return {ErrorCodes::AuthenticationRestrictionUnmet, s.str()};
        }

        const CIDR address(addr.getAddr());
        for (auto const& range : _ranges) {
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
        BSONArrayBuilder b;
        for (auto const& range : _ranges) {
            b.append(range.toString());
        }
        builder->append(T::field, b.arr());
    }

    friend bool operator==(const AddressRestriction<T>& lhs, const AddressRestriction<T>& rhs) {
        return lhs.equalityLens() == rhs.equalityLens();
    }
    friend bool operator!=(const AddressRestriction<T>& lhs, const AddressRestriction<T>& rhs) {
        return !(lhs == rhs);
    }

private:
    auto equalityLens() const {
        return std::tie(_ranges);
    }

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
StatusWith<BSONArray> getRawAuthenticationRestrictions(const BSONArray& arr) noexcept;


template <>
inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<<ClientSourceRestriction>(
    ClientSourceRestriction value) {
    BSONObjBuilder b;
    value.appendToBuilder(&b);
    _builder->append(_fieldName, b.obj());
    _fieldName = StringData();
    return *_builder;
}

template <>
inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<<ServerAddressRestriction>(
    ServerAddressRestriction value) {
    BSONObjBuilder b;
    value.appendToBuilder(&b);
    _builder->append(_fieldName, b.obj());
    _fieldName = StringData();
    return *_builder;
}

}  // namespace mongo

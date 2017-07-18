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

#include <sstream>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/restriction_environment.h"

namespace mongo {

// Represents a restriction which may be attached to a user or role.
// A client authenticating as a user with a restriction, or as a user which possesses a role with a
// restriction, must be able to meet it. There are potentially many different 'types' of
// restriction. Their evaluation logic is implemented in child classes which conform to this
// interface.
template <typename S>
class Restriction {
public:
    using serialization_type = S;

    Restriction() = default;
    virtual ~Restriction() = default;

    // Returns True if the restriction is met by the RestrictionEnvironment.
    // Implemented by child classes.
    virtual Status validate(const RestrictionEnvironment& environment) const = 0;

    friend std::ostream& operator<<(std::ostream& os, const Restriction& r) {
        r.serialize(os);
        return os;
    }

    friend StringBuilder& operator<<(StringBuilder& sb, const Restriction& r) {
        return sb << r.toString();
    }

    std::string toString() const {
        std::ostringstream oss;
        oss << *this;
        return oss.str();
    }

    virtual void appendToBuilder(typename serialization_type::bson_builder_type* builder) const = 0;

private:
    // Stringifies the Restriction.
    virtual void serialize(std::ostream& os) const = 0;
};

namespace restriction_detail {
struct NamedRestrictionImpl {
    using bson_type = BSONObj;
    using bson_builder_type = BSONObjBuilder;
    static bson_type finalize(bson_builder_type* builder) {
        return builder->obj();
    }
};
struct UnnamedRestrictionImpl {
    using bson_type = BSONArray;
    using bson_builder_type = BSONArrayBuilder;
    static bson_type finalize(bson_builder_type* builder) {
        return builder->arr();
    }
};
}  // namespace restriction_detail

using NamedRestriction = Restriction<restriction_detail::NamedRestrictionImpl>;
using UnnamedRestriction = Restriction<restriction_detail::UnnamedRestrictionImpl>;

}  // namespace mongo

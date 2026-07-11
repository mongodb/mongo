// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/util/modules.h"

#include <sstream>
#include <string>

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

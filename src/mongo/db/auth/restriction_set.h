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

#include <memory>
#include <vector>

#include "mongo/db/auth/restriction.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace restriction_detail {

// Represents a set of restrictions, which may be attached to a user or role.
// This set of restrictions is met by a RestrictionEnvironment, if any restriction
// in the set is met by the RestrictionEnvironment, or if the set is empty.
template <typename T,
          template <typename...> class Pointer = std::unique_ptr,
          template <typename...> class Sequence = std::vector>
class RestrictionSetAny : public UnnamedRestriction {
public:
    using element_type = T;
    using pointer_type = Pointer<element_type>;
    using sequence_type = Sequence<pointer_type>;

    RestrictionSetAny() = default;
    explicit RestrictionSetAny(sequence_type restrictions)
        : _restrictions(std::move(restrictions)) {}

    template <typename U>
    explicit RestrictionSetAny(std::unique_ptr<U> restriction) {
        _restrictions.push_back(std::move(restriction));
    }

    Status validate(const RestrictionEnvironment& environment) const final {
        if (_restrictions.empty()) {
            return Status::OK();
        }
        for (const pointer_type& restriction : _restrictions) {
            Status status = restriction->validate(environment);
            if (status.isOK()) {
                return status;
            }
        }
        return Status(ErrorCodes::AuthenticationRestrictionUnmet,
                      str::stream() << "No member restriction in '" << *this << "' met");
    }

    void appendToBuilder(BSONArrayBuilder* builder) const final {
        builder->append(toBSON());
    }

    typename T::serialization_type::bson_type toBSON() const {
        typename T::serialization_type::bson_builder_type builder;
        for (const auto& r : _restrictions) {
            r->appendToBuilder(&builder);
        }
        return T::serialization_type::finalize(&builder);
    }

private:
    void serialize(std::ostream& os) const final {
        os << "{anyOf: [";
        for (const pointer_type& restriction : _restrictions) {
            if (restriction.get() != _restrictions.front().get()) {
                os << ", ";
            }
            os << *restriction;
        }
        os << "]}";
    }

    sequence_type _restrictions;
};

// Represents a set of restrictions which may be attached to a user or role. This set of is met by
// a RestrictionEnvironment, if each set is met by the RestrictionEnvironment.
template <typename T,
          template <typename...> class Pointer = std::unique_ptr,
          template <typename...> class Sequence = std::vector>
class RestrictionSetAll : public UnnamedRestriction {
public:
    using element_type = T;
    using pointer_type = Pointer<element_type>;
    using sequence_type = Sequence<pointer_type>;

    RestrictionSetAll() = default;
    explicit RestrictionSetAll(sequence_type restrictions)
        : _restrictions(std::move(restrictions)) {}

    template <typename U>
    explicit RestrictionSetAll(std::unique_ptr<U> restriction) {
        _restrictions.push_back(std::move(restriction));
    }

    explicit RestrictionSetAll(const T& restriction) {
        _restrictions.push_back(stdx::make_unique<T>(restriction));
    }

    Status validate(const RestrictionEnvironment& environment) const final {
        for (const pointer_type& restriction : _restrictions) {
            Status status = restriction->validate(environment);
            if (!status.isOK()) {
                return Status(ErrorCodes::AuthenticationRestrictionUnmet,
                              str::stream() << "Restriction '" << *restriction << "' in '" << *this
                                            << "' unmet");
            }
        }
        return Status::OK();
    }

    void appendToBuilder(BSONArrayBuilder* builder) const final {
        builder->append(toBSON());
    }

    typename T::serialization_type::bson_type toBSON() const {
        typename T::serialization_type::bson_builder_type builder;
        for (const auto& r : _restrictions) {
            r->appendToBuilder(&builder);
        }
        return T::serialization_type::finalize(&builder);
    }

private:
    void serialize(std::ostream& os) const final {
        os << "{allOf: [";
        for (const pointer_type& restriction : _restrictions) {
            if (restriction.get() != _restrictions.front().get()) {
                os << ", ";
            }
            os << *restriction;
        }
        os << "]}";
    }

    sequence_type _restrictions;
};
}  // namespace restriction_detail

// Users and roles may have a set of sets of restrictions. The set of set of restrictions is met if
// any of the sets are met. The sets are met if all of their restrictions are met.
// A user may have restrictions, and may have roles with restrictions. If it acquires multiple
// sets of restrictions, then the user's restrictions, and each of their roles' restrictions must
// be met.
template <template <typename...> class Pointer = std::unique_ptr,
          template <typename...> class Sequence = std::vector>
using RestrictionSet = restriction_detail::RestrictionSetAll<NamedRestriction, Pointer, Sequence>;
template <template <typename...> class Pointer = std::unique_ptr,
          template <typename...> class Sequence = std::vector>
using RestrictionDocument =
    restriction_detail::RestrictionSetAny<RestrictionSet<>, Pointer, Sequence>;
template <template <typename...> class Pointer = std::unique_ptr,
          template <typename...> class Sequence = std::vector>
using RestrictionDocumentsSequence =
    restriction_detail::RestrictionSetAll<RestrictionDocument<>, Pointer, Sequence>;

using SharedRestrictionDocument = std::shared_ptr<RestrictionDocument<>>;
using RestrictionDocuments = RestrictionDocumentsSequence<std::shared_ptr>;

}  // namespace mongo

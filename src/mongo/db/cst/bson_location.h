/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {

/**
 * Represents the location of a specific token in a BSON object.
 */
class BSONLocation {
public:
    using LocationPrefix = stdx::variant<unsigned int, StringData>;
    // A location may be either the payload of a BSONElement, or a string representing a fieldname
    // or metadata token (e.g. '{' for the start of an object). Array indices are not represented as
    // a BSONLocation, per se, but instead are part of the list of prefix descriptors.
    using LocationType = stdx::variant<BSONElement, StringData>;

    BSONLocation() = default;
    /**
     * Builds a location of a token in the input BSON. The 'prefix' argument is a list of elements
     * that describe the path to 'location'. There must be at least one element in 'prefix',
     * detailing the parser entry point.
     */
    BSONLocation(LocationType location, std::vector<LocationPrefix> prefix)
        : _location(std::move(location)), _prefix(std::move(prefix)) {}

    /**
     * Prints this location along with the prefix strings that describe the path to the element. The
     * resulting string is verbose and useful in debugging or syntax errors.
     */
    std::string toString() const {
        std::ostringstream stream;
        stdx::visit(
            OverloadedVisitor{
                [&](const BSONElement& elem) { stream << "'" << elem.toString(false) << "'"; },
                [&](const StringData& elem) { stream << "'" << elem << "'"; }},
            _location);
        // The assumption is that there is always at least one prefix that represents the entry
        // point to the parser (e.g. the 'pipeline' argument for an aggregation command).
        invariant(_prefix.size() > 0);
        for (auto it = _prefix.rbegin(); it != _prefix.rend() - 1; ++it) {
            stdx::visit(OverloadedVisitor{[&](const unsigned int& index) {
                                              stream << " within array at index " << index;
                                          },
                                          [&](const StringData& pref) {
                                              stream << " within '" << pref << "'";
                                          }},
                        *it);
        }

        // The final prefix (or first element in the vector) is the input description.
        stdx::visit(
            OverloadedVisitor{[&](const unsigned int& index) { MONGO_UNREACHABLE; },
                              [&](const StringData& pref) { stream << " of input " << pref; }},
            _prefix[0]);
        return stream.str();
    }

    friend std::ostream& operator<<(std::ostream& stream, const BSONLocation& location) {
        return stream << location.toString();
    }
    friend StringBuilder& operator<<(StringBuilder& stream, const BSONLocation& location) {
        return stream << location.toString();
    }

private:
    LocationType _location;
    std::vector<LocationPrefix> _prefix;
};

}  // namespace mongo

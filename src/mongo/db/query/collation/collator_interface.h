/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/query/collation/collation_spec.h"

namespace mongo {

class StringData;

/**
 * An interface for ordering and matching according to a collation. Instances should be retrieved
 * from the CollatorFactoryInterface and may not be copied.
 *
 * All methods are thread-safe.
 *
 * Does not throw exceptions.
 *
 * TODO SERVER-22738: Extend interface with a getComparisonKey() method and implement a
 * MongoDB-specific abstraction for a collator-generated comparison key.
 */
class CollatorInterface {
    MONGO_DISALLOW_COPYING(CollatorInterface);

public:
    /**
     * Constructs a CollatorInterface capable of computing the collation described by 'spec'.
     */
    CollatorInterface(CollationSpec spec) : _spec(std::move(spec)) {}

    virtual ~CollatorInterface() {}

    /**
     * Returns a number < 0 if 'left' is less than 'right' with respect to the collation, a number >
     * 0 if 'left' is greater than 'right' w.r.t. the collation, and 0 if 'left' and 'right' are
     * equal w.r.t. the collation.
     */
    virtual int compare(StringData left, StringData right) = 0;

    /**
     * Returns whether this collation has the same matching and sorting semantics as 'other'.
     */
    bool operator==(const CollatorInterface& other) const {
        return getSpec() == other.getSpec();
    }

    /**
     * Returns whether this collation *does not* have the same matching and sorting semantics as
     * 'other'.
     */
    bool operator!=(const CollatorInterface& other) const {
        return !(*this == other);
    }

    /**
     * Returns a reference to the CollationSpec.
     */
    const CollationSpec& getSpec() const {
        return _spec;
    }

private:
    const CollationSpec _spec;
};

}  // namespace mongo

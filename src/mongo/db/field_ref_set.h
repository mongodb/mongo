/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#pragma once

#include <set>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/field_ref.h"

namespace mongo {

    /**
     * A FieldRefSet holds a set of FieldRefs's that do not conflict with one another, that is,
     * they target different subtrees of a given document. Two fieldRef's would conflict if they
     * are equal or one is prefix of the other.
     */
    class FieldRefSet {
        MONGO_DISALLOW_COPYING(FieldRefSet);
    public:
        FieldRefSet();

        /**
         * Returns true if the field 'toInsert' can be added in the set without
         * conflicts. Otwerwise returns false and fill in '*conflict' with the field 'toInsert'
         * clashed with.
         *
         * There is no ownership transfer of 'toInsert'. The caller is responsible for
         * maintaining it alive for as long as the FieldRefSet is so. By the same token
         * 'conflict' can only be referred to while the FieldRefSet can.
         */
        bool insert(const FieldRef* toInsert, const FieldRef** conflict);

    private:
        struct FieldRefPtrLessThan {
            bool operator()(const FieldRef* lhs, const FieldRef* rhs) const;
        };
        typedef std::set<const FieldRef*, FieldRefPtrLessThan> FieldSet;

        // A set of field_ref pointers, none of which is owned here.
        FieldSet _fieldSet;
    };

} // namespace mongo

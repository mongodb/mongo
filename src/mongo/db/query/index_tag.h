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

#include "mongo/bson/util/builder.h"
#include "mongo/db/matcher/expression.h"

namespace mongo {

    // XXX
    class IndexTag : public MatchExpression::TagData {
    public:

        IndexTag(size_t i) : index(i) { }

        virtual ~IndexTag() { }

        virtual void debugString(StringBuilder* builder) const {
            *builder << " || Selected Index #" << index;
        }

        virtual MatchExpression::TagData* clone() const {
            return new IndexTag(index);
        }

        // What index should we try to use for this leaf?
        size_t index;
    };

} // namespace mongo

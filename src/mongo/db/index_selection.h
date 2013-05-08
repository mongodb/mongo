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

namespace mongo {

    class BSONObj;
    class FieldRangeSet;

    enum IndexSuitability { USELESS = 0 , HELPFUL = 1 , OPTIMAL = 2 };

    /**
     * This class is part of query optimization.  For a given index (as uniquely described by
     * keyPattern) and a given query (queryConstraints) and given sort order (order), return how
     * "suitable" the index is for the query + sort.
     *
     * USELESS indices are never used.
     * HELPFUL indices are explored and compared to other HELPFUL indices.
     * OPTIMAL indices short-circuit the search process and are always used.
     */
    class IndexSelection {
    public:
        static IndexSuitability isSuitableFor(const BSONObj& keyPattern,
                                              const FieldRangeSet& queryConstraints,
                                              const BSONObj& order);
    };

}  // namespace mongo

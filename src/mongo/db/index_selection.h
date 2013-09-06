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

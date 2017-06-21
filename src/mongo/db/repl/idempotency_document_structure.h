/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"

namespace mongo {

class DocumentStructureEnumerator {
public:
    using iterator = std::vector<BSONObj>::const_iterator;
    DocumentStructureEnumerator(std::set<StringData> fields, std::size_t depth, std::size_t length);

    iterator begin() const;
    iterator end() const;
    std::vector<BSONObj> getDocs() const;
    std::vector<BSONObj> enumerateDocs() const;
    std::vector<BSONArray> enumerateArrs() const;

private:
    static BSONArrayBuilder _getArrayBuilderFromArr(const BSONArray arr);

    static void _enumerateFixedLenArrs(const std::set<StringData>& fields,
                                       const std::size_t depthRemaining,
                                       const std::size_t length,
                                       BSONArray arr,
                                       std::vector<BSONArray>* arrs);

    static void _enumerateDocs(const std::set<StringData>& fields,
                               const std::size_t depthRemaining,
                               const std::size_t length,
                               BSONObj doc,
                               std::vector<BSONObj>* docs);

    static std::vector<BSONArray> _enumerateArrs(const std::set<StringData>& fields,
                                                 const std::size_t depth,
                                                 const std::size_t length);


    const std::set<StringData> _fields;
    const std::size_t _depth = 0;
    const std::size_t _length = 0;
    std::vector<BSONObj> _docs;
};

}  // namespace mongo

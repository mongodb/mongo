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
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/idempotency_scalar_generator.h"

namespace mongo {

class BSONArrayBuilder;
class StringData;

struct DocumentStructureEnumeratorConfig {
    DocumentStructureEnumeratorConfig(std::set<StringData> fields_,
                                      std::size_t depth_,
                                      std::size_t length_,
                                      bool skipSubDocs_ = false,
                                      bool skipSubArrs_ = false);

    std::set<StringData> fields = {};
    std::size_t depth = 0;
    std::size_t length = 0;
    const bool skipSubDocs = false;
    const bool skipSubArrs = false;
};

class DocumentStructureEnumerator {
public:
    using iterator = std::vector<BSONObj>::const_iterator;

    DocumentStructureEnumerator(DocumentStructureEnumeratorConfig config,
                                const ScalarGenerator* scalarGenerator);

    iterator begin() const;

    iterator end() const;

    std::vector<BSONObj> getDocs() const;

    std::vector<BSONObj> enumerateDocs() const;

    std::vector<BSONArray> enumerateArrs() const;

private:
    static BSONArrayBuilder _getArrayBuilderFromArr(BSONArray arr);

    static void _enumerateFixedLenArrs(const DocumentStructureEnumeratorConfig& config,
                                       const ScalarGenerator* scalarGenerator,
                                       BSONArray arr,
                                       std::vector<BSONArray>* arrs);

    static void _enumerateDocs(const DocumentStructureEnumeratorConfig& config,
                               const ScalarGenerator* scalarGenerator,
                               BSONObj doc,
                               std::vector<BSONObj>* docs);

    static std::vector<BSONArray> _enumerateArrs(const DocumentStructureEnumeratorConfig& config,
                                                 const ScalarGenerator* scalarGenerator);


    const DocumentStructureEnumeratorConfig _config;
    const ScalarGenerator* _scalarGenerator;
    std::vector<BSONObj> _docs;
};

}  // namespace mongo

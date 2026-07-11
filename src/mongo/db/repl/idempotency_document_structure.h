// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/repl/idempotency_scalar_generator.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <set>
#include <string_view>
#include <vector>

namespace mongo {

class BSONArrayBuilder;

struct DocumentStructureEnumeratorConfig {
    DocumentStructureEnumeratorConfig(std::set<std::string_view> fields_,
                                      std::size_t depth_,
                                      std::size_t length_,
                                      bool skipSubDocs_ = false,
                                      bool skipSubArrs_ = false);

    std::set<std::string_view> fields = {};
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

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type_system_test_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/time_support.h"

#include <span>
#include <string>

#include <absl/strings/str_split.h>

namespace mongo::pipeline::type_system {

using namespace std::literals::string_view_literals;

std::vector<Value> scalarRepresentatives() {
    const OID oid("0123456789abcdef01234567");
    return {
        Value(MINKEY),
        Value(),
        Value(1.0),  // BSONType::numberDouble
        Value("str"sv),
        Value(BSONBinData("", 0, BinDataGeneral)),
        Value(BSONUndefined),
        Value(oid),
        Value(true),
        Value(Date_t()),
        Value(BSONNULL),
        Value(BSONRegEx("a")),
        Value(BSONDBRef("db.coll", oid)),
        Value(BSONCode("f()")),
        Value(BSONSymbol("sym")),
        Value(BSONCodeWScope("f()", BSON("a" << 1))),
        Value(1),  // BSONType::numberInt
        Value(Timestamp(1, 1)),
        Value(1LL),            // BSONType::numberLong
        Value(Decimal128(1)),  // BSONType::numberDecimal
        Value(MAXKEY),
    };
}

namespace {
/**
 * Returns arrays in which each element of 'elements' is nested within an array or two arrays.
 * Example: 1 maps to [1], [null, 1] and [[1]].
 */
std::vector<Value> nestInArrays(const std::vector<Value>& elements) {
    std::vector<Value> arrays;
    for (const auto& element : elements) {
        arrays.emplace_back(std::vector<Value>{element});
        arrays.emplace_back(std::vector<Value>{Value(BSONNULL), element});
        arrays.emplace_back(std::vector<Value>{Value(std::vector<Value>{element})});
    }
    return arrays;
}

/// Returns the scalars a corpus places along the path.
std::vector<Value> corpusScalars(ScalarCorpus scalars) {
    if (scalars == ScalarCorpus::kMatchTraversal) {
        return {Value(), Value(BSONNULL), Value(1), Value(1.0)};
    }
    return scalarRepresentatives();
}

/// Returns {name: 1, name2: 1}, an object that also holds a field unrelated to 'name'.
BSONObj withUnrelatedField(std::string_view name) {
    return BSON(name << 1 << std::string(name) + "2" << 1);
}

/**
 * Returns the values the first component of 'path' takes: every scalar of 'scalars', an object
 * holding the rest of the path, and arrays of those.
 */
std::vector<Value> generateValues(std::span<const std::string_view> path, ScalarCorpus scalars) {
    auto values = corpusScalars(scalars);
    if (path.size() == 1) {
        values.emplace_back(BSONObj());
    } else {
        for (const auto& value : generateValues(path.subspan(1), scalars)) {
            values.emplace_back(BSON(path[1] << value));
        }
        values.emplace_back(withUnrelatedField(path[1]));
    }
    for (auto& array : nestInArrays(values)) {
        values.push_back(std::move(array));
    }
    return values;
}

void generateDocuments(std::string_view path, ScalarCorpus scalars, BSONObjBinarySet& documents) {
    const std::vector<std::string_view> components =
        absl::StrSplit(path, absl::ByChar('.'), absl::AllowEmpty());
    for (const auto& value : generateValues(components, scalars)) {
        documents.insert(BSON(components.front() << value));
    }
    documents.insert(withUnrelatedField(components.front()));
}
}  // namespace

BSONObjBinarySet generateDocumentCorpus(std::string_view path, ScalarCorpus scalars) {
    BSONObjBinarySet documents;
    generateDocuments(path, scalars, documents);
    return documents;
}

BSONObjBinarySet generateCombinedCorpus(const std::vector<std::string>& paths,
                                        ScalarCorpus scalars) {
    BSONObjBinarySet documents;
    for (auto&& path : paths) {
        generateDocuments(path, scalars, documents);
    }
    return documents;
}

}  // namespace mongo::pipeline::type_system

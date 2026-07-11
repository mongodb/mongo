// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace query_benchmark_constants {
extern const BSONObj kMockMetadataWrapper;
extern const BSONElement kMockClientMetadataElem;

enum class QueryComplexity : int { kIDHack = 0, kMildlyComplex, kMkComplex, kVeryComplex };
extern BSONObj queryComplexityToJSON(const QueryComplexity& complexity);

extern const BSONObj kIDHackPredicate;
extern const BSONObj kMildlyComplexPredicate;

extern const BSONObj kComplexPredicate;
extern const BSONObj kComplexProjection;

extern const BSONObj kChangeStreamPredicate;
extern const BSONObj kVeryComplexProjection;

struct UpdateSpec {
    BSONObj u;
    boost::optional<BSONObj> c;
    boost::optional<BSONObj> arrayFilters;
};

enum class PipelineComplexity : int {
    kSimple = 0,
    kWithConstants,
    kWithMultipleStages,
    kWithMultipleStagesAndExpressions,
};

enum class ModifierUpdateComplexity : int {
    kSimple = 0,
    kMildlyComplex,
    kComplex,
    kVeryComplex,
};

extern UpdateSpec getUpdateSpec(const PipelineComplexity& complexity);

const UpdateSpec& getUpdateSpec(const ModifierUpdateComplexity& complexity,
                                bool useArrayFilters = false);

extern const UpdateSpec kReplacementUpdate;
extern const UpdateSpec kPipelineUpdateSimple;
extern const UpdateSpec kPipelineUpdateWithConstants;
extern const UpdateSpec kPipelineUpdateWithMultipleStages;
extern const UpdateSpec kPipelineUpdateWithMultipleStagesAndExpressions;

struct DeleteSpec {
    std::vector<BSONObj> deletes;
    boost::optional<BSONObj> let;
};

// Complexity of a delete predicate that uses $expr with let variables.
enum class LetDeleteComplexity : int {
    kSimple = 0,
    kComplex,
};

const DeleteSpec& getDeleteWithLetSpec(const LetDeleteComplexity& complexity);

// A set of delete specs representing a realistic bulk-delete command with multiple operations.
extern const DeleteSpec kMultiOpDeleteSpec;

constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

}  // namespace query_benchmark_constants
}  // namespace mongo

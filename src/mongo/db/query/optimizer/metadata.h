/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/partial_schema_requirements.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"


namespace mongo::optimizer {

/**
 * Describes how documents are distributed among separate threads or machines.
 */
struct DistributionAndPaths {
    DistributionAndPaths(DistributionType type);
    DistributionAndPaths(DistributionType type, ABTVector paths);

    /**
     * Each distribution type assigns documents to partitions in a particular pattern.
     *
     * For example, RangePartitioning tries to keep similar values together,
     * while HashPartitioning tries to separate similar values.
     */
    DistributionType _type;

    /**
     * The paths identify the parts of a document that determine its partition.
     *
     * For example, in RangePartitioning or HashPartitioning, if two documents agree on
     * these paths, then they always end up in the same partition.
     *
     * Some distribution types, such as Centralized or Replicated, don't need to
     * look at any part of a document to decide its partition, so they don't
     * need any paths.
     */
    ABTVector _paths;
};

/**
 * Structure to represent index field component and its associated collation. The _path field
 * contains the path to the field component, restricted to Get, Traverse, and Id elements.
 * For example, if we have an index on {a.b, c} that contains arrays, the _path for the first entry
 * would be Get "a" Traverse Get "b" Traverse Id, and the _path for the second entry would be
 * Get "c" Traverse Id.
 * Implicitly contains multikey info through Traverse element or lack of Traverse element.
 */
struct IndexCollationEntry {
    IndexCollationEntry(ABT path, CollationOp op);

    bool operator==(const IndexCollationEntry& other) const = default;

    ABT _path;
    CollationOp _op;
};

// Full collation specification, using a list of component entries.
using IndexCollationSpec = std::vector<IndexCollationEntry>;

/**
 * Represents a set of paths that are known to be 'non-multikey'--which in this context
 * is defined as 'do not apply their child path to an array'.
 *
 * For example, in this document: {a: [ {b: 3}, {b: 4} ]}
 * - 'a' is multikey
 * - 'a.b' is non-multikey
 *
 * We say 'a.b' is non-multikey, because even though 'Get [a] Traverse [1] Get [b] p'
 * applies 'p' to two different values (3 and 4), neither one is an array.
 * Therefore if 'p' starts with Traverse (of any maxDepth), we can remove it.
 * If 'p' starts with more than one Traverse, we can apply the rule repeatedly.
 *
 * This also implies that 'Get [a] Get [b] p' is non-multikey.
 * (Because: if Get [a] produces an array, then Get [b] applies 'p' to Nothing.
 *  In other words: replacing Traverse Get with Get can only make a path be Nothing
 *  in more cases.)
 *
 * However, this doesn't tell us anything about 'Get [a] Traverse [inf] Get [b] p',
 * where the intermediate Traverse has maxDepth > 1. For example, consider this document:
 *     {a: [ {b: 5}, [ {b: [6, 7]} ] ]}
 * We'd still say 'a.b' is non-multikey, because 'Get [a] Traverse [1] Get [b] p' doesn't
 * reach into the nested array, and doesn't find [6, 7].
 * But 'Get [a] Traverse [inf] Get [b] p' does reach into the nested array, so it does
 * apply 'p' to [6, 7], so we can't remove Traverse nodes from 'p'.
 */
struct MultikeynessTrie {
    static MultikeynessTrie fromIndexPath(const ABT& path);

    void merge(const MultikeynessTrie& other);
    void add(const ABT& path);

    std::map<FieldNameType, MultikeynessTrie> children;
    // An empty trie doesn't know whether anything is multikey.
    // 'true' means "not sure" while 'false' means "definitely no arrays".
    bool isMultiKey = true;
};

/**
 * Metadata associated with an index. Holds the index specification (index fields and their
 * collations), its version (0 or 1), the collations as a bit mask, multikeyness info, and
 * distribution info. This is a convenient structure for the query planning process.
 */
class IndexDefinition {
public:
    // For testing.
    IndexDefinition(IndexCollationSpec collationSpec, bool isMultiKey);

    IndexDefinition(IndexCollationSpec collationSpec,
                    bool isMultiKey,
                    DistributionAndPaths distributionAndPaths,
                    PSRExpr::Node partialReqMap);

    IndexDefinition(IndexCollationSpec collationSpec,
                    int64_t version,
                    uint32_t orderingBits,
                    bool isMultiKey,
                    DistributionAndPaths distributionAndPaths,
                    PSRExpr::Node partialReqMap);

    const IndexCollationSpec& getCollationSpec() const;

    int64_t getVersion() const;
    uint32_t getOrdering() const;
    bool isMultiKey() const;

    const DistributionAndPaths& getDistributionAndPaths() const;

    const PSRExpr::Node& getPartialReqMap() const;
    PSRExpr::Node& getPartialReqMap();

private:
    const IndexCollationSpec _collationSpec;

    const int64_t _version;
    const uint32_t _orderingBits;
    const bool _isMultiKey;

    const DistributionAndPaths _distributionAndPaths;

    // Requirements map for partial filter expression. May be trivially true.
    PSRExpr::Node _partialReqMap;
};

using IndexDefinitions = opt::unordered_map<std::string, IndexDefinition>;
using ScanDefOptions = opt::unordered_map<std::string, std::string>;

/**
 * Metadata associated with the sharding state of a collection.
 */
class ShardingMetadata {
public:
    ShardingMetadata();

    ShardingMetadata(IndexCollationSpec shardKey, bool mayContainOrphans);

    const IndexCollationSpec& shardKey() const {
        return _shardKey;
    }

    bool mayContainOrphans() const {
        return _mayContainOrphans;
    }

    void setMayContainOrphans(bool val) {
        _mayContainOrphans = val;
    }

    const std::vector<FieldNameType>& topLevelShardKeyFieldNames() const {
        return _topLevelShardKeyFieldNames;
    }

private:
    // Shard key of the collection. This is stored as an IndexCollectionSpec because the shard key
    // is conceptually an index to the shard which contains a particular key. The only collation ops
    // that are allowed are Ascending and Clustered.
    // Note: Clustered collation op is intended to represent a hashed shard key; however, if two
    // keys hash to the same value, it is possible that an index scan of the hashed index will
    // produce a stream of keys which are not clustered. Hashed indexes are implemented with a
    // B-tree using the hashed value as a key, which makes it sensitive to insertion order.
    IndexCollationSpec _shardKey;

    // Whether the collection may contain orphans.
    bool _mayContainOrphans{false};

    // Top level field name of each component of the shard key.
    std::vector<FieldNameType> _topLevelShardKeyFieldNames;
};

/**
 * Parameters to a scan node, including distribution information, associated index definitions,
 * and multikeyness information. Also includes any ScanDefOptions we might have, such as which
 * database the collection is associated with, the origin of the collection (mongod or a BSON file),
 * or the UUID of the collection.
 */
class ScanDefinition {
public:
    ScanDefinition();

    ScanDefinition(DatabaseName dbName,
                   boost::optional<UUID> uuid,
                   ScanDefOptions options,
                   IndexDefinitions indexDefs,
                   MultikeynessTrie multikeynessTrie,
                   DistributionAndPaths distributionAndPaths,
                   bool exists,
                   boost::optional<CEType> ce,
                   ShardingMetadata shardingMetadata);

    const ScanDefOptions& getOptionsMap() const;

    const DistributionAndPaths& getDistributionAndPaths() const;

    const DatabaseName& getDatabaseName() const;

    const boost::optional<UUID>& getUUID() const;

    const opt::unordered_map<std::string, IndexDefinition>& getIndexDefs() const;
    opt::unordered_map<std::string, IndexDefinition>& getIndexDefs();

    const MultikeynessTrie& getMultikeynessTrie() const;

    bool exists() const;

    const boost::optional<CEType>& getCE() const;

    const ShardingMetadata& shardingMetadata() const;
    ShardingMetadata& shardingMetadata();

    const NamespaceStringOrUUID& getNamespaceStringOrUUID() const;

private:
    ScanDefOptions _options;
    DistributionAndPaths _distributionAndPaths;
    DatabaseName _dbName;
    boost::optional<UUID> _uuid;

    /**
     * Indexes associated with this collection.
     */
    opt::unordered_map<std::string, IndexDefinition> _indexDefs;

    MultikeynessTrie _multikeynessTrie;

    /**
     * True if the collection exists.
     */
    bool _exists;

    // If positive, estimated number of docs in the collection.
    boost::optional<CEType> _ce;

    ShardingMetadata _shardingMetadata;
};

/**
 * Represents the optimizer’s view of the state of the rest of the system in terms of relevant
 * resources. Currently we store the set of available collections in the system. In the future,
 * when we support distributed planning, this is where we will put information related to the
 * physical organization and topology of the machines.
 * For each collection, we hold distribution information (fields it may be sharded on), multikeyness
 * info, and data related to associated indexes in addition to other relevant metadata.
 */
struct Metadata {
    Metadata() = default;
    Metadata(opt::unordered_map<std::string, ScanDefinition> scanDefs);
    Metadata(opt::unordered_map<std::string, ScanDefinition> scanDefs, size_t numberOfPartitions);

    opt::unordered_map<std::string, ScanDefinition> _scanDefs;

    // Degree of parallelism.
    size_t _numberOfPartitions{1};

    bool isParallelExecution() const;

    // TODO: generalize cluster spec.
};

}  // namespace mongo::optimizer

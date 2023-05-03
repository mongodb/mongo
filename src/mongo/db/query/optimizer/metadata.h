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

#include <map>

#include "mongo/db/query/optimizer/partial_schema_requirements.h"


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


struct IndexCollationEntry {
    IndexCollationEntry(ABT path, CollationOp op);

    bool operator==(const IndexCollationEntry& other) const;

    ABT _path;
    CollationOp _op;
};

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

    opt::unordered_map<FieldNameType, MultikeynessTrie, FieldNameType::Hasher> children;
    // An empty trie doesn't know whether anything is multikey.
    // 'true' means "not sure" while 'false' means "definitely no arrays".
    bool isMultiKey = true;
};

/**
 * Defines an available system index.
 */
class IndexDefinition {
public:
    // For testing.
    IndexDefinition(IndexCollationSpec collationSpec, bool isMultiKey);

    IndexDefinition(IndexCollationSpec collationSpec,
                    bool isMultiKey,
                    DistributionAndPaths distributionAndPaths,
                    PartialSchemaRequirements partialReqMap);

    IndexDefinition(IndexCollationSpec collationSpec,
                    int64_t version,
                    uint32_t orderingBits,
                    bool isMultiKey,
                    DistributionAndPaths distributionAndPaths,
                    PartialSchemaRequirements partialReqMap);

    const IndexCollationSpec& getCollationSpec() const;

    int64_t getVersion() const;
    uint32_t getOrdering() const;
    bool isMultiKey() const;

    const DistributionAndPaths& getDistributionAndPaths() const;

    const PartialSchemaRequirements& getPartialReqMap() const;
    PartialSchemaRequirements& getPartialReqMap();

private:
    const IndexCollationSpec _collationSpec;

    const int64_t _version;
    const uint32_t _orderingBits;
    const bool _isMultiKey;

    const DistributionAndPaths _distributionAndPaths;

    // Requirements map for partial filter expression. May be trivially true.
    PartialSchemaRequirements _partialReqMap;
};

using IndexDefinitions = opt::unordered_map<std::string, IndexDefinition>;
using ScanDefOptions = opt::unordered_map<std::string, std::string>;

// Used to specify parameters to scan node, such as collection name, or file where collection is
// read from.
class ScanDefinition {
public:
    ScanDefinition();

    ScanDefinition(ScanDefOptions options,
                   IndexDefinitions indexDefs,
                   MultikeynessTrie multikeynessTrie,
                   DistributionAndPaths distributionAndPaths,
                   bool exists,
                   CEType ce);

    const ScanDefOptions& getOptionsMap() const;

    const DistributionAndPaths& getDistributionAndPaths() const;

    const opt::unordered_map<std::string, IndexDefinition>& getIndexDefs() const;
    opt::unordered_map<std::string, IndexDefinition>& getIndexDefs();

    const MultikeynessTrie& getMultikeynessTrie() const;

    bool exists() const;

    CEType getCE() const;

private:
    ScanDefOptions _options;
    DistributionAndPaths _distributionAndPaths;

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
    CEType _ce;
};

struct Metadata {
    Metadata(opt::unordered_map<std::string, ScanDefinition> scanDefs);
    Metadata(opt::unordered_map<std::string, ScanDefinition> scanDefs, size_t numberOfPartitions);

    opt::unordered_map<std::string, ScanDefinition> _scanDefs;

    // Degree of parallelism.
    size_t _numberOfPartitions;

    bool isParallelExecution() const;

    // TODO: generalize cluster spec.
};

}  // namespace mongo::optimizer

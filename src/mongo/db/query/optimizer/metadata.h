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

#include "mongo/db/query/optimizer/index_bounds.h"


namespace mongo::optimizer {

struct DistributionAndPaths {
    DistributionAndPaths(DistributionType type);
    DistributionAndPaths(DistributionType type, ABTVector paths);

    DistributionType _type;
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

    // Requirements map for partial filter expression.
    PartialSchemaRequirements _partialReqMap;
};

using ScanDefOptions = opt::unordered_map<std::string, std::string>;

// Used to specify parameters to scan node, such as collection name, or file where collection is
// read from.
class ScanDefinition {
public:
    ScanDefinition();

    ScanDefinition(ScanDefOptions options,
                   opt::unordered_map<std::string, IndexDefinition> indexDefs,
                   IndexPathSet nonMultiKeyPathSet,
                   DistributionAndPaths distributionAndPaths,
                   bool exists,
                   CEType ce);

    const ScanDefOptions& getOptionsMap() const;

    const DistributionAndPaths& getDistributionAndPaths() const;

    const opt::unordered_map<std::string, IndexDefinition>& getIndexDefs() const;
    opt::unordered_map<std::string, IndexDefinition>& getIndexDefs();

    const IndexPathSet& getNonMultiKeyPathSet() const;

    bool exists() const;

    CEType getCE() const;

private:
    ScanDefOptions _options;
    DistributionAndPaths _distributionAndPaths;

    /**
     * Indexes associated with this collection.
     */
    opt::unordered_map<std::string, IndexDefinition> _indexDefs;

    /**
     * Which index paths are NOT multi-key.
     */
    IndexPathSet _nonMultiKeyPathSet;

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

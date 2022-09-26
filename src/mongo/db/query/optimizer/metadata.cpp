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

#include "mongo/db/query/optimizer/metadata.h"

#include "mongo/db/query/optimizer/node.h"


namespace mongo::optimizer {

DistributionAndPaths::DistributionAndPaths(DistributionType type)
    : DistributionAndPaths(type, {}) {}

DistributionAndPaths::DistributionAndPaths(DistributionType type, ABTVector paths)
    : _type(type), _paths(std::move(paths)) {
    uassert(6624080,
            "Invalid distribution type",
            _paths.empty() || _type == DistributionType::HashPartitioning ||
                _type == DistributionType::RangePartitioning);
}

class MultikeynessTrieTransport {
public:
    MultikeynessTrie transport(const PathIdentity& node) {
        return {
            {} /* children */,
            false /* isMultiKey */,
        };
    }

    MultikeynessTrie transport(const PathGet& node, MultikeynessTrie child) {
        return {
            {{node.name(), std::move(child)}},
            false /* isMultiKey */,
        };
    }

    MultikeynessTrie transport(const PathTraverse& node, MultikeynessTrie child) {
        tassert(6859601,
                "Traverse in index spec has unexpected maxDepth",
                node.getMaxDepth() == PathTraverse::kSingleLevel);
        child.isMultiKey = true;
        return child;
    }

    template <class N, class... Ts>
    MultikeynessTrie transport(const N&, Ts&&...) {
        tasserted(6859602, "Unexpected Path node in index spec");
    }
};

MultikeynessTrie MultikeynessTrie::fromIndexPath(const ABT& path) {
    MultikeynessTrieTransport instance;
    return algebra::transport<false>(path, instance);
}

void MultikeynessTrie::merge(const MultikeynessTrie& other) {
    // 'isMultikey == true' means "not sure", while 'false' means "definitely no arrays".
    // So 'false' should win as we accumulate facts from separate tries.
    isMultiKey &= other.isMultiKey;

    for (auto&& [key, child] : other.children) {
        children[key].merge(child);
    }
}

void MultikeynessTrie::add(const ABT& path) {
    merge(MultikeynessTrie::fromIndexPath(path));
}

bool IndexCollationEntry::operator==(const IndexCollationEntry& other) const {
    return _path == other._path && _op == other._op;
}

IndexCollationEntry::IndexCollationEntry(ABT path, CollationOp op)
    : _path(std::move(path)), _op(op) {}

IndexDefinition::IndexDefinition(IndexCollationSpec collationSpec, bool isMultiKey)
    : IndexDefinition(std::move(collationSpec), isMultiKey, {DistributionType::Centralized}, {}) {}

IndexDefinition::IndexDefinition(IndexCollationSpec collationSpec,
                                 bool isMultiKey,
                                 DistributionAndPaths distributionAndPaths,
                                 PartialSchemaRequirements partialReqMap)
    : IndexDefinition(std::move(collationSpec),
                      2 /*version*/,
                      0 /*orderingBits*/,
                      isMultiKey,
                      std::move(distributionAndPaths),
                      std::move(partialReqMap)) {}

IndexDefinition::IndexDefinition(IndexCollationSpec collationSpec,
                                 int64_t version,
                                 uint32_t orderingBits,
                                 bool isMultiKey,
                                 DistributionAndPaths distributionAndPaths,
                                 PartialSchemaRequirements partialReqMap)
    : _collationSpec(std::move(collationSpec)),
      _version(version),
      _orderingBits(orderingBits),
      _isMultiKey(isMultiKey),
      _distributionAndPaths(distributionAndPaths),
      _partialReqMap(std::move(partialReqMap)) {}

const IndexCollationSpec& IndexDefinition::getCollationSpec() const {
    return _collationSpec;
}

int64_t IndexDefinition::getVersion() const {
    return _version;
}

uint32_t IndexDefinition::getOrdering() const {
    return _orderingBits;
}

bool IndexDefinition::isMultiKey() const {
    return _isMultiKey;
}

const DistributionAndPaths& IndexDefinition::getDistributionAndPaths() const {
    return _distributionAndPaths;
}

const PartialSchemaRequirements& IndexDefinition::getPartialReqMap() const {
    return _partialReqMap;
}

PartialSchemaRequirements& IndexDefinition::getPartialReqMap() {
    return _partialReqMap;
}

ScanDefinition::ScanDefinition()
    : ScanDefinition({} /*options*/,
                     {} /*indexDefs*/,
                     {} /*nonMultiKeyPathSet*/,
                     {DistributionType::Centralized},
                     true /*exists*/,
                     -1.0 /*ce*/) {}

ScanDefinition::ScanDefinition(ScanDefOptions options,
                               opt::unordered_map<std::string, IndexDefinition> indexDefs,
                               MultikeynessTrie multikeynessTrie,
                               DistributionAndPaths distributionAndPaths,
                               const bool exists,
                               const CEType ce)
    : _options(std::move(options)),
      _distributionAndPaths(std::move(distributionAndPaths)),
      _indexDefs(std::move(indexDefs)),
      _multikeynessTrie(std::move(multikeynessTrie)),
      _exists(exists),
      _ce(ce) {}

const ScanDefOptions& ScanDefinition::getOptionsMap() const {
    return _options;
}

const DistributionAndPaths& ScanDefinition::getDistributionAndPaths() const {
    return _distributionAndPaths;
}

const opt::unordered_map<std::string, IndexDefinition>& ScanDefinition::getIndexDefs() const {
    return _indexDefs;
}

opt::unordered_map<std::string, IndexDefinition>& ScanDefinition::getIndexDefs() {
    return _indexDefs;
}

const MultikeynessTrie& ScanDefinition::getMultikeynessTrie() const {
    return _multikeynessTrie;
}

bool ScanDefinition::exists() const {
    return _exists;
}

CEType ScanDefinition::getCE() const {
    return _ce;
}

Metadata::Metadata(opt::unordered_map<std::string, ScanDefinition> scanDefs)
    : Metadata(std::move(scanDefs), 1 /*numberOfPartitions*/) {}

Metadata::Metadata(opt::unordered_map<std::string, ScanDefinition> scanDefs,
                   size_t numberOfPartitions)
    : _scanDefs(std::move(scanDefs)), _numberOfPartitions(numberOfPartitions) {}

bool Metadata::isParallelExecution() const {
    return _numberOfPartitions > 1;
}

}  // namespace mongo::optimizer

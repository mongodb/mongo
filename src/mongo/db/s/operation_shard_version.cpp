/*
*    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/operation_shard_version.h"

#include "mongo/bson/util/bson_extract.h"

namespace mongo {

namespace {

const OperationContext::Decoration<OperationShardVersion> shardingMetadataDecoration =
    OperationContext::declareDecoration<OperationShardVersion>();

const char* kShardVersionField = "shardVersion";
const ChunkVersion kUnshardedVersion(ChunkVersion::UNSHARDED());

}  // namespace mongo

OperationShardVersion::OperationShardVersion() = default;

OperationShardVersion& OperationShardVersion::get(OperationContext* txn) {
    return shardingMetadataDecoration(txn);
}

void OperationShardVersion::initializeFromCommand(NamespaceString ns, const BSONObj& cmdObj) {
    if (ns.isSystemDotIndexes()) {
        setShardVersion(std::move(ns), ChunkVersion::IGNORED());
        return;
    }

    BSONElement versionElt;
    Status status = bsonExtractTypedField(cmdObj, kShardVersionField, BSONType::Array, &versionElt);
    if (!status.isOK()) {
        return;
    }

    const BSONArray versionArr(versionElt.Obj());
    bool hasVersion = false;
    ChunkVersion newVersion = ChunkVersion::fromBSON(versionArr, &hasVersion);

    if (!hasVersion) {
        return;
    }

    setShardVersion(std::move(ns), std::move(newVersion));
}

bool OperationShardVersion::hasShardVersion() const {
    return _hasVersion;
}

const ChunkVersion& OperationShardVersion::getShardVersion(const NamespaceString& ns) const {
    if (_ns != ns) {
        return kUnshardedVersion;
    }

    return _shardVersion;
}

void OperationShardVersion::setShardVersion(NamespaceString ns, ChunkVersion newVersion) {
    // This currently supports only setting the shard version for one namespace.
    invariant(!_hasVersion || _ns == ns);
    invariant(!ns.isSystemDotIndexes() || ChunkVersion::isIgnoredVersion(newVersion));

    _ns = std::move(ns);
    _shardVersion = std::move(newVersion);
    _hasVersion = true;
}

void OperationShardVersion::_clear() {
    _hasVersion = false;
    _shardVersion.clear();
    _ns = NamespaceString();
}

OperationShardVersion::IgnoreVersioningBlock::IgnoreVersioningBlock(OperationContext* txn,
                                                                    const NamespaceString& ns)
    : _txn(txn), _ns(ns) {
    auto& operationVersion = OperationShardVersion::get(txn);
    _hadOriginalVersion = operationVersion._hasVersion;
    if (_hadOriginalVersion) {
        _originalVersion = operationVersion.getShardVersion(ns);
    }
    operationVersion.setShardVersion(ns, ChunkVersion::IGNORED());
}

OperationShardVersion::IgnoreVersioningBlock::~IgnoreVersioningBlock() {
    auto& operationVersion = OperationShardVersion::get(_txn);
    invariant(ChunkVersion::isIgnoredVersion(operationVersion.getShardVersion(_ns)));
    if (_hadOriginalVersion) {
        operationVersion.setShardVersion(_ns, _originalVersion);
    } else {
        operationVersion._clear();
    }
}

}  // namespace mongo

/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/write_command_ref.h"

namespace mongo {

bool BatchWriteCommandRefImpl::getBypassDocumentValidation() const {
    return getRequest().getBypassDocumentValidation();
}

const OptionalBool& BatchWriteCommandRefImpl::getBypassEmptyTsReplacement() const {
    return getRequest().getBypassEmptyTsReplacement();
}

const boost::optional<IDLAnyTypeOwned>& BatchWriteCommandRefImpl::getComment() const {
    return getRequest().getGenericArguments().getComment();
}

boost::optional<bool> BatchWriteCommandRefImpl::getErrorsOnly() const {
    return boost::none;
}

const boost::optional<LegacyRuntimeConstants>& BatchWriteCommandRefImpl::getLegacyRuntimeConstants()
    const {
    return getRequest().getLegacyRuntimeConstants();
}

const boost::optional<BSONObj>& BatchWriteCommandRefImpl::getLet() const {
    return getRequest().getLet();
}

boost::optional<std::int64_t> BatchWriteCommandRefImpl::getMaxTimeMS() const {
    return getRequest().getGenericArguments().getMaxTimeMS();
}

std::set<NamespaceString> BatchWriteCommandRefImpl::getNssSet() const {
    std::set<NamespaceString> nssSet;
    nssSet.insert(getRequest().getNS());
    return nssSet;
}

bool BatchWriteCommandRefImpl::getOrdered() const {
    return getRequest().getOrdered();
}

boost::optional<std::int32_t> BatchWriteCommandRefImpl::getStmtId() const {
    return getRequest().getWriteCommandRequestBase().getStmtId();
}

boost::optional<std::vector<std::int32_t>> BatchWriteCommandRefImpl::getStmtIds() const {
    return getRequest().getWriteCommandRequestBase().getStmtIds();
}

bool BulkWriteCommandRefImpl::getBypassDocumentValidation() const {
    return getRequest().getBypassDocumentValidation();
}

const OptionalBool& BulkWriteCommandRefImpl::getBypassEmptyTsReplacement() const {
    return getRequest().getBypassEmptyTsReplacement();
}

const boost::optional<IDLAnyTypeOwned>& BulkWriteCommandRefImpl::getComment() const {
    return getRequest().getGenericArguments().getComment();
}

boost::optional<bool> BulkWriteCommandRefImpl::getErrorsOnly() const {
    return getRequest().getErrorsOnly();
}

const boost::optional<LegacyRuntimeConstants>& BulkWriteCommandRefImpl::getLegacyRuntimeConstants()
    const {
    static const boost::optional<LegacyRuntimeConstants> kMissingLegacyRuntimeConstants;
    return kMissingLegacyRuntimeConstants;
}

const boost::optional<BSONObj>& BulkWriteCommandRefImpl::getLet() const {
    return getRequest().getLet();
}

boost::optional<std::int64_t> BulkWriteCommandRefImpl::getMaxTimeMS() const {
    return getRequest().getGenericArguments().getMaxTimeMS();
}

std::set<NamespaceString> BulkWriteCommandRefImpl::getNssSet() const {
    std::set<NamespaceString> nssSet;
    for (const auto& nsInfo : getRequest().getNsInfo()) {
        nssSet.insert(nsInfo.getNs());
    }
    return nssSet;
}

bool BulkWriteCommandRefImpl::getOrdered() const {
    return getRequest().getOrdered();
}

boost::optional<std::int32_t> BulkWriteCommandRefImpl::getStmtId() const {
    return getRequest().getStmtId();
}

boost::optional<std::vector<std::int32_t>> BulkWriteCommandRefImpl::getStmtIds() const {
    return getRequest().getStmtIds();
}

int BatchWriteCommandRefImpl::estimateOpSizeInBytes(int index) const {
    return visitOpData(
        index,
        OverloadedVisitor{
            [&](const BSONObj& insertDoc) { return insertDoc.objsize(); },
            [&](const write_ops::UpdateOpEntry& update) {
                auto estSize = write_ops::getUpdateSizeEstimate(
                    update.getQ(),
                    update.getU(),
                    update.getC(),
                    update.getUpsertSupplied().has_value(),
                    update.getCollation(),
                    update.getArrayFilters(),
                    update.getSort(),
                    update.getHint(),
                    update.getSampleId(),
                    update.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery().has_value());
                // Verify that estSize is at least the BSON serialization size for debug builds.
                dassert(estSize >= update.toBSON().objsize());
                return estSize;
            },
            [&](const write_ops::DeleteOpEntry& del) {
                auto estSize = write_ops::getDeleteSizeEstimate(
                    del.getQ(), del.getCollation(), del.getHint(), del.getSampleId());
                // Verify that estSize is at least the BSON serialization size for debug builds.
                dassert(estSize >= del.toBSON().objsize());
                return estSize;
            }});
}

const boost::optional<std::vector<BSONObj>>& BatchWriteCommandRefImpl::getArrayFilters(
    int index) const {
    using RetT = const boost::optional<std::vector<BSONObj>>&;
    return visitUpdateOpData(index, [&](const write_ops::UpdateOpEntry& updateOp) -> RetT {
        return updateOp.getArrayFilters();
    });
}

const boost::optional<BSONObj>& BatchWriteCommandRefImpl::getCollation(int index) const {
    using RetT = const boost::optional<BSONObj>&;
    static const boost::optional<BSONObj> kMissingBSONObj;

    return visitOpData(
        index,
        OverloadedVisitor{[&](const BSONObj& insertDoc) -> RetT { return kMissingBSONObj; },
                          [&](const write_ops::UpdateOpEntry& updateOp) -> RetT {
                              return updateOp.getCollation();
                          },
                          [&](const write_ops::DeleteOpEntry& deleteOp) -> RetT {
                              return deleteOp.getCollation();
                          }});
}

boost::optional<BSONObj> BatchWriteCommandRefImpl::getConstants(int index) const {
    using RetT = const boost::optional<BSONObj>&;
    return visitUpdateOpData(
        index, [&](const write_ops::UpdateOpEntry& updateOp) -> RetT { return updateOp.getC(); });
}

const BSONObj& BatchWriteCommandRefImpl::getFilter(int index) const {
    using RetT = const BSONObj&;
    return visitUpdateOrDeleteOpData(
        index,
        OverloadedVisitor{
            [&](const write_ops::UpdateOpEntry& updateOp) -> RetT { return updateOp.getQ(); },
            [&](const write_ops::DeleteOpEntry& deleteOp) -> RetT {
                return deleteOp.getQ();
            }});
}

const BSONObj& BatchWriteCommandRefImpl::getDocument(int index) const {
    using RetT = const BSONObj&;
    return visitInsertOpData(index, [&](const BSONObj& insertDoc) -> RetT { return insertDoc; });
}

bool BatchWriteCommandRefImpl::getMulti(int index) const {
    return visitOpData(index,
                       OverloadedVisitor{[&](const BSONObj& insertDoc) { return false; },
                                         [&](const write_ops::UpdateOpEntry& updateOp) {
                                             return updateOp.getMulti();
                                         },
                                         [&](const write_ops::DeleteOpEntry& deleteOp) {
                                             return deleteOp.getMulti();
                                         }});
}

const NamespaceString& BatchWriteCommandRefImpl::getNss(int index) const {
    return getRequest().getNS();
}

boost::optional<UUID> BatchWriteCommandRefImpl::getCollectionUUID(int index) const {
    return getRequest().getCollectionUUID();
}

BatchedCommandRequest::BatchType BatchWriteCommandRefImpl::getOpType(int index) const {
    return getRequest().getBatchType();
}

const write_ops::UpdateModification& BatchWriteCommandRefImpl::getUpdateMods(int index) const {
    using RetT = const write_ops::UpdateModification&;
    return visitUpdateOpData(
        index, [&](const write_ops::UpdateOpEntry& updateOp) -> RetT { return updateOp.getU(); });
}

bool BatchWriteCommandRefImpl::getUpsert(int index) const {
    return visitOpData(index,
                       OverloadedVisitor{[&](const BSONObj& insertDoc) { return false; },
                                         [&](const write_ops::UpdateOpEntry& updateOp) {
                                             return updateOp.getUpsert();
                                         },
                                         [&](const write_ops::DeleteOpEntry& deleteOp) {
                                             return false;
                                         }});
}

const boost::optional<mongo::EncryptionInformation>&
BatchWriteCommandRefImpl::getEncryptionInformation(int index) const {
    return getRequest().getWriteCommandRequestBase().getEncryptionInformation();
}

BSONObj BatchWriteCommandRefImpl::toBSON(int index) const {
    return visitOpData(index,
                       OverloadedVisitor{[&](const BSONObj& insertDoc) { return insertDoc; },
                                         [&](const write_ops::UpdateOpEntry& updateOp) {
                                             return updateOp.toBSON();
                                         },
                                         [&](const write_ops::DeleteOpEntry& deleteOp) {
                                             return deleteOp.toBSON();
                                         }});
}

int BulkWriteCommandRefImpl::estimateOpSizeInBytes(int index) const {
    return visitOpData(
        index,
        OverloadedVisitor{[&](const mongo::BulkWriteInsertOp& insertOp) {
                              auto estSize =
                                  write_ops::getBulkWriteInsertSizeEstimate(insertOp.getDocument());
                              // Verify that estSize is at least the BSON serialization
                              // size for debug builds.
                              dassert(estSize >= insertOp.toBSON().objsize());
                              return estSize;
                          },
                          [&](const mongo::BulkWriteUpdateOp& updateOp) {
                              auto estSize = write_ops::getBulkWriteUpdateSizeEstimate(
                                  updateOp.getFilter(),
                                  updateOp.getUpdateMods(),
                                  updateOp.getConstants(),
                                  updateOp.getUpsertSupplied().has_value(),
                                  updateOp.getCollation(),
                                  updateOp.getArrayFilters(),
                                  updateOp.getSort(),
                                  updateOp.getHint(),
                                  updateOp.getSampleId());
                              // Verify that estSize is at least the BSON serialization
                              // size for debug builds.
                              dassert(estSize >= updateOp.toBSON().objsize());
                              return estSize;
                          },
                          [&](const mongo::BulkWriteDeleteOp& deleteOp) {
                              auto estSize =
                                  write_ops::getBulkWriteDeleteSizeEstimate(deleteOp.getFilter(),
                                                                            deleteOp.getCollation(),
                                                                            deleteOp.getHint(),
                                                                            deleteOp.getSampleId());
                              // Verify that estSize is at least the BSON serialization
                              // size for debug builds.
                              dassert(estSize >= deleteOp.toBSON().objsize());
                              return estSize;
                          }});
}

const boost::optional<std::vector<BSONObj>>& BulkWriteCommandRefImpl::getArrayFilters(
    int index) const {
    using RetT = const boost::optional<std::vector<BSONObj>>&;
    return visitUpdateOpData(index, [&](const BulkWriteUpdateOp& updateOp) -> RetT {
        return updateOp.getArrayFilters();
    });
}

const boost::optional<BSONObj>& BulkWriteCommandRefImpl::getCollation(int index) const {
    using RetT = const boost::optional<BSONObj>&;
    static const boost::optional<BSONObj> kMissingBSONObj;

    return visitOpData(
        index,
        OverloadedVisitor{
            [&](const BulkWriteInsertOp& insertOp) -> RetT { return kMissingBSONObj; },
            [&](const BulkWriteUpdateOp& updateOp) -> RetT { return updateOp.getCollation(); },
            [&](const BulkWriteDeleteOp& deleteOp) -> RetT {
                return deleteOp.getCollation();
            }});
}

boost::optional<BSONObj> BulkWriteCommandRefImpl::getConstants(int index) const {
    using RetT = const boost::optional<BSONObj>&;
    return visitUpdateOpData(
        index, [&](const BulkWriteUpdateOp& updateOp) -> RetT { return updateOp.getConstants(); });
}

const BSONObj& BulkWriteCommandRefImpl::getFilter(int index) const {
    using RetT = const BSONObj&;
    return visitUpdateOrDeleteOpData(
        index,
        OverloadedVisitor{
            [&](const BulkWriteUpdateOp& updateOp) -> RetT { return updateOp.getFilter(); },
            [&](const BulkWriteDeleteOp& deleteOp) -> RetT {
                return deleteOp.getFilter();
            }});
}

const BSONObj& BulkWriteCommandRefImpl::getDocument(int index) const {
    using RetT = const BSONObj&;
    return visitInsertOpData(
        index, [&](const BulkWriteInsertOp& insertOp) -> RetT { return insertOp.getDocument(); });
}

bool BulkWriteCommandRefImpl::getMulti(int index) const {
    return visitOpData(
        index,
        OverloadedVisitor{[&](const BulkWriteInsertOp& insertOp) { return false; },
                          [&](const BulkWriteUpdateOp& updateOp) { return updateOp.getMulti(); },
                          [&](const BulkWriteDeleteOp& deleteOp) {
                              return deleteOp.getMulti();
                          }});
}

const NamespaceString& BulkWriteCommandRefImpl::getNss(int index) const {
    auto nsInfoIdx = visitOpData(index, [](const auto& op) { return op.getNsInfoIdx(); });
    return getRequest().getNsInfo()[nsInfoIdx].getNs();
}

boost::optional<UUID> BulkWriteCommandRefImpl::getCollectionUUID(int index) const {
    auto nsInfoIdx = visitOpData(index, [](const auto& op) { return op.getNsInfoIdx(); });
    return getRequest().getNsInfo()[nsInfoIdx].getCollectionUUID();
}

BatchedCommandRequest::BatchType BulkWriteCommandRefImpl::getOpType(int index) const {
    return visitOpData(
        index,
        OverloadedVisitor{
            [&](const BulkWriteInsertOp&) { return BatchedCommandRequest::BatchType_Insert; },
            [&](const BulkWriteUpdateOp&) { return BatchedCommandRequest::BatchType_Update; },
            [&](const BulkWriteDeleteOp&) {
                return BatchedCommandRequest::BatchType_Delete;
            }});
}

const write_ops::UpdateModification& BulkWriteCommandRefImpl::getUpdateMods(int index) const {
    using RetT = const write_ops::UpdateModification&;
    return visitUpdateOpData(
        index, [&](const BulkWriteUpdateOp& updateOp) -> RetT { return updateOp.getUpdateMods(); });
}

bool BulkWriteCommandRefImpl::getUpsert(int index) const {
    return visitOpData(
        index,
        OverloadedVisitor{[&](const BulkWriteInsertOp& insertOp) { return false; },
                          [&](const BulkWriteUpdateOp& updateOp) { return updateOp.getUpsert(); },
                          [&](const BulkWriteDeleteOp& deleteOp) {
                              return false;
                          }});
}

const boost::optional<mongo::EncryptionInformation>&
BulkWriteCommandRefImpl::getEncryptionInformation(int index) const {
    auto nsInfoIdx = visitOpData(index, [](const auto& op) { return op.getNsInfoIdx(); });
    return getRequest().getNsInfo()[nsInfoIdx].getEncryptionInformation();
}

BSONObj BulkWriteCommandRefImpl::toBSON(int index) const {
    return visitOpData(index, [&](const auto& op) { return op.toBSON(); });
}

int FindAndModifyCommandRefImpl::estimateOpSizeInBytes(int index) const {
    return 0;
}

bool FindAndModifyCommandRefImpl::getBypassDocumentValidation() const {
    return getRequest().getBypassDocumentValidation().value_or(false);
}

const OptionalBool& FindAndModifyCommandRefImpl::getBypassEmptyTsReplacement() const {
    return getRequest().getBypassEmptyTsReplacement();
}

const boost::optional<IDLAnyTypeOwned>& FindAndModifyCommandRefImpl::getComment() const {
    return getRequest().getGenericArguments().getComment();
}

boost::optional<bool> FindAndModifyCommandRefImpl::getErrorsOnly() const {
    return false;
}

const boost::optional<LegacyRuntimeConstants>&
FindAndModifyCommandRefImpl::getLegacyRuntimeConstants() const {
    static const boost::optional<LegacyRuntimeConstants> kMissingLegacyRuntimeConstants;
    return kMissingLegacyRuntimeConstants;
}

const boost::optional<BSONObj>& FindAndModifyCommandRefImpl::getLet() const {
    return getRequest().getLet();
}

boost::optional<std::int64_t> FindAndModifyCommandRefImpl::getMaxTimeMS() const {
    return getRequest().getGenericArguments().getMaxTimeMS();
}

std::set<NamespaceString> FindAndModifyCommandRefImpl::getNssSet() const {
    std::set<NamespaceString> nssSet;
    nssSet.insert(getRequest().getNamespace());
    return nssSet;
}

bool FindAndModifyCommandRefImpl::getOrdered() const {
    return true;
}

boost::optional<std::int32_t> FindAndModifyCommandRefImpl::getStmtId() const {
    return getRequest().getStmtId();
}

boost::optional<std::vector<std::int32_t>> FindAndModifyCommandRefImpl::getStmtIds() const {
    return getRequest().getStmtId().map([](auto stmtId) { return std::vector<int32_t>{stmtId}; });
}

const boost::optional<std::vector<BSONObj>>& FindAndModifyCommandRefImpl::getArrayFilters(
    int index) const {
    return getRequest().getArrayFilters();
}

const boost::optional<BSONObj>& FindAndModifyCommandRefImpl::getCollation(int index) const {
    return getRequest().getCollation();
}

boost::optional<BSONObj> FindAndModifyCommandRefImpl::getConstants(int index) const {
    return boost::none;
}

const BSONObj& FindAndModifyCommandRefImpl::getFilter(int index) const {
    return getRequest().getQuery();
}

const BSONObj& FindAndModifyCommandRefImpl::getDocument(int index) const {
    return BSONObj::kEmptyObject;
}

bool FindAndModifyCommandRefImpl::getMulti(int index) const {
    return false;
}

const NamespaceString& FindAndModifyCommandRefImpl::getNss(int index) const {
    return getRequest().getNamespace();
}

boost::optional<UUID> FindAndModifyCommandRefImpl::getCollectionUUID(int index) const {
    return boost::none;
}

BatchedCommandRequest::BatchType FindAndModifyCommandRefImpl::getOpType(int index) const {
    if (getRequest().getUpdate()) {
        return BatchedCommandRequest::BatchType_Update;
    } else {
        tassert(10394910,
                "Expect 'remove' to be true for a findAndModify command",
                getRequest().getRemove());
        return BatchedCommandRequest::BatchType_Delete;
    }
}

const write_ops::UpdateModification& FindAndModifyCommandRefImpl::getUpdateMods(int index) const {
    static write_ops::UpdateModification emptyUpdateMod;
    if (!getRequest().getUpdate()) {
        return emptyUpdateMod;
    }
    return *getRequest().getUpdate();
}

bool FindAndModifyCommandRefImpl::getUpsert(int index) const {
    return getRequest().getUpsert().value_or(false);
}

const boost::optional<mongo::EncryptionInformation>&
FindAndModifyCommandRefImpl::getEncryptionInformation(int index) const {
    return getRequest().getEncryptionInformation();
}

BSONObj FindAndModifyCommandRefImpl::toBSON(int index) const {
    return getRequest().toBSON();
}
}  // namespace mongo

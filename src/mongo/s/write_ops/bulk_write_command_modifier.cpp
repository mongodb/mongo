/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/variables.h"
#include "mongo/s/write_ops/bulk_write_command_modifier.h"

#include "mongo/bson/bsonobj.h"

namespace mongo {

void BulkWriteCommandModifier::parseRequestFromOpMsg(const NamespaceString& nss,
                                                     const OpMsgRequest& request) {
    auto shardVersionField = request.body[ShardVersion::kShardVersionField];
    if (!shardVersionField.eoo()) {
        auto shardVersion = ShardVersion::parse(shardVersionField);
        if (shardVersion == ShardVersion::UNSHARDED()) {
            setDbVersion(nss, DatabaseVersion(request.body));
        }
        setShardVersion(nss, shardVersion);
    }

    // The 'isTimeseriesNamespace' is an internal parameter used for communication between mongos
    // and mongod.
    auto isTimeseriesNamespace =
        request.body[write_ops::WriteCommandRequestBase::kIsTimeseriesNamespaceFieldName];
    uassert(7299100,
            "the 'isTimeseriesNamespace' parameter cannot be used on mongos",
            !isTimeseriesNamespace.trueValue());

    setIsTimeseriesNamespace(nss, isTimeseriesNamespace.trueValue());
}

std::tuple<NamespaceInfoEntry&, size_t> BulkWriteCommandModifier::getNsInfoEntry(
    const NamespaceString& nss) {
    if (_nsInfoIdxes.contains(nss)) {
        // Already have this NamespaceInfoEntry stored.
        auto idx = _nsInfoIdxes[nss];
        return std::tie(_nsInfos[idx], idx);
    }
    // Create new NamespaceInfoEntry.
    auto nsInfoEntry = NamespaceInfoEntry(nss);
    auto idx = _nsInfos.size();
    _nsInfos.emplace_back(nsInfoEntry);

    _nsInfoIdxes[nss] = idx;
    return std::tie(_nsInfos[idx], idx);
}

void BulkWriteCommandModifier::finishBuild() {
    _request->setOps(std::move(_ops));
    _request->setNsInfo(std::move(_nsInfos));
}

void BulkWriteCommandModifier::addOp(write_ops::InsertCommandRequest insertOp) {
    auto nss = insertOp.getNamespace();
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
    nsInfoEntry.setEncryptionInformation(insertOp.getEncryptionInformation());

    for (const auto& doc : insertOp.getDocuments()) {
        auto op = BulkWriteInsertOp(idx, doc);
        _ops.emplace_back(op);
    }
}

void BulkWriteCommandModifier::addOp(write_ops::UpdateCommandRequest updateOp) {
    auto nss = updateOp.getNamespace();
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
    nsInfoEntry.setEncryptionInformation(updateOp.getEncryptionInformation());

    for (const auto& update : updateOp.getUpdates()) {
        auto op = BulkWriteUpdateOp(idx, update.getQ(), update.getU());

        op.setArrayFilters(update.getArrayFilters());
        op.setMulti(update.getMulti());
        op.setCollation(update.getCollation());
        op.setUpsert(update.getUpsert());
        op.setHint(update.getHint());
        op.setConstants(update.getC());

        _ops.emplace_back(op);
    }
}

void BulkWriteCommandModifier::addOp(write_ops::DeleteCommandRequest deleteOp) {
    auto nss = deleteOp.getNamespace();
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);
    nsInfoEntry.setEncryptionInformation(deleteOp.getEncryptionInformation());

    for (const auto& delOp : deleteOp.getDeletes()) {
        auto op = BulkWriteDeleteOp(idx, delOp.getQ());

        op.setHint(delOp.getHint());
        op.setMulti(delOp.getMulti());
        op.setCollation(delOp.getCollation());

        _ops.emplace_back(op);
    }
}

void BulkWriteCommandModifier::addInsert(const OpMsgRequest& request) {
    auto parsedInsertOp = InsertOp::parse(request);

    auto nss = parsedInsertOp.getNamespace();

    parseRequestFromOpMsg(nss, request);

    addOp(parsedInsertOp);
}

void BulkWriteCommandModifier::addUpdate(const OpMsgRequest& request) {
    auto parsedUpdateOp = UpdateOp::parse(request);

    auto nss = parsedUpdateOp.getNamespace();

    parseRequestFromOpMsg(nss, request);

    addOp(parsedUpdateOp);
}

void BulkWriteCommandModifier::addDelete(const OpMsgRequest& request) {
    auto parsedDeleteOp = DeleteOp::parse(request);

    auto nss = parsedDeleteOp.getNamespace();

    parseRequestFromOpMsg(nss, request);

    addOp(parsedDeleteOp);
}

void BulkWriteCommandModifier::addInsertOps(const NamespaceString& nss,
                                            const std::vector<BSONObj> docs) {
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);

    for (const auto& doc : docs) {
        auto op = BulkWriteInsertOp(idx, doc);

        _ops.emplace_back(op);
    }
}

void BulkWriteCommandModifier::addUpdateOp(
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& update,
    bool upsert,
    bool multi,
    const StringData& returnField,
    const boost::optional<std::vector<BSONObj>>& arrayFilters,
    const boost::optional<BSONObj>& collation,
    const boost::optional<BSONObj>& sort,
    const boost::optional<BSONObj>& returnFields,
    const boost::optional<BSONObj>& hint) {
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);

    auto op = BulkWriteUpdateOp(idx, query, update);

    op.setUpsert(upsert);
    op.setMulti(multi);
    op.setReturn(returnField);
    op.setReturnFields(returnFields);
    op.setCollation(collation);
    op.setHint(hint.value_or(BSONObj()));
    op.setArrayFilters(arrayFilters);
    op.setSort(sort);

    _ops.emplace_back(op);
}

void BulkWriteCommandModifier::addPipelineUpdateOps(const NamespaceString& nss,
                                                    const BSONObj& query,
                                                    const std::vector<BSONObj>& updates,
                                                    bool upsert,
                                                    bool useMultiUpdate) {
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);

    auto updateMod = write_ops::UpdateModification();
    auto op = BulkWriteUpdateOp(idx, query, updates);

    op.setUpsert(upsert);
    op.setMulti(useMultiUpdate);

    _ops.emplace_back(op);
}

void BulkWriteCommandModifier::addDeleteOp(const NamespaceString& nss,
                                           const BSONObj& query,
                                           bool multiDelete,
                                           bool returnField,
                                           const boost::optional<BSONObj>& collation,
                                           const boost::optional<BSONObj>& sort,
                                           const boost::optional<BSONObj>& returnFields,
                                           const boost::optional<BSONObj>& hint) {
    auto [nsInfoEntry, idx] = getNsInfoEntry(nss);

    auto op = BulkWriteDeleteOp(idx, query);

    op.setMulti(multiDelete);
    op.setReturn(returnField);
    op.setReturnFields(returnFields);
    op.setHint(hint.value_or(BSONObj()));
    op.setSort(sort);
    op.setCollation(collation);

    _ops.emplace_back(op);
}

}  // namespace mongo

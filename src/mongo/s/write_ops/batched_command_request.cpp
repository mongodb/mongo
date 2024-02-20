/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/write_ops/batched_command_request.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo {
namespace {

const auto kWriteConcern = "writeConcern"_sd;

template <class T>
BatchedCommandRequest constructBatchedCommandRequest(const OpMsgRequest& request) {
    auto batchRequest = BatchedCommandRequest{T::parse(request)};

    auto shardVersionField = request.body[ShardVersion::kShardVersionField];
    if (!shardVersionField.eoo()) {
        auto shardVersion = ShardVersion::parse(shardVersionField);
        if (shardVersion == ShardVersion::UNSHARDED()) {
            batchRequest.setDbVersion(DatabaseVersion(request.body));
        }
        batchRequest.setShardVersion(shardVersion);
    }

    auto writeConcernField = request.body[kWriteConcern];
    if (!writeConcernField.eoo()) {
        batchRequest.setWriteConcern(writeConcernField.Obj());
    }

    // The 'isTimeseriesNamespace' is an internal parameter used for communication between mongos
    // and mongod.
    auto isTimeseriesNamespace =
        request.body[write_ops::WriteCommandRequestBase::kIsTimeseriesNamespaceFieldName];
    uassert(5916401,
            "the 'isTimeseriesNamespace' parameter cannot be used on mongos",
            !isTimeseriesNamespace.trueValue());

    return batchRequest;
}

// Utility that parses and evaluates 'let'. It returns the result as a serialized object.
BSONObj freezeLet(OperationContext* opCtx,
                  const mongo::BSONObj& let,
                  const boost::optional<mongo::LegacyRuntimeConstants>& legacyRuntimeConstants,
                  const NamespaceString& nss) {
    // Evaluate the let parameters.
    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    nullptr /* collator */,
                                                    nss,
                                                    legacyRuntimeConstants,
                                                    let,
                                                    false,  // disk use is banned on mongos
                                                    false,  // mongos has no profile collection
                                                    boost::none /* verbosity */);
    expCtx->variables.seedVariablesWithLetParameters(expCtx.get(), let);
    return expCtx->variables.toBSON(expCtx->variablesParseState, let);
}

}  // namespace

const boost::optional<LegacyRuntimeConstants> BatchedCommandRequest::kEmptyRuntimeConstants =
    boost::optional<LegacyRuntimeConstants>{};
const boost::optional<BSONObj> BatchedCommandRequest::kEmptyLet = boost::optional<BSONObj>{};

BatchedCommandRequest BatchedCommandRequest::parseInsert(const OpMsgRequest& request) {
    return constructBatchedCommandRequest<InsertOp>(request);
}

BatchedCommandRequest BatchedCommandRequest::parseUpdate(const OpMsgRequest& request) {
    return constructBatchedCommandRequest<UpdateOp>(request);
}

BatchedCommandRequest BatchedCommandRequest::parseDelete(const OpMsgRequest& request) {
    return constructBatchedCommandRequest<DeleteOp>(request);
}

bool BatchedCommandRequest::getBypassDocumentValidation() const {
    return _visit([](auto&& op) -> decltype(auto) { return op.getBypassDocumentValidation(); });
}

const NamespaceString& BatchedCommandRequest::getNS() const {
    return _visit([](auto&& op) -> decltype(auto) { return op.getNamespace(); });
}

bool BatchedCommandRequest::hasEncryptionInformation() const {
    return _visit(
        [](auto&& op) -> decltype(auto) { return op.getEncryptionInformation().has_value(); });
}

std::size_t BatchedCommandRequest::sizeWriteOps() const {
    struct Visitor {
        auto operator()(const write_ops::InsertCommandRequest& op) const {
            return op.getDocuments().size();
        }
        auto operator()(const write_ops::UpdateCommandRequest& op) const {
            return op.getUpdates().size();
        }
        auto operator()(const write_ops::DeleteCommandRequest& op) const {
            return op.getDeletes().size();
        }
    };
    return _visit(Visitor{});
}

bool BatchedCommandRequest::hasLegacyRuntimeConstants() const {
    return _visit(OverloadedVisitor{[](write_ops::InsertCommandRequest&) { return false; },
                                    [&](write_ops::UpdateCommandRequest& op) {
                                        return op.getLegacyRuntimeConstants().has_value();
                                    },
                                    [&](write_ops::DeleteCommandRequest& op) {
                                        return op.getLegacyRuntimeConstants().has_value();
                                    }});
}

void BatchedCommandRequest::setLegacyRuntimeConstants(LegacyRuntimeConstants runtimeConstants) {
    _visit(OverloadedVisitor{[](write_ops::InsertCommandRequest&) {},
                             [&](write_ops::UpdateCommandRequest& op) {
                                 op.setLegacyRuntimeConstants(std::move(runtimeConstants));
                             },
                             [&](write_ops::DeleteCommandRequest& op) {
                                 op.setLegacyRuntimeConstants(std::move(runtimeConstants));
                             }});
}

void BatchedCommandRequest::unsetLegacyRuntimeConstants() {
    _visit(OverloadedVisitor{
        [](write_ops::InsertCommandRequest&) {},
        [&](write_ops::UpdateCommandRequest& op) { op.setLegacyRuntimeConstants(boost::none); },
        [&](write_ops::DeleteCommandRequest& op) {
            op.setLegacyRuntimeConstants(boost::none);
        }});
}

const boost::optional<LegacyRuntimeConstants>& BatchedCommandRequest::getLegacyRuntimeConstants()
    const {
    struct Visitor {
        auto& operator()(const write_ops::InsertCommandRequest& op) const {
            return kEmptyRuntimeConstants;
        }
        auto& operator()(const write_ops::UpdateCommandRequest& op) const {
            return op.getLegacyRuntimeConstants();
        }
        auto& operator()(const write_ops::DeleteCommandRequest& op) const {
            return op.getLegacyRuntimeConstants();
        }
    };
    return _visit(Visitor{});
};

const boost::optional<BSONObj>& BatchedCommandRequest::getLet() const {
    struct Visitor {
        auto& operator()(const write_ops::InsertCommandRequest& op) const {
            return kEmptyLet;
        }
        auto& operator()(const write_ops::UpdateCommandRequest& op) const {
            return op.getLet();
        }
        auto& operator()(const write_ops::DeleteCommandRequest& op) const {
            return op.getLet();
        }
    };
    return _visit(Visitor{});
};

void BatchedCommandRequest::evaluateAndReplaceLetParams(OperationContext* opCtx) {
    switch (_batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            break;
        case BatchedCommandRequest::BatchType_Update:
            if (auto let = _updateReq->getLet()) {
                _updateReq->setLet(
                    freezeLet(opCtx, *let, _updateReq->getLegacyRuntimeConstants(), getNS()));
            }
            break;
        case BatchedCommandRequest::BatchType_Delete:
            if (auto let = _deleteReq->getLet()) {
                _deleteReq->setLet(
                    freezeLet(opCtx, *let, _deleteReq->getLegacyRuntimeConstants(), getNS()));
            }
            break;
    }
}

const write_ops::WriteCommandRequestBase& BatchedCommandRequest::getWriteCommandRequestBase()
    const {
    return _visit([](auto&& op) -> decltype(auto) { return op.getWriteCommandRequestBase(); });
}

void BatchedCommandRequest::setWriteCommandRequestBase(
    write_ops::WriteCommandRequestBase writeCommandBase) {
    return _visit([&](auto&& op) { op.setWriteCommandRequestBase(std::move(writeCommandBase)); });
}

void BatchedCommandRequest::serialize(BSONObjBuilder* builder) const {
    _visit([&](auto&& op) { op.serialize({}, builder); });
    if (_shardVersion) {
        _shardVersion->serialize(ShardVersion::kShardVersionField, builder);
    }

    if (_dbVersion) {
        builder->append("databaseVersion", _dbVersion->toBSON());
    }

    if (_writeConcern) {
        builder->append(kWriteConcern, *_writeConcern);
    }
}

BSONObj BatchedCommandRequest::toBSON() const {
    BSONObjBuilder builder;
    serialize(&builder);

    return builder.obj();
}

std::string BatchedCommandRequest::toString() const {
    return toBSON().toString();
}

BatchedCommandRequest BatchedCommandRequest::cloneInsertWithIds(
    BatchedCommandRequest origCmdRequest) {
    invariant(origCmdRequest.getBatchType() == BatchedCommandRequest::BatchType_Insert);

    BatchedCommandRequest newCmdRequest(std::move(origCmdRequest));

    const auto& origDocs = newCmdRequest._insertReq->getDocuments();

    std::vector<BSONObj> newDocs;

    for (const auto& doc : origDocs) {
        if (doc["_id"].eoo()) {
            newDocs.emplace_back([&] {
                BSONObjBuilder idInsertB;
                idInsertB.append("_id", OID::gen());
                idInsertB.appendElements(doc);
                return idInsertB.obj();
            }());
        } else {
            newDocs.emplace_back(doc);
        }
    }

    newCmdRequest._insertReq->setDocuments(std::move(newDocs));

    return newCmdRequest;
}

BatchedCommandRequest BatchedCommandRequest::buildDeleteOp(const NamespaceString& nss,
                                                           const BSONObj& query,
                                                           bool multiDelete,
                                                           const boost::optional<BSONObj>& hint) {
    return BatchedCommandRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(multiDelete);

            if (hint) {
                entry.setHint(hint.value());
            }
            return entry;
        }()});
        return deleteOp;
    }());
}

BatchedCommandRequest BatchedCommandRequest::buildInsertOp(const NamespaceString& nss,
                                                           const std::vector<BSONObj> docs) {
    return BatchedCommandRequest([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments(docs);
        return insertOp;
    }());
}

BatchedCommandRequest BatchedCommandRequest::buildUpdateOp(const NamespaceString& nss,
                                                           const BSONObj& query,
                                                           const BSONObj& update,
                                                           bool upsert,
                                                           bool multi,
                                                           const boost::optional<BSONObj>& hint) {
    return BatchedCommandRequest([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(upsert);
            entry.setMulti(multi);
            if (hint) {
                entry.setHint(hint.value());
            }
            return entry;
        }()});
        return updateOp;
    }());
}

BatchedCommandRequest BatchedCommandRequest::buildPipelineUpdateOp(
    const NamespaceString& nss,
    const BSONObj& query,
    const std::vector<BSONObj>& updates,
    bool upsert,
    bool useMultiUpdate) {
    return BatchedCommandRequest([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification(updates));
            entry.setUpsert(upsert);
            entry.setMulti(useMultiUpdate);
            return entry;
        }()});
        return updateOp;
    }());
}

BatchItemRef::BatchItemRef(const BatchedCommandRequest* request, int index)
    : _batchedRequest(*request), _index(index), _batchType(_batchedRequest->getBatchType()) {
    invariant(index < int(request->sizeWriteOps()));
}

BatchItemRef::BatchItemRef(const BulkWriteCommandRequest* request, int index)
    : _bulkWriteRequest(*request), _index(index) {
    invariant(index < int(request->getOps().size()));
    _batchType = convertOpType(BulkWriteCRUDOp(request->getOps()[index]).getType());
}

int BatchItemRef::getSizeForBatchWriteBytes() const {
    tassert(7328113, "Invalid BatchedCommandRequest reference", _batchedRequest);

    switch (_batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            return getDocument().objsize();

        case BatchedCommandRequest::BatchType_Update: {
            auto& update = _batchedRequest->getUpdateRequest().getUpdates()[_index];
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
            // When running a debug build, verify that estSize is at least the BSON serialization
            // size.
            dassert(estSize >= update.toBSON().objsize());
            return estSize;
        }

        case BatchedCommandRequest::BatchType_Delete: {
            auto& deleteOp = _batchedRequest->getDeleteRequest().getDeletes()[_index];
            auto estSize = write_ops::getDeleteSizeEstimate(deleteOp.getQ(),
                                                            deleteOp.getCollation(),
                                                            deleteOp.getHint(),
                                                            deleteOp.getSampleId());
            // When running a debug build, verify that estSize is at least the BSON serialization
            // size.
            dassert(estSize >= deleteOp.toBSON().objsize());
            return estSize;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

int BatchItemRef::getSizeForBulkWriteBytes() const {
    tassert(7353600, "Invalid BulkWriteCommandRequest reference", _bulkWriteRequest);

    switch (_batchType) {
        case BatchedCommandRequest::BatchType_Insert: {
            auto& insertOp = *BulkWriteCRUDOp(_bulkWriteRequest->getOps()[_index]).getInsert();
            auto estSize = write_ops::getBulkWriteInsertSizeEstimate(insertOp.getDocument());
            // When running a debug build, verify that estSize is at least the BSON serialization
            // size.
            dassert(estSize >= insertOp.toBSON().objsize());
            return estSize;
        }

        case BatchedCommandRequest::BatchType_Update: {
            auto& updateOp = *BulkWriteCRUDOp(_bulkWriteRequest->getOps()[_index]).getUpdate();
            auto estSize =
                write_ops::getBulkWriteUpdateSizeEstimate(updateOp.getFilter(),
                                                          updateOp.getUpdateMods(),
                                                          updateOp.getConstants(),
                                                          updateOp.getUpsertSupplied().has_value(),
                                                          updateOp.getCollation(),
                                                          updateOp.getArrayFilters(),
                                                          updateOp.getSort(),
                                                          updateOp.getHint(),
                                                          updateOp.getSampleId());
            // When running a debug build, verify that estSize is at least the BSON serialization
            // size.
            dassert(estSize >= updateOp.toBSON().objsize());
            return estSize;
        }
        case BatchedCommandRequest::BatchType_Delete: {
            auto& deleteOp = *BulkWriteCRUDOp(_bulkWriteRequest->getOps()[_index]).getDelete();
            auto estSize = write_ops::getBulkWriteDeleteSizeEstimate(deleteOp.getFilter(),
                                                                     deleteOp.getCollation(),
                                                                     deleteOp.getHint(),
                                                                     deleteOp.getSampleId());
            // When running a debug build, verify that estSize is at least the BSON serialization
            // size.
            dassert(estSize >= deleteOp.toBSON().objsize());
            return estSize;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

BatchedCommandRequest::BatchType convertOpType(BulkWriteCRUDOp::OpType opType) {
    switch (opType) {
        case BulkWriteCRUDOp::OpType::kInsert:
            return BatchedCommandRequest::BatchType_Insert;
        case BulkWriteCRUDOp::OpType::kUpdate:
            return BatchedCommandRequest::BatchType_Update;
        case BulkWriteCRUDOp::OpType::kDelete:
            return BatchedCommandRequest::BatchType_Delete;
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace mongo

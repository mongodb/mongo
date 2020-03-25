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

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batched_command_request.h"

#include "mongo/bson/bsonobj.h"

namespace mongo {
namespace {

const auto kWriteConcern = "writeConcern"_sd;
const auto kAllowImplicitCollectionCreation = "allowImplicitCollectionCreation"_sd;

template <class T>
BatchedCommandRequest constructBatchedCommandRequest(const OpMsgRequest& request) {
    auto batchRequest = BatchedCommandRequest{T::parse(request)};

    auto chunkVersion = ChunkVersion::parseFromCommand(request.body);
    if (chunkVersion != ErrorCodes::NoSuchKey) {
        batchRequest.setShardVersion(uassertStatusOK(std::move(chunkVersion)));
        if (chunkVersion == ChunkVersion::UNSHARDED()) {
            auto dbVersion = DatabaseVersion::parse(IDLParserErrorContext("BatchedCommandRequest"),
                                                    request.body);
            batchRequest.setDbVersion(std::move(dbVersion));
        }
    }

    auto writeConcernField = request.body[kWriteConcern];
    if (!writeConcernField.eoo()) {
        batchRequest.setWriteConcern(writeConcernField.Obj());
    }

    if (auto allowImplicitElement = request.body[kAllowImplicitCollectionCreation]) {
        batchRequest.setAllowImplicitCreate(allowImplicitElement.boolean());
    }

    return batchRequest;
}

}  // namespace

BatchedCommandRequest BatchedCommandRequest::parseInsert(const OpMsgRequest& request) {
    return constructBatchedCommandRequest<InsertOp>(request);
}

BatchedCommandRequest BatchedCommandRequest::parseUpdate(const OpMsgRequest& request) {
    return constructBatchedCommandRequest<UpdateOp>(request);
}

BatchedCommandRequest BatchedCommandRequest::parseDelete(const OpMsgRequest& request) {
    return constructBatchedCommandRequest<DeleteOp>(request);
}

const NamespaceString& BatchedCommandRequest::getNS() const {
    return _visit([](auto&& op) -> decltype(auto) { return op.getNamespace(); });
}

std::size_t BatchedCommandRequest::sizeWriteOps() const {
    struct Visitor {
        auto operator()(const write_ops::Insert& op) const {
            return op.getDocuments().size();
        }
        auto operator()(const write_ops::Update& op) const {
            return op.getUpdates().size();
        }
        auto operator()(const write_ops::Delete& op) const {
            return op.getDeletes().size();
        }
    };
    return _visit(Visitor{});
}

bool BatchedCommandRequest::isVerboseWC() const {
    if (!hasWriteConcern()) {
        return true;
    }

    BSONObj writeConcern = getWriteConcern();
    BSONElement wElem = writeConcern["w"];
    if (!wElem.isNumber() || wElem.Number() != 0) {
        return true;
    }

    return false;
}

const write_ops::WriteCommandBase& BatchedCommandRequest::getWriteCommandBase() const {
    return _visit([](auto&& op) -> decltype(auto) { return op.getWriteCommandBase(); });
}

void BatchedCommandRequest::setWriteCommandBase(write_ops::WriteCommandBase writeCommandBase) {
    return _visit([&](auto&& op) { op.setWriteCommandBase(std::move(writeCommandBase)); });
}

void BatchedCommandRequest::serialize(BSONObjBuilder* builder) const {
    _visit([&](auto&& op) { op.serialize({}, builder); });
    if (_shardVersion) {
        _shardVersion->appendToCommand(builder);
    }

    if (_dbVersion) {
        builder->append("databaseVersion", _dbVersion->toBSON());
    }

    if (_writeConcern) {
        builder->append(kWriteConcern, *_writeConcern);
    }

    if (!_allowImplicitCollectionCreation) {
        builder->append(kAllowImplicitCollectionCreation, _allowImplicitCollectionCreation);
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

BatchItemRef::BatchItemRef(const BatchedCommandRequest* request, int index)
    : _request(*request), _index(index) {
    invariant(index < int(request->sizeWriteOps()));
}

}  // namespace mongo

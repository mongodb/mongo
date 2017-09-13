/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batched_command_request.h"

#include "mongo/bson/bsonobj.h"

namespace mongo {
namespace {

const auto kWriteConcern = "writeConcern"_sd;

template <class T>
BatchedCommandRequest constructBatchedCommandRequest(const OpMsgRequest& request) {
    auto batchRequest = BatchedCommandRequest{T::parse(request)};

    auto chunkVersion = ChunkVersion::parseFromBSONForCommands(request.body);
    if (chunkVersion != ErrorCodes::NoSuchKey) {
        batchRequest.setShardVersion(uassertStatusOK(std::move(chunkVersion)));
    }

    auto writeConcernField = request.body[kWriteConcern];
    if (!writeConcernField.eoo()) {
        batchRequest.setWriteConcern(writeConcernField.Obj());
    }

    return batchRequest;
}

// This macro just invokes a given method on one of the three types of ops with parameters
#define INVOKE(M, ...)                                    \
    {                                                     \
        switch (_batchType) {                             \
            case BatchedCommandRequest::BatchType_Insert: \
                return _insertReq->M(__VA_ARGS__);        \
            case BatchedCommandRequest::BatchType_Update: \
                return _updateReq->M(__VA_ARGS__);        \
            case BatchedCommandRequest::BatchType_Delete: \
                return _deleteReq->M(__VA_ARGS__);        \
        }                                                 \
        MONGO_UNREACHABLE;                                \
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
    INVOKE(getNamespace);
}

NamespaceString BatchedCommandRequest::getTargetingNS() const {
    if (!isInsertIndexRequest()) {
        return getNS();
    }

    const auto& documents = _insertReq->getDocuments();
    invariant(documents.size() == 1);

    return NamespaceString(documents.at(0)["ns"].str());
}

bool BatchedCommandRequest::isInsertIndexRequest() const {
    if (_batchType != BatchedCommandRequest::BatchType_Insert) {
        return false;
    }

    return getNS().isSystemDotIndexes();
}

std::size_t BatchedCommandRequest::sizeWriteOps() const {
    switch (_batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            return getInsertRequest().getDocuments().size();
        case BatchedCommandRequest::BatchType_Update:
            return getUpdateRequest().getUpdates().size();
        case BatchedCommandRequest::BatchType_Delete:
            return getDeleteRequest().getDeletes().size();
    }
    MONGO_UNREACHABLE;
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
    INVOKE(getWriteCommandBase);
}

void BatchedCommandRequest::setWriteCommandBase(write_ops::WriteCommandBase writeCommandBase) {
    INVOKE(setWriteCommandBase, std::move(writeCommandBase));
}

void BatchedCommandRequest::serialize(BSONObjBuilder* builder) const {
    switch (_batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            getInsertRequest().serialize({}, builder);
            break;
        case BatchedCommandRequest::BatchType_Update:
            getUpdateRequest().serialize({}, builder);
            break;
        case BatchedCommandRequest::BatchType_Delete:
            getDeleteRequest().serialize({}, builder);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    if (_shardVersion) {
        _shardVersion->appendForCommands(builder);
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

    if (newCmdRequest.isInsertIndexRequest()) {
        return newCmdRequest;
    }

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

}  // namespace mongo

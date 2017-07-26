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
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace {

const BSONField<BSONObj> writeConcern("writeConcern");

}  // namespace

BatchedCommandRequest::BatchedCommandRequest(BatchType batchType) : _batchType(batchType) {
    switch (getBatchType()) {
        case BatchedCommandRequest::BatchType_Insert:
            _insertReq.reset(new BatchedInsertRequest);
            return;
        case BatchedCommandRequest::BatchType_Update:
            _updateReq.reset(new BatchedUpdateRequest);
            return;
        default:
            dassert(getBatchType() == BatchedCommandRequest::BatchType_Delete);
            _deleteReq.reset(new BatchedDeleteRequest);
            return;
    }
}

// This macro just invokes a given method on one of the three types of ops with parameters
#define INVOKE(M, ...)                                                              \
    {                                                                               \
        switch (getBatchType()) {                                                   \
            case BatchedCommandRequest::BatchType_Insert:                           \
                return _insertReq->M(__VA_ARGS__);                                  \
            case BatchedCommandRequest::BatchType_Update:                           \
                return _updateReq->M(__VA_ARGS__);                                  \
            default:                                                                \
                dassert(getBatchType() == BatchedCommandRequest::BatchType_Delete); \
                return _deleteReq->M(__VA_ARGS__);                                  \
        }                                                                           \
    }

bool BatchedCommandRequest::isInsertIndexRequest() const {
    if (_batchType != BatchedCommandRequest::BatchType_Insert)
        return false;

    return getNS().isSystemDotIndexes();
}

NamespaceString BatchedCommandRequest::getTargetingNSS() const {
    if (!isInsertIndexRequest()) {
        return getNS();
    }

    const auto& documents = _insertReq->getDocuments();
    invariant(documents.size() == 1);

    return NamespaceString(documents.at(0)["ns"].str());
}

bool BatchedCommandRequest::isVerboseWC() const {
    if (!isWriteConcernSet()) {
        return true;
    }

    BSONObj writeConcern = getWriteConcern();
    BSONElement wElem = writeConcern["w"];
    if (!wElem.isNumber() || wElem.Number() != 0) {
        return true;
    }

    return false;
}

BSONObj BatchedCommandRequest::toBSON() const {
    BSONObjBuilder builder([&] {
        switch (getBatchType()) {
            case BatchedCommandRequest::BatchType_Insert:
                return _insertReq->toBSON();
            case BatchedCommandRequest::BatchType_Update:
                return _updateReq->toBSON();
            case BatchedCommandRequest::BatchType_Delete:
                return _deleteReq->toBSON();
            default:
                MONGO_UNREACHABLE;
        }
    }());

    _writeCommandBase.serialize(&builder);

    if (_shardVersion) {
        _shardVersion->appendForCommands(&builder);
    }

    if (_writeConcern) {
        builder.append(writeConcern(), *_writeConcern);
    }

    return builder.obj();
}

void BatchedCommandRequest::parseRequest(const OpMsgRequest& request) {
    _writeCommandBase =
        write_ops::WriteCommandBase::parse(IDLParserErrorContext("WriteOpTxnInfo"), request.body);

    switch (getBatchType()) {
        case BatchedCommandRequest::BatchType_Insert:
            _insertReq->parseRequest(request);
            break;
        case BatchedCommandRequest::BatchType_Update:
            _updateReq->parseRequest(request);
            break;
        case BatchedCommandRequest::BatchType_Delete:
            _deleteReq->parseRequest(request);
            break;
        default:
            MONGO_UNREACHABLE;
    }

    auto chunkVersion = ChunkVersion::parseFromBSONForCommands(request.body);
    if (chunkVersion != ErrorCodes::NoSuchKey) {
        setShardVersion(uassertStatusOK(std::move(chunkVersion)));
    }

    auto writeConcernField = request.body[writeConcern.name()];
    if (!writeConcernField.eoo()) {
        setWriteConcern(writeConcernField.Obj());
    }
}

std::string BatchedCommandRequest::toString() const {
    INVOKE(toString);
}

void BatchedCommandRequest::setNS(NamespaceString ns) {
    INVOKE(setNS, std::move(ns));
}

const NamespaceString& BatchedCommandRequest::getNS() const {
    INVOKE(getNS);
}

std::size_t BatchedCommandRequest::sizeWriteOps() const {
    switch (getBatchType()) {
        case BatchedCommandRequest::BatchType_Insert:
            return _insertReq->sizeDocuments();
        case BatchedCommandRequest::BatchType_Update:
            return _updateReq->sizeUpdates();
        default:
            return _deleteReq->sizeDeletes();
    }
}

void BatchedCommandRequest::setWriteConcern(const BSONObj& writeConcern) {
    _writeConcern = writeConcern.getOwned();
}

bool BatchedCommandRequest::isWriteConcernSet() const {
    return _writeConcern.is_initialized();
}

const BSONObj& BatchedCommandRequest::getWriteConcern() const {
    invariant(_writeConcern);
    return *_writeConcern;
}

const write_ops::WriteCommandBase& BatchedCommandRequest::getWriteCommandBase() const {
    return _writeCommandBase;
}

void BatchedCommandRequest::setWriteCommandBase(write_ops::WriteCommandBase writeCommandBase) {
    _writeCommandBase = std::move(writeCommandBase);
}

/**
 * Generates a new request with insert _ids if required.  Otherwise returns NULL.
 */
BatchedCommandRequest* BatchedCommandRequest::cloneWithIds(
    const BatchedCommandRequest& origCmdRequest) {
    if (origCmdRequest.getBatchType() != BatchedCommandRequest::BatchType_Insert ||
        origCmdRequest.isInsertIndexRequest()) {
        return nullptr;
    }

    std::unique_ptr<BatchedInsertRequest> idRequest;
    BatchedInsertRequest* origRequest = origCmdRequest.getInsertRequest();

    const std::vector<BSONObj>& inserts = origRequest->getDocuments();

    size_t i = 0u;
    for (auto it = inserts.begin(); it != inserts.end(); ++it, ++i) {
        const BSONObj& insert = *it;
        BSONObj idInsert;

        if (insert["_id"].eoo()) {
            BSONObjBuilder idInsertB;
            idInsertB.append("_id", OID::gen());
            idInsertB.appendElements(insert);
            idInsert = idInsertB.obj();
        }

        if (!idRequest && !idInsert.isEmpty()) {
            idRequest.reset(new BatchedInsertRequest);
            origRequest->cloneTo(idRequest.get());
        }

        if (!idInsert.isEmpty()) {
            idRequest->setDocumentAt(i, idInsert);
        }
    }

    if (!idRequest) {
        return nullptr;
    }

    // Command request owns idRequest
    auto clonedCmdRequest = stdx::make_unique<BatchedCommandRequest>(idRequest.release());

    clonedCmdRequest->_writeCommandBase = origCmdRequest._writeCommandBase;

    if (origCmdRequest.isWriteConcernSet()) {
        clonedCmdRequest->setWriteConcern(origCmdRequest.getWriteConcern());
    }

    return clonedCmdRequest.release();
}

}  // namespace mongo

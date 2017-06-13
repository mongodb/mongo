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

namespace mongo {

const size_t BatchedCommandRequest::kMaxWriteBatchSize = 1000;

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

BatchedInsertRequest* BatchedCommandRequest::getInsertRequest() const {
    return _insertReq.get();
}

BatchedUpdateRequest* BatchedCommandRequest::getUpdateRequest() const {
    return _updateReq.get();
}

BatchedDeleteRequest* BatchedCommandRequest::getDeleteRequest() const {
    return _deleteReq.get();
}

bool BatchedCommandRequest::isInsertIndexRequest() const {
    if (_batchType != BatchedCommandRequest::BatchType_Insert)
        return false;
    return getNS().isSystemDotIndexes();
}

bool BatchedCommandRequest::isValidIndexRequest(std::string* errMsg) const {
    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;
    dassert(isInsertIndexRequest());

    if (sizeWriteOps() != 1) {
        *errMsg = "invalid batch request for index creation";
        return false;
    }

    const NamespaceString& targetNSS = getTargetingNSS();
    if (!targetNSS.isValid()) {
        *errMsg = targetNSS.ns() + " is not a valid namespace to index";
        return false;
    }

    const NamespaceString& reqNSS = getNS();
    if (reqNSS.db().compare(targetNSS.db()) != 0) {
        *errMsg =
            targetNSS.ns() + " namespace is not in the request database " + reqNSS.db().toString();
        return false;
    }

    return true;
}

const NamespaceString& BatchedCommandRequest::getTargetingNSS() const {
    if (!isInsertIndexRequest())
        return getNS();

    return _insertReq->getIndexTargetingNS();
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

bool BatchedCommandRequest::isValid(std::string* errMsg) const {
    INVOKE(isValid, errMsg);
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

    // Append the shard version
    if (_shardVersion) {
        _shardVersion.get().appendForCommands(&builder);
    }

    // Append the transaction info
    _txnInfo.serialize(&builder);

    return builder.obj();
}

void BatchedCommandRequest::parseRequest(const OpMsgRequest& request) {

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

    // Now parse out the chunk version and optime.
    auto chunkVersion = ChunkVersion::parseFromBSONForCommands(request.body);
    if (chunkVersion != ErrorCodes::NoSuchKey) {
        _shardVersion = uassertStatusOK(chunkVersion);
    }

    // Parse the command's transaction info and do extra validation not done by the parser
    _txnInfo = WriteOpTxnInfo::parse(IDLParserErrorContext("WriteOpTxnInfo"), request.body);

    const auto& stmtIds = _txnInfo.getStmtIds();
    uassert(ErrorCodes::BadValue,
            str::stream() << "The size of the statement ids array (" << stmtIds->size()
                          << ") does not match the number of operations ("
                          << sizeWriteOps()
                          << ")",
            !stmtIds || stmtIds->size() == sizeWriteOps());
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
    INVOKE(setWriteConcern, writeConcern);
}

void BatchedCommandRequest::unsetWriteConcern() {
    INVOKE(unsetWriteConcern);
}

bool BatchedCommandRequest::isWriteConcernSet() const {
    INVOKE(isWriteConcernSet);
}

const BSONObj& BatchedCommandRequest::getWriteConcern() const {
    INVOKE(getWriteConcern);
}

void BatchedCommandRequest::setOrdered(bool continueOnError) {
    INVOKE(setOrdered, continueOnError);
}

void BatchedCommandRequest::unsetOrdered() {
    INVOKE(unsetOrdered);
}

bool BatchedCommandRequest::isOrderedSet() const {
    INVOKE(isOrderedSet);
}

bool BatchedCommandRequest::getOrdered() const {
    INVOKE(getOrdered);
}

void BatchedCommandRequest::setShouldBypassValidation(bool newVal) {
    INVOKE(setShouldBypassValidation, newVal);
}

bool BatchedCommandRequest::shouldBypassValidation() const {
    INVOKE(shouldBypassValidation);
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
    return new BatchedCommandRequest(idRequest.release());
}

bool BatchedCommandRequest::containsNoIDUpsert(const BatchedCommandRequest& request) {
    if (request.getBatchType() != BatchedCommandRequest::BatchType_Update) {
        return false;
    }

    const auto& updates = request.getUpdateRequest()->getUpdates();

    for (const auto& updateDoc : updates) {
        if (updateDoc->getUpsert() && updateDoc->getQuery()["_id"].eoo()) {
            return true;
        }
    }

    return false;
}

bool BatchedCommandRequest::containsUpserts(const BSONObj& writeCmdObj) {
    BSONElement updatesEl = writeCmdObj[BatchedUpdateRequest::updates()];
    if (updatesEl.type() != Array) {
        return false;
    }

    BSONObjIterator it(updatesEl.Obj());
    while (it.more()) {
        BSONElement updateEl = it.next();
        if (!updateEl.isABSONObj())
            continue;
        if (updateEl.Obj()[BatchedUpdateDocument::upsert()].trueValue())
            return true;
    }

    return false;
}

bool BatchedCommandRequest::getIndexedNS(const BSONObj& writeCmdObj,
                                         std::string* nsToIndex,
                                         std::string* errMsg) {
    BSONElement documentsEl = writeCmdObj[BatchedInsertRequest::documents()];
    if (documentsEl.type() != Array) {
        *errMsg = "index write batch is invalid";
        return false;
    }

    BSONObjIterator it(documentsEl.Obj());
    if (!it.more()) {
        *errMsg = "index write batch is empty";
        return false;
    }

    BSONElement indexDescEl = it.next();
    *nsToIndex = indexDescEl["ns"].str();
    if (*nsToIndex == "") {
        *errMsg = "index write batch contains an invalid index descriptor";
        return false;
    }

    if (it.more()) {
        *errMsg = "index write batches may only contain a single index descriptor";
        return false;
    }

    return true;
}

const boost::optional<std::int64_t> BatchedCommandRequest::getTxnNum() const& {
    return _txnInfo.getTxnNum();
}

void BatchedCommandRequest::setTxnNum(boost::optional<std::int64_t> value) {
    _txnInfo.setTxnNum(std::move(value));
}

const boost::optional<std::vector<std::int32_t>> BatchedCommandRequest::getStmtIds() const& {
    return _txnInfo.getStmtIds();
}

void BatchedCommandRequest::setStmtIds(boost::optional<std::vector<std::int32_t>> value) {
    invariant(_txnInfo.getTxnNum());
    invariant(!value || value->size() == sizeWriteOps());

    _txnInfo.setStmtIds(std::move(value));
}

int32_t BatchedCommandRequest::getStmtIdForWriteAt(size_t writePos) const {
    invariant(getTxnNum());

    const auto& stmtIds = _txnInfo.getStmtIds();

    if (stmtIds) {
        return stmtIds->at(writePos);
    }

    const int32_t kFirstStmtId = 0;
    return kFirstStmtId + writePos;
}

}  // namespace mongo

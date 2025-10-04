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

#include "mongo/s/write_ops/batched_command_response.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"

#include <iosfwd>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/ostream.h>

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(MultipleErrorsOccurredInfo);

}  // namespace

const BSONField<long long> BatchedCommandResponse::n("n", 0);
const BSONField<long long> BatchedCommandResponse::nModified("nModified", 0);
const BSONField<std::vector<BatchedUpsertDetail*>> BatchedCommandResponse::upsertDetails(
    "upserted");
const BSONField<OID> BatchedCommandResponse::electionId("electionId");
const BSONField<WriteConcernErrorDetail*> BatchedCommandResponse::writeConcernError(
    "writeConcernError");
const BSONField<std::vector<std::string>> BatchedCommandResponse::errorLabels("errorLabels");
const BSONField<std::vector<StmtId>> BatchedCommandResponse::retriedStmtIds("retriedStmtIds");

BatchedCommandResponse::BatchedCommandResponse() {
    clear();
}

BatchedCommandResponse::~BatchedCommandResponse() {
    unsetErrDetails();
    unsetUpsertDetails();
}

BSONObj BatchedCommandResponse::toBSON() const {
    BSONObjBuilder builder;

    invariant(_isStatusSet);
    uassertStatusOK(_status);

    if (_isNModifiedSet)
        builder.appendNumber(nModified(), _nModified);
    if (_isNSet)
        builder.appendNumber(n(), _n);

    if (_upsertDetails.get()) {
        BSONArrayBuilder upsertedBuilder(builder.subarrayStart(upsertDetails()));
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            BSONObj upsertedDetailsDocument = (*it)->toBSON();
            upsertedBuilder.append(upsertedDetailsDocument);
        }
        upsertedBuilder.done();
    }

    if (_isLastOpSet) {
        if (_lastOp.getTerm() != repl::OpTime::kUninitializedTerm) {
            _lastOp.append("opTime", &builder);
        } else {
            builder.append("opTime", _lastOp.getTimestamp());
        }
    }
    if (_isElectionIdSet)
        builder.appendOID(electionId(), const_cast<OID*>(&_electionId));

    if (_writeErrors) {
        auto truncateErrorMessage = [errorCount = size_t(0),
                                     errorSize = size_t(0)](StringData rawMessage) mutable {
            // Start truncating error messages once both of these limits are exceeded.
            constexpr size_t kErrorSizeTruncationMin = 1024 * 1024;
            constexpr size_t kErrorCountTruncationMin = 2;
            if (errorSize >= kErrorSizeTruncationMin && errorCount >= kErrorCountTruncationMin) {
                return true;
            }

            errorCount++;
            errorSize += rawMessage.size();
            return false;
        };

        BSONArrayBuilder errDetailsBuilder(
            builder.subarrayStart(write_ops::WriteCommandReplyBase::kWriteErrorsFieldName));
        for (auto&& writeError : *_writeErrors) {
            if (truncateErrorMessage(writeError.getStatus().reason())) {
                write_ops::WriteError truncatedError(writeError.getIndex(),
                                                     writeError.getStatus().withReason(""));
                errDetailsBuilder.append(truncatedError.serialize());
            } else {
                errDetailsBuilder.append(writeError.serialize());
            }
        }
        errDetailsBuilder.done();
    }

    if (_wcErrDetails.get()) {
        builder.append(writeConcernError(), _wcErrDetails->toBSON());
    }

    if (areRetriedStmtIdsSet()) {
        builder.append(retriedStmtIds(), _retriedStmtIds);
    }

    return builder.obj();
}

bool BatchedCommandResponse::parseBSON(const BSONObj& source, std::string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    _status = getStatusFromCommandResult(source);
    _isStatusSet = true;

    // We're using appendNumber on generation so we'll try a smaller type
    // (int) first and then fall back to the original type (long long).
    BSONField<int> fieldN(n());
    int tempN;
    auto fieldState = FieldParser::extract(source, fieldN, &tempN, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID) {
        // try falling back to a larger type
        fieldState = FieldParser::extract(source, n, &_n, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID)
            return false;
        _isNSet = fieldState == FieldParser::FIELD_SET;
    } else if (fieldState == FieldParser::FIELD_SET) {
        _isNSet = true;
        _n = tempN;
    }

    // We're using appendNumber on generation so we'll try a smaller type
    // (int) first and then fall back to the original type (long long).
    BSONField<int> fieldNModified(nModified());
    int intNModified;
    fieldState = FieldParser::extract(source, fieldNModified, &intNModified, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID) {
        // try falling back to a larger type
        fieldState = FieldParser::extract(source, nModified, &_nModified, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID)
            return false;
        _isNModifiedSet = fieldState == FieldParser::FIELD_SET;
    } else if (fieldState == FieldParser::FIELD_SET) {
        _isNModifiedSet = true;
        _nModified = intNModified;
    }

    std::vector<BatchedUpsertDetail*>* tempUpsertDetails = nullptr;
    fieldState = FieldParser::extract(source, upsertDetails, &tempUpsertDetails, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _upsertDetails.reset(tempUpsertDetails);

    const BSONElement opTimeElement = source["opTime"];
    _isLastOpSet = true;
    if (opTimeElement.eoo()) {
        _isLastOpSet = false;
    } else if (opTimeElement.type() == BSONType::timestamp) {
        _lastOp = repl::OpTime(opTimeElement.timestamp(), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == BSONType::date) {
        _lastOp = repl::OpTime(Timestamp(opTimeElement.date()), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == BSONType::object) {
        Status status = bsonExtractOpTimeField(source, "opTime", &_lastOp);
        if (!status.isOK()) {
            return false;
        }
    } else {
        return false;
    }

    fieldState = FieldParser::extract(source, electionId, &_electionId, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isElectionIdSet = fieldState == FieldParser::FIELD_SET;

    if (auto writeErrorsElem = source[write_ops::WriteCommandReplyBase::kWriteErrorsFieldName]) {
        for (auto writeError : writeErrorsElem.Array()) {
            if (!_writeErrors)
                _writeErrors.emplace();
            _writeErrors->emplace_back(write_ops::WriteError::parse(writeError.Obj()));
        }
    }

    WriteConcernErrorDetail* wcError = nullptr;
    fieldState = FieldParser::extract(source, writeConcernError, &wcError, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _wcErrDetails.reset(wcError);

    std::vector<std::string> tempErrorLabels;
    fieldState = FieldParser::extract(source, errorLabels, &tempErrorLabels, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _errorLabels = std::move(tempErrorLabels);

    std::vector<StmtId> tempRetriedStmtIds;
    fieldState = FieldParser::extract(source, retriedStmtIds, &tempRetriedStmtIds, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _retriedStmtIds = std::move(tempRetriedStmtIds);

    return true;
}

void BatchedCommandResponse::clear() {
    _status = Status::OK();
    _isStatusSet = false;

    _nModified = 0;
    _isNModifiedSet = false;

    _n = 0;
    _isNSet = false;

    if (_upsertDetails.get()) {
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            delete *it;
        };
        _upsertDetails.reset();
    }

    _lastOp = repl::OpTime();
    _isLastOpSet = false;

    _electionId = OID();
    _isElectionIdSet = false;

    _writeErrors.reset();

    _wcErrDetails.reset();
}

void BatchedCommandResponse::setStatus(Status status) {
    _status = std::move(status);
    _isStatusSet = true;
}

void BatchedCommandResponse::setNModified(long long n) {
    _nModified = n;
    _isNModifiedSet = true;
}

long long BatchedCommandResponse::getNModified() const {
    if (_isNModifiedSet) {
        return _nModified;
    } else {
        return nModified.getDefault();
    }
}

void BatchedCommandResponse::setN(long long n) {
    _n = n;
    _isNSet = true;
}

long long BatchedCommandResponse::getN() const {
    if (_isNSet) {
        return _n;
    } else {
        return n.getDefault();
    }
}

void BatchedCommandResponse::setUpsertDetails(
    const std::vector<BatchedUpsertDetail*>& upsertDetails) {
    unsetUpsertDetails();
    for (std::vector<BatchedUpsertDetail*>::const_iterator it = upsertDetails.begin();
         it != upsertDetails.end();
         ++it) {
        std::unique_ptr<BatchedUpsertDetail> tempBatchedUpsertDetail(new BatchedUpsertDetail);
        (*it)->cloneTo(tempBatchedUpsertDetail.get());
        addToUpsertDetails(tempBatchedUpsertDetail.release());
    }
}

void BatchedCommandResponse::addToUpsertDetails(BatchedUpsertDetail* upsertDetails) {
    if (_upsertDetails.get() == nullptr) {
        _upsertDetails.reset(new std::vector<BatchedUpsertDetail*>);
    }
    _upsertDetails->push_back(upsertDetails);
}

void BatchedCommandResponse::unsetUpsertDetails() {
    if (_upsertDetails.get() != nullptr) {
        for (std::vector<BatchedUpsertDetail*>::iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            delete *it;
        }
        _upsertDetails.reset();
    }
}

bool BatchedCommandResponse::isUpsertDetailsSet() const {
    return _upsertDetails.get() != nullptr;
}

size_t BatchedCommandResponse::sizeUpsertDetails() const {
    dassert(_upsertDetails.get());
    return _upsertDetails->size();
}

const std::vector<BatchedUpsertDetail*>& BatchedCommandResponse::getUpsertDetails() const {
    dassert(_upsertDetails.get());
    return *_upsertDetails;
}

const BatchedUpsertDetail* BatchedCommandResponse::getUpsertDetailsAt(size_t pos) const {
    dassert(_upsertDetails.get());
    dassert(_upsertDetails->size() > pos);
    return _upsertDetails->at(pos);
}

void BatchedCommandResponse::setLastOp(repl::OpTime lastOp) {
    _lastOp = lastOp;
    _isLastOpSet = true;
}

bool BatchedCommandResponse::isLastOpSet() const {
    return _isLastOpSet;
}

repl::OpTime BatchedCommandResponse::getLastOp() const {
    dassert(_isLastOpSet);
    return _lastOp;
}

void BatchedCommandResponse::setElectionId(const OID& electionId) {
    _electionId = electionId;
    _isElectionIdSet = true;
}

bool BatchedCommandResponse::isElectionIdSet() const {
    return _isElectionIdSet;
}

OID BatchedCommandResponse::getElectionId() const {
    dassert(_isElectionIdSet);
    return _electionId;
}

void BatchedCommandResponse::addToErrDetails(write_ops::WriteError error) {
    if (!_writeErrors)
        _writeErrors.emplace();
    _writeErrors->emplace_back(std::move(error));
}

void BatchedCommandResponse::unsetErrDetails() {
    _writeErrors.reset();
}

bool BatchedCommandResponse::isErrDetailsSet() const {
    return _writeErrors.has_value();
}

size_t BatchedCommandResponse::sizeErrDetails() const {
    dassert(isErrDetailsSet());
    return _writeErrors->size();
}

std::vector<write_ops::WriteError>& BatchedCommandResponse::getErrDetails() {
    dassert(isErrDetailsSet());
    return *_writeErrors;
}

const std::vector<write_ops::WriteError>& BatchedCommandResponse::getErrDetails() const {
    dassert(isErrDetailsSet());
    return *_writeErrors;
}

const write_ops::WriteError& BatchedCommandResponse::getErrDetailsAt(size_t pos) const {
    dassert(isErrDetailsSet());
    dassert(pos < _writeErrors->size());
    return _writeErrors->at(pos);
}

void BatchedCommandResponse::setWriteConcernError(WriteConcernErrorDetail* error) {
    _wcErrDetails.reset(error);
}

bool BatchedCommandResponse::isWriteConcernErrorSet() const {
    return _wcErrDetails.get();
}

const WriteConcernErrorDetail* BatchedCommandResponse::getWriteConcernError() const {
    return _wcErrDetails.get();
}

Status BatchedCommandResponse::toStatus() const {
    if (!getOk()) {
        return _status;
    }

    if (isErrDetailsSet()) {
        return getErrDetails().front().getStatus();
    }

    if (isWriteConcernErrorSet()) {
        return getWriteConcernError()->toStatus();
    }

    return Status::OK();
}

bool BatchedCommandResponse::isErrorLabelsSet() const {
    return !_errorLabels.empty();
}

const std::vector<std::string>& BatchedCommandResponse::getErrorLabels() const {
    return _errorLabels;
}

bool BatchedCommandResponse::areRetriedStmtIdsSet() const {
    return !_retriedStmtIds.empty();
}

const std::vector<StmtId>& BatchedCommandResponse::getRetriedStmtIds() const {
    return _retriedStmtIds;
}

void BatchedCommandResponse::setRetriedStmtIds(std::vector<StmtId> retriedStmtIds) {
    _retriedStmtIds = std::move(retriedStmtIds);
}

std::shared_ptr<const ErrorExtraInfo> MultipleErrorsOccurredInfo::parse(const BSONObj& obj) {
    // The server never receives this error as a response from another node, so there is never
    // need to parse it.
    uasserted(645200,
              "The MultipleErrorsOccurred error should never be used for intra-cluster "
              "communication");
}

void MultipleErrorsOccurredInfo::serialize(BSONObjBuilder* bob) const {
    BSONObjBuilder errInfoBuilder(bob->subobjStart(write_ops::WriteError::kErrInfoFieldName));
    errInfoBuilder.append("causedBy", _arr);
}

}  // namespace mongo

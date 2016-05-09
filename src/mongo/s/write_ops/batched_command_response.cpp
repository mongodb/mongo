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

#include "mongo/s/write_ops/batched_command_response.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::string;

using mongoutils::str::stream;

const BSONField<int> BatchedCommandResponse::ok("ok");
const BSONField<int> BatchedCommandResponse::errCode("code", ErrorCodes::UnknownError);
const BSONField<string> BatchedCommandResponse::errMessage("errmsg");
const BSONField<long long> BatchedCommandResponse::n("n", 0);
const BSONField<long long> BatchedCommandResponse::nModified("nModified", 0);
const BSONField<std::vector<BatchedUpsertDetail*>> BatchedCommandResponse::upsertDetails(
    "upserted");
const BSONField<OID> BatchedCommandResponse::electionId("electionId");
const BSONField<std::vector<WriteErrorDetail*>> BatchedCommandResponse::writeErrors("writeErrors");
const BSONField<WriteConcernErrorDetail*> BatchedCommandResponse::writeConcernError(
    "writeConcernError");

BatchedCommandResponse::BatchedCommandResponse() {
    clear();
}

BatchedCommandResponse::~BatchedCommandResponse() {
    unsetErrDetails();
    unsetUpsertDetails();
}

bool BatchedCommandResponse::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isOkSet) {
        *errMsg = stream() << "missing " << ok.name() << " field";
        return false;
    }

    return true;
}

BSONObj BatchedCommandResponse::toBSON() const {
    BSONObjBuilder builder;

    if (_isOkSet)
        builder.append(ok(), _ok);

    if (_isErrCodeSet)
        builder.append(errCode(), _errCode);

    if (_isErrMessageSet)
        builder.append(errMessage(), _errMessage);

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
            _lastOp.append(&builder, "opTime");
        } else {
            builder.append("opTime", _lastOp.getTimestamp());
        }
    }
    if (_isElectionIdSet)
        builder.appendOID(electionId(), const_cast<OID*>(&_electionId));

    if (_writeErrorDetails.get()) {
        BSONArrayBuilder errDetailsBuilder(builder.subarrayStart(writeErrors()));
        for (std::vector<WriteErrorDetail*>::const_iterator it = _writeErrorDetails->begin();
             it != _writeErrorDetails->end();
             ++it) {
            BSONObj errDetailsDocument = (*it)->toBSON();
            errDetailsBuilder.append(errDetailsDocument);
        }
        errDetailsBuilder.done();
    }

    if (_wcErrDetails.get()) {
        builder.append(writeConcernError(), _wcErrDetails->toBSON());
    }

    return builder.obj();
}

bool BatchedCommandResponse::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;
    fieldState = FieldParser::extractNumber(source, ok, &_ok, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isOkSet = fieldState == FieldParser::FIELD_SET;

    fieldState = FieldParser::extract(source, errCode, &_errCode, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isErrCodeSet = fieldState == FieldParser::FIELD_SET;

    fieldState = FieldParser::extract(source, errMessage, &_errMessage, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isErrMessageSet = fieldState == FieldParser::FIELD_SET;

    // We're using appendNumber on generation so we'll try a smaller type
    // (int) first and then fall back to the original type (long long).
    BSONField<int> fieldN(n());
    int tempN;
    fieldState = FieldParser::extract(source, fieldN, &tempN, errMsg);
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

    std::vector<BatchedUpsertDetail*>* tempUpsertDetails = NULL;
    fieldState = FieldParser::extract(source, upsertDetails, &tempUpsertDetails, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _upsertDetails.reset(tempUpsertDetails);

    const BSONElement opTimeElement = source["opTime"];
    _isLastOpSet = true;
    if (opTimeElement.eoo()) {
        _isLastOpSet = false;
    } else if (opTimeElement.type() == bsonTimestamp) {
        _lastOp = repl::OpTime(opTimeElement.timestamp(), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == Date) {
        _lastOp = repl::OpTime(Timestamp(opTimeElement.date()), repl::OpTime::kUninitializedTerm);
    } else if (opTimeElement.type() == Object) {
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

    std::vector<WriteErrorDetail*>* tempErrDetails = NULL;
    fieldState = FieldParser::extract(source, writeErrors, &tempErrDetails, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _writeErrorDetails.reset(tempErrDetails);

    WriteConcernErrorDetail* wcError = NULL;
    fieldState = FieldParser::extract(source, writeConcernError, &wcError, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _wcErrDetails.reset(wcError);

    return true;
}

void BatchedCommandResponse::clear() {
    _ok = false;
    _isOkSet = false;

    _errCode = 0;
    _isErrCodeSet = false;

    _errMessage.clear();
    _isErrMessageSet = false;

    _nModified = 0;
    _isNModifiedSet = false;

    _n = 0;
    _isNSet = false;

    _singleUpserted = BSONObj();
    _isSingleUpsertedSet = false;

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

    if (_writeErrorDetails.get()) {
        for (std::vector<WriteErrorDetail*>::const_iterator it = _writeErrorDetails->begin();
             it != _writeErrorDetails->end();
             ++it) {
            delete *it;
        };
        _writeErrorDetails.reset();
    }

    _wcErrDetails.reset();
}

void BatchedCommandResponse::cloneTo(BatchedCommandResponse* other) const {
    other->clear();

    other->_ok = _ok;
    other->_isOkSet = _isOkSet;

    other->_errCode = _errCode;
    other->_isErrCodeSet = _isErrCodeSet;

    other->_errMessage = _errMessage;
    other->_isErrMessageSet = _isErrMessageSet;

    other->_nModified = _nModified;
    other->_isNModifiedSet = _isNModifiedSet;

    other->_n = _n;
    other->_isNSet = _isNSet;

    other->_singleUpserted = _singleUpserted;
    other->_isSingleUpsertedSet = _isSingleUpsertedSet;

    other->unsetUpsertDetails();
    if (_upsertDetails.get()) {
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            BatchedUpsertDetail* upsertDetailsItem = new BatchedUpsertDetail;
            (*it)->cloneTo(upsertDetailsItem);
            other->addToUpsertDetails(upsertDetailsItem);
        }
    }

    other->_lastOp = _lastOp;
    other->_isLastOpSet = _isLastOpSet;

    other->_electionId = _electionId;
    other->_isElectionIdSet = _isElectionIdSet;

    other->unsetErrDetails();
    if (_writeErrorDetails.get()) {
        for (std::vector<WriteErrorDetail*>::const_iterator it = _writeErrorDetails->begin();
             it != _writeErrorDetails->end();
             ++it) {
            WriteErrorDetail* errDetailsItem = new WriteErrorDetail;
            (*it)->cloneTo(errDetailsItem);
            other->addToErrDetails(errDetailsItem);
        }
    }

    if (_wcErrDetails.get()) {
        other->_wcErrDetails.reset(new WriteConcernErrorDetail());
        _wcErrDetails->cloneTo(other->_wcErrDetails.get());
    }
}

std::string BatchedCommandResponse::toString() const {
    return toBSON().toString();
}

void BatchedCommandResponse::setOk(int ok) {
    _ok = ok;
    _isOkSet = true;
}

int BatchedCommandResponse::getOk() const {
    dassert(_isOkSet);
    return _ok;
}

void BatchedCommandResponse::setErrCode(int errCode) {
    _errCode = errCode;
    _isErrCodeSet = true;
}

void BatchedCommandResponse::unsetErrCode() {
    _isErrCodeSet = false;
}

bool BatchedCommandResponse::isErrCodeSet() const {
    return _isErrCodeSet;
}

int BatchedCommandResponse::getErrCode() const {
    if (_isErrCodeSet) {
        return _errCode;
    } else {
        return errCode.getDefault();
    }
}

void BatchedCommandResponse::setErrMessage(StringData errMessage) {
    _errMessage = errMessage.toString();
    _isErrMessageSet = true;
}

bool BatchedCommandResponse::isErrMessageSet() const {
    return _isErrMessageSet;
}

const std::string& BatchedCommandResponse::getErrMessage() const {
    dassert(_isErrMessageSet);
    return _errMessage;
}

void BatchedCommandResponse::setNModified(long long n) {
    _nModified = n;
    _isNModifiedSet = true;
}

void BatchedCommandResponse::unsetNModified() {
    _isNModifiedSet = false;
}

bool BatchedCommandResponse::isNModified() const {
    return _isNModifiedSet;
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

void BatchedCommandResponse::unsetN() {
    _isNSet = false;
}

bool BatchedCommandResponse::isNSet() const {
    return _isNSet;
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
        unique_ptr<BatchedUpsertDetail> tempBatchedUpsertDetail(new BatchedUpsertDetail);
        (*it)->cloneTo(tempBatchedUpsertDetail.get());
        addToUpsertDetails(tempBatchedUpsertDetail.release());
    }
}

void BatchedCommandResponse::addToUpsertDetails(BatchedUpsertDetail* upsertDetails) {
    if (_upsertDetails.get() == NULL) {
        _upsertDetails.reset(new std::vector<BatchedUpsertDetail*>);
    }
    _upsertDetails->push_back(upsertDetails);
}

void BatchedCommandResponse::unsetUpsertDetails() {
    if (_upsertDetails.get() != NULL) {
        for (std::vector<BatchedUpsertDetail*>::iterator it = _upsertDetails->begin();
             it != _upsertDetails->end();
             ++it) {
            delete *it;
        }
        _upsertDetails.reset();
    }
}

bool BatchedCommandResponse::isUpsertDetailsSet() const {
    return _upsertDetails.get() != NULL;
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

void BatchedCommandResponse::unsetLastOp() {
    _isLastOpSet = false;
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

void BatchedCommandResponse::unsetElectionId() {
    _isElectionIdSet = false;
}

bool BatchedCommandResponse::isElectionIdSet() const {
    return _isElectionIdSet;
}

OID BatchedCommandResponse::getElectionId() const {
    dassert(_isElectionIdSet);
    return _electionId;
}

void BatchedCommandResponse::setErrDetails(const std::vector<WriteErrorDetail*>& errDetails) {
    unsetErrDetails();
    for (std::vector<WriteErrorDetail*>::const_iterator it = errDetails.begin();
         it != errDetails.end();
         ++it) {
        unique_ptr<WriteErrorDetail> tempBatchErrorDetail(new WriteErrorDetail);
        (*it)->cloneTo(tempBatchErrorDetail.get());
        addToErrDetails(tempBatchErrorDetail.release());
    }
}

void BatchedCommandResponse::addToErrDetails(WriteErrorDetail* errDetails) {
    if (_writeErrorDetails.get() == NULL) {
        _writeErrorDetails.reset(new std::vector<WriteErrorDetail*>);
    }
    _writeErrorDetails->push_back(errDetails);
}

void BatchedCommandResponse::unsetErrDetails() {
    if (_writeErrorDetails.get() != NULL) {
        for (std::vector<WriteErrorDetail*>::iterator it = _writeErrorDetails->begin();
             it != _writeErrorDetails->end();
             ++it) {
            delete *it;
        }
        _writeErrorDetails.reset();
    }
}

bool BatchedCommandResponse::isErrDetailsSet() const {
    return _writeErrorDetails.get() != NULL;
}

size_t BatchedCommandResponse::sizeErrDetails() const {
    dassert(_writeErrorDetails.get());
    return _writeErrorDetails->size();
}

const std::vector<WriteErrorDetail*>& BatchedCommandResponse::getErrDetails() const {
    dassert(_writeErrorDetails.get());
    return *_writeErrorDetails;
}

const WriteErrorDetail* BatchedCommandResponse::getErrDetailsAt(size_t pos) const {
    dassert(_writeErrorDetails.get());
    dassert(_writeErrorDetails->size() > pos);
    return _writeErrorDetails->at(pos);
}

void BatchedCommandResponse::setWriteConcernError(WriteConcernErrorDetail* error) {
    _wcErrDetails.reset(error);
}

void BatchedCommandResponse::unsetWriteConcernError() {
    _wcErrDetails.reset();
}

bool BatchedCommandResponse::isWriteConcernErrorSet() const {
    return _wcErrDetails.get();
}

const WriteConcernErrorDetail* BatchedCommandResponse::getWriteConcernError() const {
    return _wcErrDetails.get();
}

Status BatchedCommandResponse::toStatus() const {
    if (!getOk()) {
        return Status(ErrorCodes::fromInt(getErrCode()), getErrMessage());
    }

    if (isErrDetailsSet()) {
        const WriteErrorDetail* errDetail = getErrDetails().front();

        return Status(ErrorCodes::fromInt(errDetail->getErrCode()), errDetail->getErrMessage());
    }

    if (isWriteConcernErrorSet()) {
        const WriteConcernErrorDetail* errDetail = getWriteConcernError();

        return Status(ErrorCodes::fromInt(errDetail->getErrCode()), errDetail->getErrMessage());
    }

    return Status::OK();
}

}  // namespace mongo

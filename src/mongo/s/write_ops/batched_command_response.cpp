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

#include "mongo/s/write_ops/batched_command_response.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const BSONField<int> BatchedCommandResponse::ok("ok");
    const BSONField<int> BatchedCommandResponse::errCode("code", ErrorCodes::UnknownError);
    const BSONField<BSONObj> BatchedCommandResponse::errInfo("errInfo");
    const BSONField<string> BatchedCommandResponse::errMessage("errmsg");
    const BSONField<long long> BatchedCommandResponse::n("n", 0);
    const BSONField<long long> BatchedCommandResponse::nDocsModified("nDocsModified", 0);
    const BSONField<BSONObj> BatchedCommandResponse::singleUpserted("upserted");
    const BSONField<std::vector<BatchedUpsertDetail*> >
        BatchedCommandResponse::upsertDetails("upserted");
    const BSONField<Date_t> BatchedCommandResponse::lastOp("lastOp");
    const BSONField<std::vector<BatchedErrorDetail*> >
        BatchedCommandResponse::errDetails("errDetails");

    BatchedCommandResponse::BatchedCommandResponse() {
        clear();
    }

    BatchedCommandResponse::~BatchedCommandResponse() {
        unsetErrDetails();
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

        // upserted and singleUpserted cannot live together
        if (_isSingleUpsertedSet && _upsertDetails.get()) {
            *errMsg = stream() << "duplicated " << singleUpserted.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj BatchedCommandResponse::toBSON() const {
        BSONObjBuilder builder;

        if (_isOkSet) builder.append(ok(), _ok);

        if (_isErrCodeSet) builder.append(errCode(), _errCode);

        if (_isErrInfoSet) builder.append(errInfo(), _errInfo);

        if (_isErrMessageSet) builder.append(errMessage(), _errMessage);

        if (_isNDocsModifiedSet) builder.appendNumber(nDocsModified(), _nDocsModified);
        if (_isNSet) builder.appendNumber(n(), _n);

        // We're using the BSONObj to store the _id value.
        if (_isSingleUpsertedSet) {
            builder.appendAs(_singleUpserted.firstElement(), singleUpserted());
        }

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

        if (_isLastOpSet) builder.append(lastOp(), _lastOp);

        if (_errDetails.get()) {
            BSONArrayBuilder errDetailsBuilder(builder.subarrayStart(errDetails()));
            for (std::vector<BatchedErrorDetail*>::const_iterator it = _errDetails->begin();
                 it != _errDetails->end();
                 ++it) {
                BSONObj errDetailsDocument = (*it)->toBSON();
                errDetailsBuilder.append(errDetailsDocument);
            }
            errDetailsBuilder.done();
        }

        return builder.obj();
    }

    bool BatchedCommandResponse::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extractNumber(source, ok, &_ok, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isOkSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, errCode, &_errCode, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isErrCodeSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, errInfo, &_errInfo, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isErrInfoSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, errMessage, &_errMessage, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isErrMessageSet = fieldState == FieldParser::FIELD_SET;

        // We're using appendNumber on generation so we'll try a smaller type
        // (int) first and then fall back to the original type (long long).
        BSONField<int> fieldN(n());
        int tempN;
        fieldState = FieldParser::extract(source, fieldN, &tempN, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            // try falling back to a larger type
            fieldState = FieldParser::extract(source, n, &_n, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID) return false;
            _isNSet = fieldState == FieldParser::FIELD_SET;
        }
        else if (fieldState == FieldParser::FIELD_SET) {
            _isNSet = true;
            _n = tempN;
        }

        // We're using appendNumber on generation so we'll try a smaller type
        // (int) first and then fall back to the original type (long long).
        BSONField<int> fieldNUpdated(nDocsModified());
        int tempNUpdated;
        fieldState = FieldParser::extract(source, fieldNUpdated, &tempNUpdated, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            // try falling back to a larger type
            fieldState = FieldParser::extract(source, nDocsModified, &_nDocsModified, errMsg);
            if (fieldState == FieldParser::FIELD_INVALID) return false;
            _isNDocsModifiedSet = fieldState == FieldParser::FIELD_SET;
        }
        else if (fieldState == FieldParser::FIELD_SET) {
            _isNDocsModifiedSet = true;
            _nDocsModified = tempNUpdated;
        }

        // singleUpserted and upsertDetails have the same field name, but are distinguished
        // by type.  First try parsing singleUpserted, if that doesn't work, try upsertDetails
        fieldState = FieldParser::extractID(source, singleUpserted, &_singleUpserted, errMsg);
        _isSingleUpsertedSet = fieldState == FieldParser::FIELD_SET;

        // Try upsertDetails if singleUpserted didn't work
        if (fieldState == FieldParser::FIELD_INVALID) {
            std::vector<BatchedUpsertDetail*>* tempUpsertDetails = NULL;
            fieldState = FieldParser::extract( source, upsertDetails, &tempUpsertDetails, errMsg );
            if ( fieldState == FieldParser::FIELD_INVALID ) return false;
            if ( fieldState == FieldParser::FIELD_SET ) _upsertDetails.reset( tempUpsertDetails );
        }

        fieldState = FieldParser::extract(source, lastOp, &_lastOp, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isLastOpSet = fieldState == FieldParser::FIELD_SET;

        std::vector<BatchedErrorDetail*>* tempErrDetails = NULL;
        fieldState = FieldParser::extract(source, errDetails, &tempErrDetails, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        if (fieldState == FieldParser::FIELD_SET) _errDetails.reset(tempErrDetails);

        return true;
    }

    void BatchedCommandResponse::clear() {
        _ok = false;
        _isOkSet = false;

        _errCode = 0;
        _isErrCodeSet = false;

        _errInfo = BSONObj();
        _isErrInfoSet = false;

        _errMessage.clear();
        _isErrMessageSet = false;

        _nDocsModified = 0;
        _isNDocsModifiedSet = false;

        _n = 0;
        _isNSet = false;

        _singleUpserted = BSONObj();
        _isSingleUpsertedSet = false;

        _errDetails.reset();

        _lastOp = 0ULL;
        _isLastOpSet = false;

        if (_errDetails.get()) {
            for(std::vector<BatchedErrorDetail*>::const_iterator it = _errDetails->begin();
                it != _errDetails->end();
                ++it) {
                delete *it;
            };
            _errDetails.reset();
        }
    }

    void BatchedCommandResponse::cloneTo(BatchedCommandResponse* other) const {
        other->clear();

        other->_ok = _ok;
        other->_isOkSet = _isOkSet;

        other->_errCode = _errCode;
        other->_isErrCodeSet = _isErrCodeSet;

        other->_errInfo = _errInfo;
        other->_isErrInfoSet = _isErrInfoSet;

        other->_errMessage = _errMessage;
        other->_isErrMessageSet = _isErrMessageSet;

        other->_nDocsModified = _nDocsModified;
        other->_isNDocsModifiedSet = _isNDocsModifiedSet;

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

        other->unsetErrDetails();
        if (_errDetails.get()) {
            for(std::vector<BatchedErrorDetail*>::const_iterator it = _errDetails->begin();
                it != _errDetails->end();
                ++it) {
                BatchedErrorDetail* errDetailsItem = new BatchedErrorDetail;
                (*it)->cloneTo(errDetailsItem);
                other->addToErrDetails(errDetailsItem);
            }
        }
    }

    std::string BatchedCommandResponse::toString() const {
        return toBSON().toString();
    }

    void BatchedCommandResponse::setOk(int ok) {
        _ok = ok;
        _isOkSet = true;
    }

    void BatchedCommandResponse::unsetOk() {
         _isOkSet = false;
     }

    bool BatchedCommandResponse::isOkSet() const {
         return _isOkSet;
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
        if ( _isErrCodeSet ) {
            return _errCode;
        }
        else {
            return errCode.getDefault();
        }
    }

    void BatchedCommandResponse::setErrInfo(const BSONObj& errInfo) {
        _errInfo = errInfo.getOwned();
        _isErrInfoSet = true;
    }

    void BatchedCommandResponse::unsetErrInfo() {
         _isErrInfoSet = false;
     }

    bool BatchedCommandResponse::isErrInfoSet() const {
         return _isErrInfoSet;
    }

    const BSONObj& BatchedCommandResponse::getErrInfo() const {
        dassert(_isErrInfoSet);
        return _errInfo;
    }

    void BatchedCommandResponse::setErrMessage(const StringData& errMessage) {
        _errMessage = errMessage.toString();
        _isErrMessageSet = true;
    }

    void BatchedCommandResponse::unsetErrMessage() {
         _isErrMessageSet = false;
     }

    bool BatchedCommandResponse::isErrMessageSet() const {
         return _isErrMessageSet;
    }

    const std::string& BatchedCommandResponse::getErrMessage() const {
        dassert(_isErrMessageSet);
        return _errMessage;
    }

    void BatchedCommandResponse::setNDocsModified(long long n) {
        _nDocsModified = n;
        _isNDocsModifiedSet = true;
    }

    void BatchedCommandResponse::unsetNDocsModified() {
         _isNDocsModifiedSet = false;
     }

    bool BatchedCommandResponse::isNDocsModified() const {
         return _isNDocsModifiedSet;
    }

    long long BatchedCommandResponse::getNDocsModified() const {
        if ( _isNDocsModifiedSet ) {
            return _nDocsModified;
        }
        else {
            return nDocsModified.getDefault();
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
        if ( _isNSet ) {
            return _n;
        }
        else {
            return n.getDefault();
        }
    }

    void BatchedCommandResponse::setSingleUpserted(const BSONObj& singleUpserted) {
        _singleUpserted = singleUpserted.firstElement().wrap( "" ).getOwned();
        _isSingleUpsertedSet = true;
    }

    void BatchedCommandResponse::unsetSingleUpserted() {
         _isSingleUpsertedSet = false;
     }

    bool BatchedCommandResponse::isSingleUpsertedSet() const {
         return _isSingleUpsertedSet;
    }

    const BSONObj& BatchedCommandResponse::getSingleUpserted() const {
        dassert(_isSingleUpsertedSet);
        return _singleUpserted;
    }

    void BatchedCommandResponse::setUpsertDetails(
        const std::vector<BatchedUpsertDetail*>& upsertDetails) {
        unsetUpsertDetails();
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = upsertDetails.begin();
             it != upsertDetails.end();
             ++it) {
            auto_ptr<BatchedUpsertDetail> tempBatchedUpsertDetail(new BatchedUpsertDetail);
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

    void BatchedCommandResponse::setLastOp(Date_t lastOp) {
        _lastOp = lastOp;
        _isLastOpSet = true;
    }

    void BatchedCommandResponse::unsetLastOp() {
         _isLastOpSet = false;
     }

    bool BatchedCommandResponse::isLastOpSet() const {
         return _isLastOpSet;
    }

    Date_t BatchedCommandResponse::getLastOp() const {
        dassert(_isLastOpSet);
        return _lastOp;
    }

    void BatchedCommandResponse::setErrDetails(const std::vector<BatchedErrorDetail*>& errDetails) {
        unsetErrDetails();
        for (std::vector<BatchedErrorDetail*>::const_iterator it = errDetails.begin();
             it != errDetails.end();
             ++it) {
            auto_ptr<BatchedErrorDetail> tempBatchErrorDetail(new BatchedErrorDetail);
            (*it)->cloneTo(tempBatchErrorDetail.get());
            addToErrDetails(tempBatchErrorDetail.release());
        }
    }

    void BatchedCommandResponse::addToErrDetails(BatchedErrorDetail* errDetails) {
        if (_errDetails.get() == NULL) {
            _errDetails.reset(new std::vector<BatchedErrorDetail*>);
        }
        _errDetails->push_back(errDetails);
    }

    void BatchedCommandResponse::unsetErrDetails() {
        if (_errDetails.get() != NULL) {
            for(std::vector<BatchedErrorDetail*>::iterator it = _errDetails->begin();
                it != _errDetails->end();
                ++it) {
                delete *it;
            }
            _errDetails.reset();
        }
    }

    bool BatchedCommandResponse::isErrDetailsSet() const {
        return _errDetails.get() != NULL;
    }

    size_t BatchedCommandResponse::sizeErrDetails() const {
        dassert(_errDetails.get());
        return _errDetails->size();
    }

    const std::vector<BatchedErrorDetail*>& BatchedCommandResponse::getErrDetails() const {
        dassert(_errDetails.get());
        return *_errDetails;
    }

    const BatchedErrorDetail* BatchedCommandResponse::getErrDetailsAt(size_t pos) const {
        dassert(_errDetails.get());
        dassert(_errDetails->size() > pos);
        return _errDetails->at(pos);
    }

} // namespace mongo

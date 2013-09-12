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
 */

#include "mongo/s/batched_command_response.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;
        const BSONField<bool> BatchedCommandResponse::ok("ok");
        const BSONField<int> BatchedCommandResponse::errCode("errCode");
        const BSONField<BSONObj> BatchedCommandResponse::errInfo("errInfo");
        const BSONField<string> BatchedCommandResponse::errMessage("errMessage");
        const BSONField<long long> BatchedCommandResponse::n("n");
        const BSONField<long long> BatchedCommandResponse::upserted("upserted");
        const BSONField<Date_t> BatchedCommandResponse::lastOp("lastOP");
        const BSONField<std::vector<BatchedErrorDetail*> > BatchedCommandResponse::errDetails("errDetails");

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

        if (!_isErrCodeSet) {
            *errMsg = stream() << "missing " << errCode.name() << " field";
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

        if (_isNSet) builder.append(n(), _n);

        if (_isUpsertedSet) builder.append(upserted(), _upserted);

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
        fieldState = FieldParser::extract(source, ok, &_ok, errMsg);
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

        fieldState = FieldParser::extract(source, n, &_n, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, upserted, &_upserted, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpsertedSet = fieldState == FieldParser::FIELD_SET;

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

        _n = 0;
        _isNSet = false;

        _upserted = 0;
        _isUpsertedSet = false;

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

        other->_n = _n;
        other->_isNSet = _isNSet;

        other->_upserted = _upserted;
        other->_isUpsertedSet = _isUpsertedSet;

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
        return "implement me";
    }

    void BatchedCommandResponse::setOk(bool ok) {
        _ok = ok;
        _isOkSet = true;
    }

    void BatchedCommandResponse::unsetOk() {
         _isOkSet = false;
     }

    bool BatchedCommandResponse::isOkSet() const {
         return _isOkSet;
    }

    bool BatchedCommandResponse::getOk() const {
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
        dassert(_isErrCodeSet);
        return _errCode;
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
        dassert(_isNSet);
        return _n;
    }

    void BatchedCommandResponse::setUpserted(long long upserted) {
        _upserted = upserted;
        _isUpsertedSet = true;
    }

    void BatchedCommandResponse::unsetUpserted() {
         _isUpsertedSet = false;
     }

    bool BatchedCommandResponse::isUpsertedSet() const {
         return _isUpsertedSet;
    }

    long long BatchedCommandResponse::getUpserted() const {
        dassert(_isUpsertedSet);
        return _upserted;
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

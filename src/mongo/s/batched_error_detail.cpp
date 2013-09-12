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

#include "mongo/s/batched_error_detail.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;
        const BSONField<int> BatchedErrorDetail::index("index");
        const BSONField<int> BatchedErrorDetail::errCode("errCode");
        const BSONField<BSONObj> BatchedErrorDetail::errInfo("errInfo");
        const BSONField<std::string> BatchedErrorDetail::errMessage("errMessage");

    BatchedErrorDetail::BatchedErrorDetail() {
        clear();
    }

    BatchedErrorDetail::~BatchedErrorDetail() {
    }

    bool BatchedErrorDetail::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isIndexSet) {
            *errMsg = stream() << "missing " << index.name() << " field";
            return false;
        }

        if (!_isErrCodeSet) {
            *errMsg = stream() << "missing " << errCode.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj BatchedErrorDetail::toBSON() const {
        BSONObjBuilder builder;

        if (_isIndexSet) builder.append(index(), _index);

        if (_isErrCodeSet) builder.append(errCode(), _errCode);

        if (_isErrInfoSet) builder.append(errInfo(), _errInfo);

        if (_isErrMessageSet) builder.append(errMessage(), _errMessage);

        return builder.obj();
    }

    bool BatchedErrorDetail::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, index, &_index, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isIndexSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, errCode, &_errCode, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isErrCodeSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, errInfo, &_errInfo, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isErrInfoSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, errMessage, &_errMessage, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isErrMessageSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void BatchedErrorDetail::clear() {
        _index = 0;
        _isIndexSet = false;

        _errCode = 0;
        _isErrCodeSet = false;

        _errInfo = BSONObj();
        _isErrInfoSet = false;

        _errMessage.clear();
        _isErrMessageSet = false;

    }

    void BatchedErrorDetail::cloneTo(BatchedErrorDetail* other) const {
        other->clear();

        other->_index = _index;
        other->_isIndexSet = _isIndexSet;

        other->_errCode = _errCode;
        other->_isErrCodeSet = _isErrCodeSet;

        other->_errInfo = _errInfo;
        other->_isErrInfoSet = _isErrInfoSet;

        other->_errMessage = _errMessage;
        other->_isErrMessageSet = _isErrMessageSet;
    }

    std::string BatchedErrorDetail::toString() const {
        return "implement me";
    }

    void BatchedErrorDetail::setIndex(int index) {
        _index = index;
        _isIndexSet = true;
    }

    void BatchedErrorDetail::unsetIndex() {
         _isIndexSet = false;
     }

    bool BatchedErrorDetail::isIndexSet() const {
         return _isIndexSet;
    }

    int BatchedErrorDetail::getIndex() const {
        dassert(_isIndexSet);
        return _index;
    }

    void BatchedErrorDetail::setErrCode(int errCode) {
        _errCode = errCode;
        _isErrCodeSet = true;
    }

    void BatchedErrorDetail::unsetErrCode() {
         _isErrCodeSet = false;
     }

    bool BatchedErrorDetail::isErrCodeSet() const {
         return _isErrCodeSet;
    }

    int BatchedErrorDetail::getErrCode() const {
        dassert(_isErrCodeSet);
        return _errCode;
    }

    void BatchedErrorDetail::setErrInfo(const BSONObj& errInfo) {
        _errInfo = errInfo.getOwned();
        _isErrInfoSet = true;
    }

    void BatchedErrorDetail::unsetErrInfo() {
         _isErrInfoSet = false;
     }

    bool BatchedErrorDetail::isErrInfoSet() const {
         return _isErrInfoSet;
    }

    const BSONObj& BatchedErrorDetail::getErrInfo() const {
        dassert(_isErrInfoSet);
        return _errInfo;
    }

    void BatchedErrorDetail::setErrMessage(const StringData& errMessage) {
        _errMessage = errMessage.toString();
        _isErrMessageSet = true;
    }

    void BatchedErrorDetail::unsetErrMessage() {
         _isErrMessageSet = false;
     }

    bool BatchedErrorDetail::isErrMessageSet() const {
         return _isErrMessageSet;
    }

    const std::string& BatchedErrorDetail::getErrMessage() const {
        dassert(_isErrMessageSet);
        return _errMessage;
    }

} // namespace mongo

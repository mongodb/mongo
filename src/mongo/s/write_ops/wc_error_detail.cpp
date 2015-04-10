/**
 *    Copyright (C) 2013 mongoDB Inc.
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

#include "mongo/s/write_ops/wc_error_detail.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::string;

    using mongoutils::str::stream;
    const BSONField<int> WCErrorDetail::errCode("code");
    const BSONField<BSONObj> WCErrorDetail::errInfo("errInfo");
    const BSONField<std::string> WCErrorDetail::errMessage("errmsg");

    WCErrorDetail::WCErrorDetail() {
        clear();
    }

    WCErrorDetail::~WCErrorDetail() {
    }

    bool WCErrorDetail::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isErrCodeSet) {
            *errMsg = stream() << "missing " << errCode.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj WCErrorDetail::toBSON() const {
        BSONObjBuilder builder;

        if (_isErrCodeSet) builder.append(errCode(), _errCode);

        if (_isErrInfoSet) builder.append(errInfo(), _errInfo);

        if (_isErrMessageSet) builder.append(errMessage(), _errMessage);

        return builder.obj();
    }

    bool WCErrorDetail::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
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

    void WCErrorDetail::clear() {
        _errCode = 0;
        _isErrCodeSet = false;

        _errInfo = BSONObj();
        _isErrInfoSet = false;

        _errMessage.clear();
        _isErrMessageSet = false;

    }

    void WCErrorDetail::cloneTo(WCErrorDetail* other) const {
        other->clear();

        other->_errCode = _errCode;
        other->_isErrCodeSet = _isErrCodeSet;

        other->_errInfo = _errInfo;
        other->_isErrInfoSet = _isErrInfoSet;

        other->_errMessage = _errMessage;
        other->_isErrMessageSet = _isErrMessageSet;
    }

    std::string WCErrorDetail::toString() const {
        return "implement me";
    }

    void WCErrorDetail::setErrCode(int errCode) {
        _errCode = errCode;
        _isErrCodeSet = true;
    }

    void WCErrorDetail::unsetErrCode() {
         _isErrCodeSet = false;
     }

    bool WCErrorDetail::isErrCodeSet() const {
         return _isErrCodeSet;
    }

    int WCErrorDetail::getErrCode() const {
        dassert(_isErrCodeSet);
        return _errCode;
    }

    void WCErrorDetail::setErrInfo(const BSONObj& errInfo) {
        _errInfo = errInfo.getOwned();
        _isErrInfoSet = true;
    }

    void WCErrorDetail::unsetErrInfo() {
         _isErrInfoSet = false;
     }

    bool WCErrorDetail::isErrInfoSet() const {
         return _isErrInfoSet;
    }

    const BSONObj& WCErrorDetail::getErrInfo() const {
        dassert(_isErrInfoSet);
        return _errInfo;
    }

    void WCErrorDetail::setErrMessage(StringData errMessage) {
        _errMessage = errMessage.toString();
        _isErrMessageSet = true;
    }

    void WCErrorDetail::unsetErrMessage() {
         _isErrMessageSet = false;
     }

    bool WCErrorDetail::isErrMessageSet() const {
         return _isErrMessageSet;
    }

    const std::string& WCErrorDetail::getErrMessage() const {
        dassert(_isErrMessageSet);
        return _errMessage;
    }

} // namespace mongo

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

#pragma once

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * This class represents the layout and content of the error that occurs while trying
 * to satisfy the write concern after executing runCommand.
 */
class WriteConcernErrorDetail {
    MONGO_DISALLOW_COPYING(WriteConcernErrorDetail);

public:
    WriteConcernErrorDetail();

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(WriteConcernErrorDetail* other) const;

    //
    // bson serializable interface implementation
    //

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    bool parseBSON(const BSONObj& source, std::string* errMsg);
    void clear();
    std::string toString() const;
    Status toStatus() const;

    //
    // individual field accessors
    //

    void setErrCode(ErrorCodes::Error code);
    ErrorCodes::Error getErrCode() const;

    void setErrInfo(const BSONObj& errInfo);
    bool isErrInfoSet() const;
    const BSONObj& getErrInfo() const;

    void setErrMessage(StringData errMessage);
    bool isErrMessageSet() const;
    const std::string& getErrMessage() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  error code for the write concern error.
    ErrorCodes::Error _errCode;
    bool _isErrCodeSet;

    // (O)  further details about the write concern error.
    BSONObj _errInfo;
    bool _isErrInfoSet;

    // (O)  user readable explanation about the write concern error.
    std::string _errMessage;
    bool _isErrMessageSet;
};

}  // namespace mongo

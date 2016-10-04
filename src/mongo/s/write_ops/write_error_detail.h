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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * This class represents the layout and content of a insert/update/delete runCommand,
 * the response side.
 */
class WriteErrorDetail {
    MONGO_DISALLOW_COPYING(WriteErrorDetail);

public:
    //
    // schema declarations
    //

    static const BSONField<int> index;
    static const BSONField<int> errCode;
    static const BSONField<BSONObj> errInfo;
    static const BSONField<std::string> errMessage;

    //
    // construction / destruction
    //

    WriteErrorDetail();

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(WriteErrorDetail* other) const;

    //
    // bson serializable interface implementation
    //

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    bool parseBSON(const BSONObj& source, std::string* errMsg);
    void clear();
    std::string toString() const;

    //
    // individual field accessors
    //

    void setIndex(int index);
    bool isIndexSet() const;
    int getIndex() const;

    void setErrCode(int errCode);
    bool isErrCodeSet() const;
    int getErrCode() const;

    void setErrInfo(const BSONObj& errInfo);
    bool isErrInfoSet() const;
    const BSONObj& getErrInfo() const;

    void setErrMessage(StringData errMessage);
    bool isErrMessageSet() const;
    const std::string& getErrMessage() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  number of the batch item the error refers to
    int _index;
    bool _isIndexSet;

    // (M)  whether all items in the batch applied correctly
    int _errCode;
    bool _isErrCodeSet;

    // (O)  further details about the batch item error
    BSONObj _errInfo;
    bool _isErrInfoSet;

    // (O)  user readable explanation about the batch item error
    std::string _errMessage;
    bool _isErrMessageSet;
};

}  // namespace mongo

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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/bson_serializable.h"

namespace mongo {

    /**
     * This class represents the layout and content of a insert/update/delete runCommand,
     * the response side.
     */
    class BatchedErrorDetail : public BSONSerializable {
        MONGO_DISALLOW_COPYING(BatchedErrorDetail);
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

        BatchedErrorDetail();
        virtual ~BatchedErrorDetail();

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo(BatchedErrorDetail* other) const;

        //
        // bson serializable interface implementation
        //

        virtual bool isValid(std::string* errMsg) const;
        virtual BSONObj toBSON() const;
        virtual bool parseBSON(const BSONObj& source, std::string* errMsg);
        virtual void clear();
        virtual std::string toString() const;

        //
        // individual field accessors
        //

        void setIndex(int index);
        void unsetIndex();
        bool isIndexSet() const;
        int getIndex() const;

        void setErrCode(int errCode);
        void unsetErrCode();
        bool isErrCodeSet() const;
        int getErrCode() const;

        void setErrInfo(const BSONObj& errInfo);
        void unsetErrInfo();
        bool isErrInfoSet() const;
        const BSONObj& getErrInfo() const;

        void setErrMessage(const StringData& errMessage);
        void unsetErrMessage();
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

} // namespace mongo

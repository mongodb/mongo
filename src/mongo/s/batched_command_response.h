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

#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/batched_error_detail.h"
#include "mongo/s/bson_serializable.h"

namespace mongo {

    /**
     * This class represents the layout and content of a insert/update/delete runCommand,
     * the response side.
     */
    class BatchedCommandResponse : public BSONSerializable {
        MONGO_DISALLOW_COPYING(BatchedCommandResponse);
    public:

        //
        // schema declarations
        //

        static const BSONField<bool> ok;
        static const BSONField<int> errCode;
        static const BSONField<BSONObj> errInfo;
        static const BSONField<string> errMessage;
        static const BSONField<long long> n;
        static const BSONField<long long> upserted;
        static const BSONField<Date_t> lastOp;
        static const BSONField<std::vector<BatchedErrorDetail*> > errDetails;

        //
        // construction / destruction
        //

        BatchedCommandResponse();
        virtual ~BatchedCommandResponse();

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo(BatchedCommandResponse* other) const;

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

        void setOk(bool ok);
        void unsetOk();
        bool isOkSet() const;
        bool getOk() const;

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

        void setN(long long n);
        void unsetN();
        bool isNSet() const;
        long long getN() const;

        void setUpserted(long long upserted);
        void unsetUpserted();
        bool isUpsertedSet() const;
        long long getUpserted() const;

        void setLastOp(Date_t lastOp);
        void unsetLastOp();
        bool isLastOpSet() const;
        Date_t getLastOp() const;

        void setErrDetails(const std::vector<BatchedErrorDetail*>& errDetails);
        void addToErrDetails(BatchedErrorDetail* errDetails);
        void unsetErrDetails();
        bool isErrDetailsSet() const;
        std::size_t sizeErrDetails() const;
        const std::vector<BatchedErrorDetail*>& getErrDetails() const;
        const BatchedErrorDetail* getErrDetailsAt(std::size_t pos) const;

    private:
        // Convention: (M)andatory, (O)ptional

        // (M)  false if batch didn't get to be applied for any reason
        bool _ok;
        bool _isOkSet;

        // (M)  whether all items in the batch applied correctly
        int _errCode;
        bool _isErrCodeSet;

        // (O)  further details about the error
        BSONObj _errInfo;
        bool _isErrInfoSet;

        // (O)  whether all items in the batch applied correctly
        string _errMessage;
        bool _isErrMessageSet;

        // (O)  number of documents affected
        long long _n;
        bool _isNSet;

        // (O)  in updates, number of ops that were upserts
        long long _upserted;
        bool _isUpsertedSet;

        // (O)  XXX What is lastop?
        Date_t _lastOp;
        bool _isLastOpSet;

        // (O)  Array of item-level error information
        boost::scoped_ptr<std::vector<BatchedErrorDetail*> >_errDetails;
    };

} // namespace mongo

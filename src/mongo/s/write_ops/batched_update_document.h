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
     * This class represents the layout and content of a update document runCommand,
     * in the request side.
     */
    class BatchedUpdateDocument : public BSONSerializable {
        MONGO_DISALLOW_COPYING(BatchedUpdateDocument);
    public:

        //
        // schema declarations
        //

        static const BSONField<BSONObj> query;
        static const BSONField<BSONObj> updateExpr;
        static const BSONField<bool> multi;
        static const BSONField<bool> upsert;

        //
        // construction / destruction
        //

        BatchedUpdateDocument();
        virtual ~BatchedUpdateDocument();

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo(BatchedUpdateDocument* other) const;

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

        void setQuery(const BSONObj& query);
        void unsetQuery();
        bool isQuerySet() const;
        const BSONObj& getQuery() const;

        void setUpdateExpr(const BSONObj& updateExpr);
        void unsetUpdateExpr();
        bool isUpdateExprSet() const;
        const BSONObj& getUpdateExpr() const;

        void setMulti(bool multi);
        void unsetMulti();
        bool isMultiSet() const;
        bool getMulti() const;

        void setUpsert(bool upsert);
        void unsetUpsert();
        bool isUpsertSet() const;
        bool getUpsert() const;

    private:
        // Convention: (M)andatory, (O)ptional

        // (M)  query whose result the update will manipulate
        BSONObj _query;
        bool _isQuerySet;

        // (M)  the update expression itself
        BSONObj _updateExpr;
        bool _isUpdateExprSet;

        // (O)  whether multiple documents are to be updated
        bool _multi;
        bool _isMultiSet;

        // (O)  whether upserts are allowed
        bool _upsert;
        bool _isUpsertSet;
    };

} // namespace mongo

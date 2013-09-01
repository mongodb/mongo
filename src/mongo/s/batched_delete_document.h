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
     * This class represents the layout and content of a delete document runCommand,
     * in the resquest side.
     */
    class BatchedDeleteDocument : public BSONSerializable {
        MONGO_DISALLOW_COPYING(BatchedDeleteDocument);
    public:

        //
        // schema declarations
        //

        static const BSONField<BSONObj> query;
        static const BSONField<int> limit;

        //
        // construction / destruction
        //

        BatchedDeleteDocument();
        virtual ~BatchedDeleteDocument();

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo(BatchedDeleteDocument* other) const;

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

        void setLimit(int limit);
        void unsetLimit();
        bool isLimitSet() const;
        int getLimit() const;

    private:
        // Convention: (M)andatory, (O)ptional

        // (M)  query whose result the delete will remove
        BSONObj _query;
        bool _isQuerySet;

        // (O)  cap the number of documents to be deleted
        int _limit;
        bool _isLimitSet;
    };

} // namespace mongo

//@file dbclientmockcursor.h

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientmockcursor.h"

namespace mongo {

    /**
     * Simple adapter class for mongo::DBClientMockCursor to mongo::DBClientCursor.
     * Only supports more and next, the behavior of other operations are undefined.
     */
    class MockDBClientCursor: public mongo::DBClientCursor {
    public:
        MockDBClientCursor(mongo::DBClientBase* client,
                const mongo::BSONArray& mockCollection);

        bool more();

        /**
         * Note: has the same contract as DBClientCursor - returned BSONObj will
         * become invalid when this cursor is destroyed.
         */
        mongo::BSONObj next();

    private:
        boost::scoped_ptr<mongo::DBClientMockCursor> _cursor;
        mongo::BSONObj _resultSet;
    };
}

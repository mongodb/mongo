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

#include "mongo/dbtests/mock/mock_dbclient_cursor.h"

namespace mongo {
    MockDBClientCursor::MockDBClientCursor(mongo::DBClientBase* client,
            const mongo::BSONArray& resultSet):
        mongo::DBClientCursor(client, "", 0, 0, 0) {
        _resultSet = resultSet.copy();
        _cursor.reset(new mongo::DBClientMockCursor(BSONArray(_resultSet)));
    }

    bool MockDBClientCursor::more() {
        return _cursor->more();
    }

    mongo::BSONObj MockDBClientCursor::next() {
        return _cursor->next();
    }
}

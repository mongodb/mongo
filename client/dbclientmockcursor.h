//@file dbclientmockcursor.h

/*    Copyright 2010 10gen Inc.
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

#include "dbclientcursor.h"

namespace mongo {

    class DBClientMockCursor : public DBClientCursorInterface {
    public:
        DBClientMockCursor( const BSONArray& mockCollection ) : _iter( mockCollection ) {}
        virtual ~DBClientMockCursor() {}

        bool more() { return _iter.more(); }
        BSONObj next() { return _iter.next().Obj(); }

    private:
        BSONObjIterator _iter;

        // non-copyable , non-assignable
        DBClientMockCursor( const DBClientMockCursor& );
        DBClientMockCursor& operator=( const DBClientMockCursor& );
    };

} // namespace mongo

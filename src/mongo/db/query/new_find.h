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

#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/util/net/message.h"

namespace mongo {

    /**
     * A switch to choose between old Cursor-based code and new Runner-based code.
     */
    bool isNewQueryFrameworkEnabled();

    /**
     * Use the new query framework.  Called from the dbtest initialization.
     */
    void enableNewQueryFramework();

    /**
     * Called from the getMore entry point in ops/query.cpp.
     */
    QueryResult* newGetMore(const char* ns, int ntoreturn, long long cursorid, CurOp& curop,
                            int pass, bool& exhaust, bool* isCursorAuthorized);

    /**
     * Called from the runQuery entry point in ops/query.cpp.
     */
    string newRunQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result);

}  // namespace mongo

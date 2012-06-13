/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/diskloc.h"

namespace mongo {
    class NamespaceDetails;
    // page in both index and data pages for an op from the oplog
    void prefetchPagesForReplicatedOp(const BSONObj& op);

    // page in pages needed for all index lookups on a given object
    void prefetchIndexPages(NamespaceDetails *nsd, const BSONObj& obj);

    // page in the data pages for a record associated with an object
    void prefetchRecordPages(const char *ns, const BSONObj& obj);
}

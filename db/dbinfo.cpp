// dbinfo.cpp

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

#include "stdafx.h"
#include "db.h"
#include "query.h"

namespace mongo {

    void DBInfo::dbDropped() {
        BSONObj empty;
        deleteObjects(ns.c_str(), empty, false);

        /* do we also need to clear the info in 'dbs' in local.sources if we
        are a slave?
           TODO if so.  need to be careful not to mess up replications of dropDatabase().
           */
    }


} // namespace mongo

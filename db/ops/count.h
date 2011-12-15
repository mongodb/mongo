// count.h

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

#include "../jsobj.h"
#include "../diskloc.h"

namespace mongo {
    
    /**
     * { count: "collectionname"[, query: <query>] }
     * @return -1 on ns does not exist error and other errors, 0 on other errors, otherwise the match count.
     */
    long long runCount(const char *ns, const BSONObj& cmd, string& err);
    
} // namespace mongo

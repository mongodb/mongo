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

#include "mongo/db/dbmessage.h"

namespace mongo {

    class CanonicalQuery {
    public:
        static CanonicalQuery* canonicalize(const QueryMessage& qm) {
            auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
            return cq.release();
        }

        const string& ns() {
            return _ns;
        }

    private:
        CanonicalQuery() { }

        string _ns;
        BSONObj _sort;
        int limit;
        // TODO: Other query arguments.
    };

}  // namespace mongo

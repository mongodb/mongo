/**
*    Copyright (C) 2010 10gen Inc.
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

/* background.h

   Concurrency coordination for administrative operations.
*/

#pragma once

#include <map>
#include <set>
#include <string>
#include <sstream>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    /* these are administrative operations / jobs
       for a namespace running in the background, and that only one
       at a time per namespace is permitted, and that if in progress,
       you aren't allowed to do other NamespaceDetails major manipulations
       (such as dropping ns or db) even in the foreground and must
       instead uassert.

       It's assumed this is not for super-high RPS things, so we don't do
       anything special in the implementation here to be fast.
    */
    class BackgroundOperation : public boost::noncopyable {
    public:
        static bool inProgForDb(const StringData& db);
        static bool inProgForNs(const StringData& ns);
        static void assertNoBgOpInProgForDb(const StringData& db);
        static void assertNoBgOpInProgForNs(const StringData& ns);
        static void dump(std::stringstream&);

        /* check for in progress before instantiating */
        BackgroundOperation(const StringData& ns);

        virtual ~BackgroundOperation();

    private:
        NamespaceString _ns;
        static std::map<std::string, unsigned> dbsInProg;
        static std::set<std::string> nsInProg;
        static SimpleMutex m;
    };

} // namespace mongo


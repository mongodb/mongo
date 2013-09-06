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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
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


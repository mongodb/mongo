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
        static bool inProgForDb(const char *db);
        static bool inProgForNs(const char *ns);
        static void assertNoBgOpInProgForDb(const char *db);
        static void assertNoBgOpInProgForNs(const char *ns);
        static void dump(stringstream&);

        /* check for in progress before instantiating */
        BackgroundOperation(const char *ns);

        virtual ~BackgroundOperation();

    private:
        NamespaceString _ns;
        static map<string, unsigned> dbsInProg;
        static set<string> nsInProg;
        static SimpleMutex m;
    };

} // namespace mongo


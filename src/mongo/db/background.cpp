// background.cpp

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

#include "mongo/db/background.h"

namespace mongo {

    SimpleMutex BackgroundOperation::m("bg");
    std::map<std::string, unsigned> BackgroundOperation::dbsInProg;
    std::set<std::string> BackgroundOperation::nsInProg;

    bool BackgroundOperation::inProgForDb(const StringData& db) {
        SimpleMutex::scoped_lock lk(m);
        return dbsInProg[db.toString()] > 0;
    }

    bool BackgroundOperation::inProgForNs(const StringData& ns) {
        SimpleMutex::scoped_lock lk(m);
        return nsInProg.count(ns.toString()) > 0;
    }

    void BackgroundOperation::assertNoBgOpInProgForDb(const StringData& db) {
        uassert(12586,
                "cannot perform operation: a background operation is currently running for this database",
                !inProgForDb(db));
    }

    void BackgroundOperation::assertNoBgOpInProgForNs(const StringData& ns) {
        uassert(12587,
                "cannot perform operation: a background operation is currently running for this collection",
                !inProgForNs(ns));
    }

    BackgroundOperation::BackgroundOperation(const StringData& ns) : _ns(ns) {
        SimpleMutex::scoped_lock lk(m);
        dbsInProg[_ns.db().toString()]++;
        nsInProg.insert(_ns.ns());
    }

    BackgroundOperation::~BackgroundOperation() {
        SimpleMutex::scoped_lock lk(m);
        dbsInProg[_ns.db().toString()]--;
        nsInProg.erase(_ns.ns());
    }

    void BackgroundOperation::dump(std::stringstream& ss) {
        SimpleMutex::scoped_lock lk(m);
        if( nsInProg.size() ) {
            ss << "\n<b>Background Jobs in Progress</b>\n";
            for( std::set<std::string>::iterator i = nsInProg.begin(); i != nsInProg.end(); i++ )
                ss << "  " << *i << '\n';
        }
        for( std::map<std::string,unsigned>::iterator i = dbsInProg.begin(); i != dbsInProg.end(); i++ ) {
            if( i->second )
                ss << "database " << i->first << ": " << i->second << '\n';
        }
    }




} // namespace mongo


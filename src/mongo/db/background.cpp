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


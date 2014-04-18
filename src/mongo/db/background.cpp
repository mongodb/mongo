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

#include "mongo/platform/basic.h"

#include "mongo/db/background.h"

#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <iostream>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {

    class BgInfo {
        MONGO_DISALLOW_COPYING(BgInfo);
    public:
        BgInfo() : _opsInProgCount(0) {}

        void recordBegin();
        int recordEnd();
        void awaitNoBgOps(boost::unique_lock<boost::mutex>& lk);

        int getOpsInProgCount() const { return _opsInProgCount; }

    private:
        int _opsInProgCount;
        boost::condition_variable _noOpsInProg;
    };

    typedef StringMap<boost::shared_ptr<BgInfo> > BgInfoMap;
    typedef BgInfoMap::const_iterator BgInfoMapIterator;

    boost::mutex m;
    BgInfoMap dbsInProg;
    BgInfoMap nsInProg;

    void BgInfo::recordBegin() {
        ++_opsInProgCount;
    }

    int BgInfo::recordEnd() {
        dassert(_opsInProgCount > 0);
        --_opsInProgCount;
        if (0 == _opsInProgCount) {
            _noOpsInProg.notify_all();
        }
        return _opsInProgCount;
    }

    void BgInfo::awaitNoBgOps(boost::unique_lock<boost::mutex>& lk) {
        while (_opsInProgCount > 0)
            _noOpsInProg.wait(lk);
    }

    void recordBeginAndInsert(BgInfoMap* bgiMap, const StringData& key) {
        boost::shared_ptr<BgInfo>& bgInfo = bgiMap->get(key);
        if (!bgInfo)
            bgInfo.reset(new BgInfo);
        bgInfo->recordBegin();
    }

    void recordEndAndRemove(BgInfoMap* bgiMap, const StringData& key) {
        BgInfoMapIterator iter = bgiMap->find(key);
        fassert(17443, iter != bgiMap->end());
        if (0 == iter->second->recordEnd()) {
            bgiMap->erase(iter);
        }
    }

    void awaitNoBgOps(
            boost::unique_lock<boost::mutex>& lk,
            BgInfoMap* bgiMap,
            const StringData& key) {

        boost::shared_ptr<BgInfo> bgInfo = mapFindWithDefault(
                *bgiMap, key, boost::shared_ptr<BgInfo>());
        if (!bgInfo)
            return;
        bgInfo->awaitNoBgOps(lk);
    }

}  // namespace
    bool BackgroundOperation::inProgForDb(const StringData& db) {
        boost::mutex::scoped_lock lk(m);
        return dbsInProg.find(db) != dbsInProg.end();
    }

    bool BackgroundOperation::inProgForNs(const StringData& ns) {
        boost::mutex::scoped_lock lk(m);
        return nsInProg.find(ns) != nsInProg.end();
    }

    void BackgroundOperation::assertNoBgOpInProgForDb(const StringData& db) {
        uassert(ErrorCodes::BackgroundOperationInProgressForDatabase, mongoutils::str::stream() <<
                "cannot perform operation: a background operation is currently running for "
                "database " << db,
                !inProgForDb(db));
    }

    void BackgroundOperation::assertNoBgOpInProgForNs(const StringData& ns) {
        uassert(ErrorCodes::BackgroundOperationInProgressForNamespace, mongoutils::str::stream() <<
                "cannot perform operation: a background operation is currently running for "
                "collection" << ns,
                !inProgForNs(ns));
    }

    void BackgroundOperation::awaitNoBgOpInProgForDb(const StringData& db) {
        boost::unique_lock<boost::mutex> lk(m);
        awaitNoBgOps(lk, &dbsInProg, db);
    }

    void BackgroundOperation::awaitNoBgOpInProgForNs(const StringData& ns) {
        boost::unique_lock<boost::mutex> lk(m);
        awaitNoBgOps(lk, &nsInProg, ns);
    }

    BackgroundOperation::BackgroundOperation(const StringData& ns) : _ns(ns) {
        boost::mutex::scoped_lock lk(m);
        recordBeginAndInsert(&dbsInProg, _ns.db());
        recordBeginAndInsert(&nsInProg, _ns.ns());
    }

    BackgroundOperation::~BackgroundOperation() {
        boost::mutex::scoped_lock lk(m);
        recordEndAndRemove(&dbsInProg, _ns.db());
        recordEndAndRemove(&nsInProg, _ns.ns());
    }

    void BackgroundOperation::dump(std::ostream& ss) {
        boost::mutex::scoped_lock lk(m);
        if( nsInProg.size() ) {
            ss << "\n<b>Background Jobs in Progress</b>\n";
            for( BgInfoMapIterator i = nsInProg.begin(); i != nsInProg.end(); ++i )
                ss << "  " << i->first << '\n';
        }
        for( BgInfoMapIterator i = dbsInProg.begin(); i != dbsInProg.end(); ++i ) {
            if( i->second->getOpsInProgCount() )
                ss << "database " << i->first << ": " << i->second->getOpsInProgCount() << '\n';
        }
    }

} // namespace mongo


// wiredtiger_recovery_unit.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc) :
        _sessionCache( sc ),
        _session( NULL ),
        _depth(0) {
    }

    WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
        invariant( _depth == 0 );
        _abort();
    }

    void WiredTigerRecoveryUnit::_commit() {
        if ( _session ) {
            WT_SESSION *s = _session->getSession();
            int ret = s->commit_transaction(s, NULL);
            invariantWTOK(ret);
            _sessionCache->releaseSession( _session );
            _session = NULL;
        }

        for (Changes::iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();
    }

    void WiredTigerRecoveryUnit::_abort() {
        if ( _session ) {
            WT_SESSION *s = _session->getSession();
            int ret = s->rollback_transaction(s, NULL);
            invariantWTOK(ret);
            _sessionCache->releaseSession( _session );
            _session = NULL;
        }

        for (Changes::reverse_iterator it = _changes.rbegin(), end = _changes.rend();
                it != end; ++it) {
            (*it)->rollback();
        }
        _changes.clear();
    }

    void WiredTigerRecoveryUnit::beginUnitOfWork() {
        _depth++;
    }

    void WiredTigerRecoveryUnit::commitUnitOfWork() {
        if (_depth > 1)
            return; // only outermost WUOW gets committed.
        _commit();
    }

    void WiredTigerRecoveryUnit::endUnitOfWork() {
        _depth--;
        if ( _depth == 0 ) {
            _abort();
        }
    }

    bool WiredTigerRecoveryUnit::awaitCommit() {
        // TODO need to block until data is on disk.
        return true;
    }

    void WiredTigerRecoveryUnit::registerChange(Change* change) {
        invariant(_depth > 0);
        _changes.push_back(ChangePtr(change));
    }

    WiredTigerRecoveryUnit& WiredTigerRecoveryUnit::Get(OperationContext *txn) {
        invariant( txn );
        return *dynamic_cast<WiredTigerRecoveryUnit*>(txn->recoveryUnit());
    }

    WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
        if ( !_session ) {
            _session = _sessionCache->getSession();
            WT_SESSION *s = _session->getSession();
            int ret = s->begin_transaction(s, NULL);
            invariantWTOK(ret);
        }
        return _session;
    }

    // ---------------------


    namespace {
        void _checkCursor( const std::string* uri, WT_CURSOR* c ) {
            if ( c )
                return;
            error() << "no cursor for uri: " << *uri;
        }
    }

    WiredTigerCursor::WiredTigerCursor(const std::string* uri, WiredTigerRecoveryUnit* ru)
        : _uri( uri ), _ru( ru ),
          _cursor( ru->getSession()->getCursor( *uri ) ) {
        _checkCursor( uri, _cursor );
    }

    WiredTigerCursor::WiredTigerCursor(const std::string* uri, OperationContext* txn)
        : _uri( uri ), _ru( &WiredTigerRecoveryUnit::Get( txn ) ),
          _cursor( _ru->getSession()->getCursor( *uri ) ) {
        _checkCursor( uri, _cursor );
    }

    WiredTigerCursor::~WiredTigerCursor() {
        _ru->getSession()->releaseCursor( *_uri, _cursor );
        _cursor = NULL;
    }


}

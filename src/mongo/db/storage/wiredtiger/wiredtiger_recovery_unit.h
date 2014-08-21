// wiredtiger_recovery_unit.h

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

    class WiredTigerRecoveryUnit : public RecoveryUnit {
        public:
        WiredTigerRecoveryUnit(WiredTigerDatabase &db, bool defaultCommit) :
                _session(db),
                _defaultCommit(defaultCommit),
                _depth(0),
                _begun(false) {}

        virtual ~WiredTigerRecoveryUnit() {
            if (_defaultCommit) {
                commitUnitOfWork();
            } else if (_depth > 0) {
                WT_SESSION *s = _session.Get();
                int ret = s->rollback_transaction(s, NULL);
                invariant(ret == 0);
            }
        }

        virtual void beginUnitOfWork() {
            if (_depth++ > 0)
                return;
            WT_SESSION *s = _session.Get();
            int ret = s->begin_transaction(s, NULL);
            invariant(ret == 0);
            _begun = true;
        }

        virtual void commitUnitOfWork() {
            WT_SESSION *s = _session.Get();
            if (_begun) {
                int ret = s->commit_transaction(s, NULL);
                invariant(ret == 0);
                _begun = false;
            }
        }

        virtual void endUnitOfWork() {
            invariant(_depth > 0);
            if (--_depth > 0)
                return;
            commitUnitOfWork();
        }

        virtual bool commitIfNeeded(bool force = false) {
            if (!isCommitNeeded())
                return false;
            commitUnitOfWork();
            return true;
        }

        virtual bool awaitCommit() {
            return true;
        }

        virtual bool isCommitNeeded() const {
            return false;
        }

        virtual void registerChange(Change *) {}

        virtual void* writingPtr(void* data, size_t len) {
            return data;
        }

        virtual void syncDataAndTruncateJournal() {}

        WiredTigerSession& GetSession() { return _session; }

        static WiredTigerRecoveryUnit& Get(OperationContext *txn) {
            return *dynamic_cast<WiredTigerRecoveryUnit*>(txn->recoveryUnit());
        }

    private:
        WiredTigerSession _session;
        bool _defaultCommit;
        int _depth;
        bool _begun;
    };

}

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

#include "mongo/s/balancer_lock.h"
#include "mongo/s/type_settings.h"

namespace mongo {

    ScopedBalancerLock::ScopedBalancerLock(const ConnectionString& configLoc,
                                           const string& why,
                                           int lockTryIntervalMillis,
                                           unsigned long long lockTimeout,
                                           bool asProcess) :
            ScopedDistributedLock(configLoc,
                                  "balancer",
                                  why,
                                  lockTryIntervalMillis,
                                  lockTimeout,
                                  asProcess),
            _wasStopped(false)
    {
    }

    ScopedBalancerLock::~ScopedBalancerLock() {
        if (_wasStopped) {
            unlockBalancer();
        }
    }

    bool tryStopBalancer(const ConnectionString& configLoc, bool* wasStopped, string* errmsg) {

        scoped_ptr<ScopedDbConnection> connPtr(
                ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
        ScopedDbConnection& conn = *connPtr;

        BSONObj balancerDoc;
        *wasStopped = false;

        try {
            balancerDoc = conn->findOne(SettingsType::ConfigNS, BSON("_id" << "balancer"));
        }
        catch (const DBException& e) {
            if (errmsg) {
                *errmsg = str::stream() << "could not load balancer document (to stop)"
                                        << causedBy(e);
            }
            return false;
        }

        // TODO: Make cleaner below
        if (balancerDoc["stopped"].trueValue()) {
            conn.done();
            return true;
        }

        try {
            conn->update(SettingsType::ConfigNS,
                         BSON("_id" << "balancer"),
                         BSON("$set" << BSON("stopped" << true)),
                         true);
        }
        catch (const DBException& e) {
            if (errmsg) {
                *errmsg = str::stream() << "could not stop balancer" << causedBy(e);
            }
            return false;
        }

        *wasStopped = true;
        conn.done();
        return true;
    }

    bool tryStartBalancer(const ConnectionString& configLoc, string* errmsg) {

        scoped_ptr<ScopedDbConnection> connPtr(
                ScopedDbConnection::getInternalScopedDbConnection(configLoc, 30));
        ScopedDbConnection& conn = *connPtr;

        BSONObj balancerDoc;
        try {
            balancerDoc = conn->findOne(SettingsType::ConfigNS, BSON("_id" << "balancer"));
        }
        catch (const DBException& e) {
            if (errmsg) {
                *errmsg = str::stream() << "could not load balancer document (for restart)"
                                        << causedBy(e);
            }
            return false;
        }

        // TODO: Make cleaner below
        if (!balancerDoc["stopped"].trueValue()) {
            conn.done();
            return true;
        }

        try {
            conn->update(SettingsType::ConfigNS,
                         BSON("_id" << "balancer"),
                         BSON("$set" << BSON("stopped" << false)),
                         true);
        }
        catch (const DBException& e) {
            if (errmsg) {
                *errmsg = str::stream() << "could not restart balancer" << causedBy(e);
            }
            return false;
        }

        conn.done();
        return true;
    }

    bool ScopedBalancerLock::tryAcquireOnce(string* errMsg) {

        bool wasStoppedLast;
        bool stopped = tryStopBalancer(getConfigConnectionString(), &wasStoppedLast, errMsg);

        // If we ever stopped the balancer ourselves, record it here
        _wasStopped |= wasStoppedLast;

        if (!stopped) return false;

        return ScopedDistributedLock::tryAcquireOnce(errMsg);
    }

    void ScopedBalancerLock::unlockBalancer() {
        // Note: We try to undo our previous stopped state a few times, but we can't be sure
        // that we'll succeed.
        // TODO: Is there a better way to do this?
        string errMsg;
        bool started = false;
        for (int i = 0; i < 3; i++) {
            if (i > 0) sleepsecs(i);
            if (tryStartBalancer(getConfigConnectionString(), &errMsg)) {
                started = true;
                break;
            }
        }

        if (!started) {
            warning() << "error restarting balancer for '" << getLockWhy() << "'"
                      << causedBy(&errMsg);
        }
    }

} // namespace mongo

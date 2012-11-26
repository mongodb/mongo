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

#pragma once

#include "mongo/client/distlock.h"

namespace mongo {

    /**
     * Scoped wrapper for distributed balancer lock acquisition attempt.  Adds functionality
     * managing turning off the balancer at config.settings (if currently on), and restoring the
     * previous balancer state on object destruction.
     */
    class ScopedBalancerLock : public ScopedDistributedLock {
    public:

        ScopedBalancerLock(const ConnectionString& conn,
                           const string& why,
                           int lockTryIntervalMillis = 1000,
                           unsigned long long lockTimeout = 0,
                           bool asProcess = false);

        virtual ~ScopedBalancerLock();

        virtual bool tryAcquireOnce(string* errMsg);

        void unlockBalancer();

    private:
        bool _wasStopped;
    };
}

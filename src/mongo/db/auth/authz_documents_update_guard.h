/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {

    class AuthorizationManager;

    /*
     * Guard object for locking the lock that serializes all writes to the persistent authorization
     * documents.
     */
    class AuthzDocumentsUpdateGuard {
        MONGO_DISALLOW_COPYING(AuthzDocumentsUpdateGuard);
    public:
        explicit AuthzDocumentsUpdateGuard(AuthorizationManager* authzManager);
        ~AuthzDocumentsUpdateGuard();

        /**
         * Tries to acquire the global lock guarding modifications to all persistent data related
         * to authorization, namely the admin.system.users, admin.system.roles, and
         * admin.system.version collections.  This serializes all writers to the authorization
         * documents, but does not impact readers.
         * Returns whether or not it was successful at acquiring the lock.
         */
        bool tryLock(StringData why);

        /**
         * Releases the lock guarding modifications to persistent authorization data, which must
         * already be held.
         */
        void unlock();

    private:
        AuthorizationManager* _authzManager;
        // True if the Guard has locked the lock that guards modifications to authz documents.
        bool _lockedForUpdate;
    };

} // namespace mongo

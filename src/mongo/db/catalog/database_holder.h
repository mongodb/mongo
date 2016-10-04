/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <set>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {

class Database;
class OperationContext;

/**
 * Registry of opened databases.
 */
class DatabaseHolder {
public:
    DatabaseHolder() = default;

    /**
     * Retrieves an already opened database or returns NULL. Must be called with the database
     * locked in at least IS-mode.
     */
    Database* get(OperationContext* txn, StringData ns) const;

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet. Must be called with the database locked in X-mode.
     *
     * @param justCreated Returns whether the database was newly created (true) or it already
     *          existed (false). Can be NULL if this information is not necessary.
     */
    Database* openDb(OperationContext* txn, StringData ns, bool* justCreated = NULL);

    /**
     * Closes the specified database. Must be called with the database locked in X-mode.
     */
    void close(OperationContext* txn, StringData ns);

    /**
     * Closes all opened databases. Must be called with the global lock acquired in X-mode.
     *
     * @param result Populated with the names of the databases, which were closed.
     * @param force Force close even if something underway - use at shutdown
     */
    bool closeAll(OperationContext* txn, BSONObjBuilder& result, bool force);

    /**
     * Retrieves the names of all currently opened databases. Does not require locking, but it
     * is not guaranteed that the returned set of names will be still valid unless a global
     * lock is held, which would prevent database from disappearing or being created.
     */
    void getAllShortNames(std::set<std::string>& all) const {
        stdx::lock_guard<SimpleMutex> lk(_m);
        for (DBs::const_iterator j = _dbs.begin(); j != _dbs.end(); ++j) {
            all.insert(j->first);
        }
    }

private:
    typedef StringMap<Database*> DBs;

    mutable SimpleMutex _m;
    DBs _dbs;
};

DatabaseHolder& dbHolder();
}

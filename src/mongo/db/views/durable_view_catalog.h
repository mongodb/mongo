/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/stdx/functional.h"

namespace mongo {

class BSONObj;
class Database;
class NamespaceString;
class OperationContext;

/**
 * Interface for system.views collection operations associated with view catalog management.
 * Methods must be called from within a WriteUnitOfWork, and with the DBLock held.
 */
class DurableViewCatalog {
public:
    using Callback = stdx::function<void(const BSONObj& view)>;

    virtual void iterate(OperationContext* txn, Callback callback) = 0;
    virtual void insert(OperationContext* txn, const BSONObj& view) = 0;
    virtual void remove(OperationContext* txn, const NamespaceString& name) = 0;
};

/**
 * Actual implementation of DurableViewCatalog for use by the Database class.
 * Implements durability through database operations on the system.views collection.
 */
class DurableViewCatalogImpl final : public DurableViewCatalog {
public:
    explicit DurableViewCatalogImpl(Database* db) : _db(db) {}

    void iterate(OperationContext* txn, Callback callback);
    void insert(OperationContext* txn, const BSONObj& view);
    void remove(OperationContext* txn, const NamespaceString& name);

private:
    Database* const _db;
};
}  // namespace mongo

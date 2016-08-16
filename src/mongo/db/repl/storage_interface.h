/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class OperationContext;

namespace repl {

/**
 * Storage interface used by used by the ReplicationExecutor inside mongod for supporting
 * ReplicationExectutor's ability to take database locks.
 */
class StorageInterface {
public:
    virtual ~StorageInterface();

    /**
     * Creates an operation context for running database operations.
     */
    virtual OperationContext* createOperationContext() = 0;

    /**
     * Returns the configured maximum size of the oplog.
     *
     * Implementations are allowed to be "fuzzy" and delete documents when the actual size is
     * slightly above or below this, so callers should not rely on its exact value.
     */
    virtual StatusWith<size_t> getOplogMaxSize(OperationContext* txn,
                                               const NamespaceString& nss) = 0;

protected:
    StorageInterface();
};

}  // namespace repl
}  // namespace mongo

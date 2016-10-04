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

#include "mongo/db/range_deleter.h"

namespace mongo {

/**
 * This class implements the deleter methods to be used for a shard.
 */
struct RangeDeleterDBEnv : public RangeDeleterEnv {
    /**
     * Deletes the documents from the given range synchronously.
     *
     * The keyPattern will be used to determine the right index to use to perform
     * the deletion and it can be a prefix of an existing index. Caller is responsible
     * of making sure that both min and max is a prefix of keyPattern.
     *
     * Note that secondaryThrottle will be ignored if current process is not part
     * of a replica set.
     *
     * docsDeleted would contain the number of docs deleted if the deletion was successful.
     *
     * Does not throw Exceptions.
     */
    virtual bool deleteRange(OperationContext* txn,
                             const RangeDeleteEntry& taskDetails,
                             long long int* deletedDocs,
                             std::string* errMsg);

    /**
     * Gets the list of open cursors on a given namespace.
     */
    virtual void getCursorIds(OperationContext* txn,
                              StringData ns,
                              std::set<CursorId>* openCursors);
};
}

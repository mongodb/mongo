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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/commands/write_commands/batch.h"

namespace mongo {

    class BSONObjBuilder;
    class Client;
    class CurOp;
    class OpCounters;
    class OpDebug;
    struct LastError;

    /**
     * An instance of WriteBatchExecutor is an object capable of issuing a write batch.
     */
    class WriteBatchExecutor {
        MONGO_DISALLOW_COPYING(WriteBatchExecutor);
    public:
        WriteBatchExecutor(Client* client, OpCounters* opCounters, LastError* le);

        /**
         * Issues writes with requested write concern.
         * Returns true (and fills "result") if request properly formatted.
         * Returns false (and fills "errMsg") if not.
         */
        bool executeBatch(const WriteBatch& writeBatch, string* errMsg, BSONObjBuilder* result);

    private:
        /**
         * Issues writes in batch.  Fills "resultsArray" with write results.
         * Returns true iff all items in batch were issued successfully.
         */
        bool applyWriteBatch(const WriteBatch& writeBatch, BSONArrayBuilder* resultsArray);

        /**
         * Issues a single write.  Fills "results" with write result.
         * Returns true iff write item was issued sucessfully.
         */
        bool applyWriteItem(const string& ns,
                            const WriteBatch::WriteItem& writeItem,
                            BSONObjBuilder* results);

        //
        // Helpers to issue underlying write.
        // Returns true iff write item was issued sucessfully.
        //

        bool applyInsert(const string& ns,
                         const WriteBatch::WriteItem& writeItem,
                         CurOp* currentOp);

        bool applyUpdate(const string& ns,
                         const WriteBatch::WriteItem& writeItem,
                         CurOp* currentOp);

        bool applyDelete(const string& ns,
                         const WriteBatch::WriteItem& writeItem,
                         CurOp* currentOp);

        // Client object to issue writes on behalf of.
        // Not owned here.
        Client* _client;

        // OpCounters object to update.
        // Not owned here.
        OpCounters* _opCounters;

        // LastError object to use for preparing write results.
        // Not owned here.
        LastError* _le;
    };

} // namespace mongo

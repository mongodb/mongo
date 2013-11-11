/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/lasterror.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_error_detail.h"

// TODO: Remove post-2.6

namespace mongo {

    /**
     * Interface to execute a single safe write.
     */
    class SafeWriter {
    public:

        virtual ~SafeWriter() {
        }

        virtual void safeWrite( DBClientBase* conn,
                                const BatchItemRef& batchItem,
                                LastError* error ) = 0;

        // Helper exposed for testing
        static void fillLastError( const BSONObj& gleResult, LastError* error );
    };

    /**
     * Executes a batch write using safe writes.
     *
     * The actual safe write operation is done via an interface to allow testing the rest of the
     * aggregation functionality.
     */
    class BatchSafeWriter {
    public:

        BatchSafeWriter( SafeWriter* safeWriter ) :
            _safeWriter( safeWriter ) {
        }

        // Testable static dispatching method, defers to SafeWriter for actual writes over the
        // connection.
        void safeWriteBatch( DBClientBase* conn,
                             const BatchedCommandRequest& request,
                             BatchedCommandResponse* response );

        // Helper exposed for testing
        static bool isFailedOp( const LastError& error );

        // Helper exposed for testing
        static BatchedErrorDetail* lastErrorToBatchError( const LastError& error );

    private:

        SafeWriter* _safeWriter;
    };

}

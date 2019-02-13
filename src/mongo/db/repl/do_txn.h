/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#include "mongo/base/status.h"
#include "mongo/db/repl/oplog.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class OperationContext;

class DoTxn {
public:
    static constexpr StringData kPreconditionFieldName = "preCondition"_sd;
};

/**
 * Applies ops contained in 'doTxnCmd' and populates fields in 'result' to be returned to the
 * caller. The information contained in 'result' can be returned to the user if called as part
 * of the execution of an 'doTxn' command.
 *
 * The 'oplogApplicationMode' argument determines the semantics of the operations contained within
 * the given command object. This function may be called as part of a direct user invocation of the
 * 'doTxn' command, or as part of the application of an 'doTxn' oplog operation. In either
 * case, the mode can be set to determine how the internal ops are executed.
 */
Status doTxn(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& doTxnCmd,
             BSONObjBuilder* result);

}  // namespace mongo

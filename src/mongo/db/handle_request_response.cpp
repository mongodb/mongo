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

#include "mongo/db/handle_request_response.h"
#include "mongo/base/transaction_error.h"

namespace mongo {

BSONObj getErrorLabels(const OperationSessionInfoFromClient& sessionOptions,
                       const std::string& commandName,
                       ErrorCodes::Error code,
                       bool hasWriteConcernError) {
    BSONArrayBuilder labelArray;

    // Note that we only apply the TransientTxnError label if the "autocommit" field is present in
    // the session options. When present, "autocommit" will always be false, so we don't check its
    // value.
    if (sessionOptions.getAutocommit() &&
        isTransientTransactionError(code,
                                    hasWriteConcernError,
                                    commandName == "commitTransaction" ||
                                        commandName == "coordinateCommitTransaction")) {
        // An error code for which isTransientTransactionError() is true indicates a transaction
        // failure with no persistent side effects.
        labelArray << txn::TransientTxnErrorFieldName;
    }

    if (ErrorCodes::isNonResumableChangeStreamError(code)) {
        labelArray << "NonResumableChangeStreamError";
    }

    return (labelArray.arrSize() > 0) ? BSON("errorLabels" << labelArray.arr()) : BSONObj();
}

}  // namespace mongo

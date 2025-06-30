/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/server_parameter_insert_max_batch_size.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo {

int getInternalInsertMaxBatchSize() {
    const auto userSetInternalInsertMaxBatchSize = gUserSetInternalInsertMaxBatchSize.load();
    if (userSetInternalInsertMaxBatchSize != 0) {
        // If the user-set parameter is not 0, return the value that the user has set.
        return userSetInternalInsertMaxBatchSize;
    }

    // Return the default value. For v8.0, this should be 500 if the
    // gReplicateVectoredInsertsTransactionally feature flag is enabled, but 64 otherwise.
    const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    const bool replicateVectoredInsertsTransactionally = fcvSnapshot.isVersionInitialized() &&
        repl::feature_flags::gReplicateVectoredInsertsTransactionally.isEnabled(fcvSnapshot);
    return (replicateVectoredInsertsTransactionally ? kDefaultInternalInsertMaxBatchSizeFcv80
                                                    : kDefaultInternalInsertMaxBatchSizeFcv70);
}

}  // namespace mongo

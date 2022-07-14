/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace global_index {

/**
 * Handles the insertion of index entries to the global index collection during the cloning phase.
 */
class GlobalIndexInserter {
public:
    GlobalIndexInserter(NamespaceString nss,
                        StringData indexName,
                        UUID indexUUID,
                        std::shared_ptr<executor::TaskExecutor> executor);

    /**
     * Performs the necessary checks and bookkeeping on inserting a new global index entry under a
     * transaction.
     */
    void processDoc(OperationContext* opCtx,
                    const BSONObj& indexKeyValues,
                    const BSONObj& documentKey);

private:
    NamespaceString _skipIdNss();
    NamespaceString _globalIndexNss();

    const NamespaceString _nss;
    const std::string _indexName;
    const UUID _indexUUID;

    std::shared_ptr<executor::TaskExecutor> _executor;
};

}  // namespace global_index
}  // namespace mongo

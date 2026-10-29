// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/writer_util.h"

#include "mongo/db/query/write_ops/write_ops_gen.h"

namespace mongo {

BatchedCommandRequest makeInsertCommand(const NamespaceString& outputNs,
                                        bool bypassDocumentValidation) {
    write_ops::InsertCommandRequest insertOp(outputNs);
    insertOp.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(bypassDocumentValidation);
        return wcb;
    }());
    return BatchedCommandRequest(std::move(insertOp));
}

}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

[[MONGO_MOD_NEEDS_REPLACEMENT]] constexpr inline auto kRawDataFieldName = "rawData"sv;

/**
 * Returns a settable boolean indicating whether the given operation context is performing a "raw
 * data" operation. When being run on a collection type that stores its data in a different format
 * from that in which users interact with, a "raw data" operation will operate directly on the
 * format in which it is stored.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] bool& isRawDataOperation(OperationContext*);

/**
 * RAII guard that temporarily overrides the "raw data" flag on the given operation context. The
 * original value is saved on construction and restored on destruction, including when the scope is
 * exited via an exception.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ScopedRawDataOperation {
public:
    ScopedRawDataOperation(OperationContext* opCtx, bool value)
        : _opCtx(opCtx), _original(isRawDataOperation(opCtx)) {
        isRawDataOperation(_opCtx) = value;
    }

    ScopedRawDataOperation(const ScopedRawDataOperation&) = delete;
    ScopedRawDataOperation& operator=(const ScopedRawDataOperation&) = delete;

    ~ScopedRawDataOperation() {
        isRawDataOperation(_opCtx) = _original;
    }

private:
    OperationContext* const _opCtx;
    const bool _original;
};

/**
 * Returns the rewritten command object, replacing the collection name with the one provided.
 */
template <class CommandRequest>
[[MONGO_MOD_NEEDS_REPLACEMENT]] BSONObj rewriteCommandForRawDataOperation(const BSONObj& cmd,
                                                                          std::string_view coll) {
    BSONObjBuilder builder{cmd.objsize()};
    for (auto&& [fieldName, elem] : cmd) {
        if (fieldName == CommandRequest::kCommandName) {
            builder.append(fieldName, coll);
        } else {
            builder.append(elem);
        }
    }
    return builder.obj();
}

}  // namespace mongo

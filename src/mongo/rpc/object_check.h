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

#pragma once

#include <algorithm>

#include "mongo/base/data_type_validated.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/wire_message_payload.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"

// We do not use the rpc namespace here so we can specialize Validator.
namespace mongo {
class BSONObj;
class Status;

/**
 * A validator for BSON objects. The implementation will validate the input object
 * if validation is enabled, or return Status::OK() otherwise.
 */
template <>
struct Validator<BSONObj> {
    inline static Status validateLoad(const char* ptr, size_t length) {
        if (!serverGlobalParams.objcheck) {
            return Status::OK();
        }

        auto status = validateBSON(ptr, length);
        if (serverGlobalParams.crashOnInvalidBSONError && !status.isOK()) {
            std::string msg = "Invalid BSON was received: " + status.toString() +
                // Using std::min with length so we do not max anything out in case the corruption
                // is in the size of the object. The hex dump will be longer if needed.
                ", beginning 5000 characters: " + std::string(ptr, std::min(length, (size_t)5000)) +
                ", length: " + std::to_string(length) +
                // Using std::min with hex dump length, too, to ensure we do not throw in hexdump()
                // because of exceeded length and miss out on the core dump of the fassert below.
                ", hex dump: " + hexdump(ptr, std::min(length, (size_t)(1000000 - 1)));
            Status builtStatus(ErrorCodes::InvalidBSON, redact(msg));
            fassertFailedWithStatus(50761, builtStatus);
        }
        return status;
    }

    static Status validateStore(const BSONObj& toStore);
};

/**
 * A validator for WireMessagePayload objects. The implementation will validate the input object
 * in the same way as a regular BSONObj. This type is only needed because it is required to be
 * present by DataType::Handler<WireMessagePayload>.
 */
template <>
struct Validator<WireMessagePayload> {
    inline static Status validateLoad(const char* ptr, size_t length) {
        // Reuse BSONObj validation logic.
        return Validator<BSONObj>::validateLoad(ptr, length);
    }

    static Status validateStore(const WireMessagePayload& toStore) {
        invariant(
            false,
            "Validator<WireMessagePayload> must only be used to read incoming op_msg requests, and "
            "never as a serialization sink.");

        MONGO_COMPILER_UNREACHABLE;
    }
};

}  // namespace mongo

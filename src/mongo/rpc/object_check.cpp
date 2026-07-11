// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/object_check.h"

#include "mongo/base/data_type.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace mongo {
namespace rpc {

Status checkBSONObj(const char* ptr, size_t length) {
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
            ", hex dump: " + hexdump(ptr, std::min(length, kHexDumpMaxSize - 1));
        Status builtStatus(ErrorCodes::InvalidBSON, redact(msg));
        fassertFailedWithStatus(50761, builtStatus);
    }
    return status;
}

}  // namespace rpc

Status DataType::Handler<rpc::ValidatedBSONObj>::load(rpc::ValidatedBSONObj* vt,
                                                      const char* ptr,
                                                      size_t length,
                                                      size_t* advanced,
                                                      std::ptrdiff_t debugOffset) {
    if (auto e = rpc::checkBSONObj(ptr, length); !e.isOK()) {
        return e;
    }
    size_t pos = 0;
    BSONObj obj;
    if (auto e = DataType::load(vt ? &obj : nullptr, ptr, length, &pos, debugOffset); !e.isOK()) {
        return e;
    }
    if (vt) {
        *vt = rpc::ValidatedBSONObj{std::move(obj)};
    }
    if (advanced) {
        *advanced = pos;
    }
    return Status::OK();
}

Status DataType::Handler<rpc::ValidatedBSONObj>::store(const rpc::ValidatedBSONObj& vt,
                                                       char* ptr,
                                                       size_t length,
                                                       size_t* advanced,
                                                       std::ptrdiff_t debugOffset) {
    const BSONObj& obj = static_cast<const BSONObj&>(vt);
    size_t pos = 0;
    if (auto e = DataType::store(obj, ptr, length, &pos, debugOffset); !e.isOK()) {
        return e;
    }
    if (advanced) {
        *advanced = pos;
    }
    return Status::OK();
}

rpc::ValidatedBSONObj DataType::Handler<rpc::ValidatedBSONObj>::defaultConstruct() {
    return {};
}

}  // namespace mongo

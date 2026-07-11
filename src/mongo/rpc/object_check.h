// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace rpc {

/**
 * Calls validateBSON, but controllable by server parameters.
 *
 *  - `objcheck`:
 *     controls whether the check is actually performed.
 *
 *  - `crashOnInvalidBSONError`:
 *     controls whether a verbose fassert is triggered on failure.
 */
Status checkBSONObj(const char* ptr, size_t length);

/**
 * You can use ValidatedBSONObj in a DataRange (and associated types)
 *
 * Example:
 *
 *     DataRangeCursor drc(buf, buf_end);
 *     ValidatedBSONObj vobj;
 *     auto status = drc.readAndAdvance(&vobj);
 *     if (status.isOK()) {
 *         BSONObj obj{vobj};
 *         // use obj
 *         // ....
 *     }
 *
 * The implementation will validate the input object
 * if validation is enabled, or return Status::OK() otherwise.
 */
class ValidatedBSONObj {
public:
    ValidatedBSONObj() = default;
    explicit ValidatedBSONObj(BSONObj value) : _val{std::move(value)} {}

    explicit operator const BSONObj&() const& {
        return _val;
    }

    explicit operator BSONObj() && {
        return std::move(_val);
    }

private:
    BSONObj _val;
};
}  // namespace rpc

template <>
struct DataType::Handler<rpc::ValidatedBSONObj> {
    static Status load(rpc::ValidatedBSONObj* vt,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debugOffset);

    static Status store(const rpc::ValidatedBSONObj& vt,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debugOffset);

    static rpc::ValidatedBSONObj defaultConstruct();
};
}  // namespace mongo

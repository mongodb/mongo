// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/signed_logical_time.h"

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/crypto/hash_block.h"

#include <boost/optional/optional.hpp>

namespace mongo {

std::string SignedLogicalTime::toString() const {
    StringBuilder buf;
    auto proof = _proof.get_value_or(TimeProof());
    buf << _time.toString() << "|" << proof.toString();
    return buf.str();
}

}  // namespace mongo

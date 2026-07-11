// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/election_reason_counter.h"

namespace mongo {
namespace repl {

ElectionReasonCounter ElectionReasonCounter::parse(const IDLParserContext& ctxt,
                                                   const BSONObj& bsonObject) {
    this->parseProtected(bsonObject, ctxt);
    return *this;
}

}  // namespace repl
}  // namespace mongo

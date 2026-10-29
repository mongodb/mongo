// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/election_reason_counter_parser.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/idl/idl_parser.h"

#include <string_view>

namespace mongo {
namespace repl {


ElectionReasonCounter parseElectionReasonCounter(const BSONElement& element) {
    ElectionReasonCounter counter;
    IDLParserContext ctxt = IDLParserContext("ElectionReasonCounter");

    return counter.parse(ctxt, element.Obj());
}

void serializeElectionReasonCounterToBSON(ElectionReasonCounter counter,
                                          std::string_view fieldName,
                                          BSONObjBuilder* builder) {
    builder->append(fieldName, counter.toBSON());
}

}  // namespace repl
}  // namespace mongo

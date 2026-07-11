// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/election_reason_counter_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

/**
 * Wrapper around the IDL struct ElectionReasonCounterBase that has increment methods.
 */
class ElectionReasonCounter : public ElectionReasonCounterBase {
public:
    using ElectionReasonCounterBase::getCalled;
    using ElectionReasonCounterBase::getSuccessful;
    using ElectionReasonCounterBase::setCalled;
    using ElectionReasonCounterBase::setSuccessful;

    void incrementCalled() {
        setCalled(getCalled() + 1);
    }

    void incrementSuccessful() {
        setSuccessful(getSuccessful() + 1);
    }

    ElectionReasonCounter parse(const IDLParserContext& ctxt, const BSONObj& bsonObject);
};

}  // namespace repl
}  // namespace mongo

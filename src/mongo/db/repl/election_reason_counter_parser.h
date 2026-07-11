// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/election_reason_counter.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace repl {

ElectionReasonCounter parseElectionReasonCounter(const BSONElement& element);

void serializeElectionReasonCounterToBSON(ElectionReasonCounter counter,
                                          std::string_view fieldName,
                                          BSONObjBuilder* builder);

}  // namespace repl
}  // namespace mongo

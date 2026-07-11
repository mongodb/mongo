// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

bool runAggregationMapReduce(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmd,
                             BSONObjBuilder& result,
                             boost::optional<ExplainOptions::Verbosity> verbosity);

}  // namespace mongo

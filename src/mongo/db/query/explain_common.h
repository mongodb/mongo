// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

/**
 * Namespace for static methods that are shared between explain on mongod and on mongos.
 */
namespace mongo::explain_common {

/**
 * Adds the 'serverInfo' explain section to the BSON object being built by 'out'.
 *
 * This section include the host, port, version, and gitVersion.
 */
void generateServerInfo(BSONObjBuilder* out);

/**
 * Adds the 'serverParameters' explain section to the BSON object being built by 'out'.
 *
 * This section includes various server-wide internal limits/knobs.
 */
void generateServerParameters(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              BSONObjBuilder* out);

/**
 * Adds the 'queryKnobs' explain section to the BSON object being built by 'out'.
 *
 * Contains every query knob with a non-default source, keyed by wire name with nested "value" and
 * "source" fields. setParameter overrides appear with source "setParameter"; QuerySettings
 * overrides appear with source "querySettings" (even when the value equals the compiled-in
 * default). Knobs at their default source are omitted. The section is absent entirely when no knob
 * has been overridden.
 */
void generateQueryKnobs(const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONObjBuilder* out);

/**
 * Adds the 'queryShapeHash' value to the BSON object being built by 'out'.
 */
void generateQueryShapeHash(OperationContext* opCtx, BSONObjBuilder* out);

/**
 * Adds the 'peakTrackedMemBytes' value to the BSON object being built by 'out'.
 */
void generatePeakTrackedMemBytes(const OperationContext* opCtx, BSONObjBuilder* out);

/**
 * Conditionally appends a BSONObj to 'bob' depending on whether or not the maximum user size for a
 * BSON object will be exceeded.
 */
bool appendIfRoom(const BSONObj& toAppend, std::string_view fieldName, BSONObjBuilder* out);

/**
 * Conditionally appends a BSONArray to 'bob' depending on whether or not the maximum user size for
 * a BSON object will be exceeded.
 */
bool appendIfRoom(const BSONArray& toAppend, std::string_view fieldName, BSONObjBuilder* out);
}  // namespace mongo::explain_common

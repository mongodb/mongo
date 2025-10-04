/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

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
bool appendIfRoom(const BSONObj& toAppend, StringData fieldName, BSONObjBuilder* out);

}  // namespace mongo::explain_common

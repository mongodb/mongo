/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/rpc/metadata/client_metadata.h"

namespace mongo::telemetry {

/**
 * An abstract base class to handle query shapification for telemetry. Each request type should
 * define its own shapification strategy in its implementation of makeTelemetryKey(), and then a
 * request should be registered with telemetry via telemetry::registerRequest(RequestShapifier).
 */
class RequestShapifier {
public:
    virtual ~RequestShapifier() = default;
    virtual BSONObj makeTelemetryKey(
        const SerializationOptions& opts,
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const = 0;

protected:
    RequestShapifier(OperationContext* opCtx,
                     const boost::optional<std::string> applicationName = boost::none)
        : _applicationName(applicationName) {
        if (!_applicationName) {
            if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
                _applicationName = metadata->getApplicationName().toString();
            }
        }
    }

    boost::optional<std::string> _applicationName;
};
}  // namespace mongo::telemetry

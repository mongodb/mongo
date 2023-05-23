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
#include "mongo/db/api_parameters.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include <memory>

namespace mongo::query_stats {

/**
 * An abstract base class to handle query shapification for queryStats. Each request type should
 * define its own shapification strategy in its implementation of makeQueryStatsKey(), and then a
 * request should be registered with queryStats via query_stats::registerRequest().
 */
class RequestShapifier {
public:
    virtual ~RequestShapifier() = default;

    /**
     * makeQueryStatsKey generates the query stats key representative of the specific request's
     * payload. If there exists an ExpressionContext set up to parse and evaluate the request,
     * makeQueryStatsKey should be called with that ExpressionContext. If not, you can call the
     * overload that accepts the OperationContext and will construct a minimally-acceptable
     * ExpressionContext for the sake of generating the key.
     */
    virtual BSONObj makeQueryStatsKey(const SerializationOptions& opts,
                                      OperationContext* opCtx) const = 0;
    virtual BSONObj makeQueryStatsKey(
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
        if (const auto& comment = opCtx->getComment()) {
            BSONObjBuilder commentBuilder;
            commentBuilder.append(*comment);
            _commentObj = commentBuilder.obj();
            _comment = _commentObj.firstElement();
        }

        _apiParams = std::make_unique<APIParameters>(APIParameters::get(opCtx));

        if (!ReadPreferenceSetting::get(opCtx).toInnerBSON().isEmpty() &&
            !ReadPreferenceSetting::get(opCtx).usedDefaultReadPrefValue()) {
            _readPreference = boost::make_optional(ReadPreferenceSetting::get(opCtx).toInnerBSON());
        }
    }

    boost::optional<std::string> _applicationName;
    std::unique_ptr<APIParameters> _apiParams;
    BSONObj _commentObj;
    boost::optional<BSONElement> _comment = boost::none;
    boost::optional<BSONObj> _readPreference = boost::none;
};
}  // namespace mongo::query_stats

/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"

namespace mongo {

/**
 * Represents a resolved definition, composed of a base collection namespace and a pipeline
 * built from one or more views.
 */
class ResolvedView {
public:
    ResolvedView(const NamespaceString& collectionNs, const std::vector<BSONObj>& pipeline)
        : _namespace(collectionNs), _pipeline(pipeline) {}

    /**
     * Returns whether 'commandResponseObj' contains a CommandOnShardedViewNotSupportedOnMongod
     * error and resolved view definition.
     */
    static bool isResolvedViewErrorResponse(BSONObj commandResponseObj);

    static ResolvedView fromBSON(BSONObj commandResponseObj);

    /**
     * Convert an aggregation command on a view to the equivalent command against the view's
     * underlying collection.
     */
    AggregationRequest asExpandedViewAggregation(const AggregationRequest& aggRequest) const;

    const NamespaceString& getNamespace() const {
        return _namespace;
    }

    const std::vector<BSONObj>& getPipeline() const {
        return _pipeline;
    }

private:
    NamespaceString _namespace;
    std::vector<BSONObj> _pipeline;
};

}  // namespace mongo

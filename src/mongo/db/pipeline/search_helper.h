/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

/**
 * A class that contains any functions needed to run $seach queries when the enterprise module
 * is compiled in. The enterprise module will override these functions, these are just stubs.
 */
class SearchDefaultHelperFunctions {
public:
    virtual ~SearchDefaultHelperFunctions() {}

    /**
     * Any access of $$SEARCH_META is invalid without enterprise.
     */
    virtual void assertSearchMetaAccessValid(const Pipeline::SourceContainer& pipeline,
                                             ExpressionContext* expCtx);

    /**
     * Injects shard filterer for $_internalSearchIdLookup stage on shard only. This method is not
     * invoked for inner collection in $lookup, for instance, only when expanded pipeline is passed
     * to the specific shard.
     */
    virtual void injectSearchShardFiltererIfNeeded(Pipeline* pipeline){};

    /**
     * Check to see if in the current environment an additional pipeline needs to be run by the
     * aggregation command to generate metadata results. Either returns the additional pipeline
     * or nullptr if no pipeline is necessary.
     *
     * This can modify the passed in pipeline but does not take ownership of it.
     */
    virtual std::unique_ptr<Pipeline, PipelineDeleter> generateMetadataPipelineForSearch(
        OperationContext* opCtx,
        boost::intrusive_ptr<ExpressionContext> expCtx,
        const AggregateCommandRequest& request,
        Pipeline* origPipeline,
        boost::optional<UUID> uuid) {
        return nullptr;
    }
};

/**
 * A 'ServiceContext' decorator that allows enterprise to set its own version of the above class.
 */
extern ServiceContext::Decoration<std::unique_ptr<SearchDefaultHelperFunctions>> getSearchHelpers;

}  // namespace mongo

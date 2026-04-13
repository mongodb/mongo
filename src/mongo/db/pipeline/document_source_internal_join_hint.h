/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/optimizer/join/hint.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DEFINE_LITE_PARSED_STAGE_NO_TIMESERIES_DERIVED(InternalJoinHint);

/**
 * An internal stage available for testing. This allows us to specify a join order (or some
 * constraints on what the order/method/plan shape/enumeration strategy should be).
 *
 * See join_ordering::EnumerationStrategy for what is possible.
 */
class DocumentSourceInternalJoinHint final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalJoinHint"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    DocumentSourceInternalJoinHint(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   join_ordering::EnumerationStrategy&& strategy)
        : DocumentSource(kStageName, expCtx), _joinStrategy(std::move(strategy)) {}

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        auto constraints = StageConstraints{StreamType::kBlocking,
                                            PositionRequirement::kFirst,
                                            HostTypeRequirement::kLocalOnly,
                                            DiskUseRequirement::kNoDiskUse,
                                            FacetRequirement::kNotAllowed,
                                            TransactionRequirement::kNotAllowed,
                                            LookupRequirement::kNotAllowed,
                                            UnionRequirement::kNotAllowed,
                                            ChangeStreamRequirement::kDenylist};
        constraints.canRunOnTimeseries = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    join_ordering::EnumerationStrategy getStrategy() const {
        return _joinStrategy;
    }

private:
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    join_ordering::EnumerationStrategy _joinStrategy;
};

}  // namespace mongo

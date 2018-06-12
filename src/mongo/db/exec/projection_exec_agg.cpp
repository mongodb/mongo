/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/projection_exec_agg.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"

namespace mongo {

class ProjectionExecAgg::ProjectionExecutor {
public:
    using ParsedAggregationProjection = parsed_aggregation_projection::ParsedAggregationProjection;
    using ProjectionParseMode = ParsedAggregationProjection::ProjectionParseMode;
    using TransformerType = TransformerInterface::TransformerType;

    ProjectionExecutor(BSONObj projSpec) {
        // Construct a dummy ExpressionContext for ParsedAggregationProjection. It's OK to set the
        // ExpressionContext's OperationContext and CollatorInterface to 'nullptr' here; since we
        // ban computed fields from the projection, the ExpressionContext will never be used.
        boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(nullptr, nullptr));
        _projection = ParsedAggregationProjection::create(
            expCtx, projSpec, ProjectionParseMode::kBanComputedFields);
    }

    ProjectionType getType() const {
        return (_projection->getType() == TransformerType::kInclusionProjection
                    ? ProjectionType::kInclusionProjection
                    : ProjectionType::kExclusionProjection);
    }

    BSONObj applyProjection(BSONObj inputDoc) const {
        return _projection->applyTransformation(Document{inputDoc}).toBson();
    }

private:
    std::unique_ptr<ParsedAggregationProjection> _projection;
};

// ProjectionExecAgg's constructor and destructor are defined here, at a point where the
// implementation of ProjectionExecutor is known, so that std::unique_ptr can be used with the
// forward-declared ProjectionExecutor class.
ProjectionExecAgg::ProjectionExecAgg(BSONObj projSpec, std::unique_ptr<ProjectionExecutor> exec)
    : _exec(std::move(exec)), _projSpec(std::move(projSpec)){};

ProjectionExecAgg::~ProjectionExecAgg() = default;

std::unique_ptr<ProjectionExecAgg> ProjectionExecAgg::create(BSONObj projSpec) {
    return std::unique_ptr<ProjectionExecAgg>(
        new ProjectionExecAgg(projSpec, std::make_unique<ProjectionExecutor>(projSpec)));
}

ProjectionExecAgg::ProjectionType ProjectionExecAgg::getType() const {
    return _exec->getType();
}

BSONObj ProjectionExecAgg::applyProjection(BSONObj inputDoc) const {
    return _exec->applyProjection(inputDoc);
}
}  // namespace mongo

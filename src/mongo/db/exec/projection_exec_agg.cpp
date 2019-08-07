/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/projection_exec_agg.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"
#include "mongo/db/query/projection_policies.h"

namespace mongo {

class ProjectionExecAgg::ProjectionExecutor {
public:
    using ParsedAggregationProjection = parsed_aggregation_projection::ParsedAggregationProjection;

    using TransformerType = TransformerInterface::TransformerType;

    ProjectionExecutor(BSONObj projSpec,
                       DefaultIdPolicy defaultIdPolicy,
                       ArrayRecursionPolicy arrayRecursionPolicy) {
        // Construct a dummy ExpressionContext for ParsedAggregationProjection. It's OK to set the
        // ExpressionContext's OperationContext and CollatorInterface to 'nullptr' here; since we
        // ban computed fields from the projection, the ExpressionContext will never be used.
        boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(nullptr, nullptr));

        // Default projection behaviour is to include _id if the projection spec omits it. If the
        // caller has specified that we should *exclude* _id by default, do so here. We translate
        // DefaultIdPolicy to ProjectionPolicies::DefaultIdPolicy in order to avoid exposing
        // internal aggregation types to the query system.
        const auto idPolicy = (defaultIdPolicy == ProjectionExecAgg::DefaultIdPolicy::kIncludeId
                                   ? ProjectionPolicies::DefaultIdPolicy::kIncludeId
                                   : ProjectionPolicies::DefaultIdPolicy::kExcludeId);

        // By default, $project will recurse through nested arrays. If the caller has specified that
        // it should not, we inhibit it from doing so here. We separate this class' internal enum
        // ArrayRecursionPolicy from ProjectionPolicies::ArrayRecursionPolicy in order to avoid
        // exposing aggregation types to the query system.
        const auto recursionPolicy =
            (arrayRecursionPolicy == ArrayRecursionPolicy::kRecurseNestedArrays
                 ? ProjectionPolicies::ArrayRecursionPolicy::kRecurseNestedArrays
                 : ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays);

        // Inclusion projections permit computed fields by default, so we must explicitly ban them.
        // Computed fields are implicitly banned for exclusions.
        const auto computedFieldsPolicy =
            ProjectionPolicies::ComputedFieldsPolicy::kBanComputedFields;

        // Create a ProjectionPolicies object, to be populated based on the passed arguments.
        ProjectionPolicies projectionPolicies{idPolicy, recursionPolicy, computedFieldsPolicy};

        // Construct a ParsedAggregationProjection for the given projection spec and policies.
        _projection = ParsedAggregationProjection::create(expCtx, projSpec, projectionPolicies);

        // For an inclusion, record the exhaustive set of fields retained by the projection.
        if (getType() == ProjectionType::kInclusionProjection) {
            DepsTracker depsTracker;
            _projection->addDependencies(&depsTracker);
            for (auto&& field : depsTracker.fields)
                _exhaustivePaths.insert(FieldRef{field});
        }
    }

    const std::set<FieldRef>& getExhaustivePaths() const {
        return _exhaustivePaths;
    }

    ProjectionType getType() const {
        return (_projection->getType() == TransformerType::kInclusionProjection
                    ? ProjectionType::kInclusionProjection
                    : ProjectionType::kExclusionProjection);
    }

    BSONObj applyProjection(BSONObj inputDoc) const {
        return applyTransformation(Document{inputDoc}).toBson();
    }

    bool applyProjectionToOneField(StringData field) const {
        MutableDocument doc;
        const FieldPath f{field};
        doc.setNestedField(f, Value(1.0));
        const Document transformedDoc = applyTransformation(doc.freeze());
        return !transformedDoc.getNestedField(f).missing();
    }

    stdx::unordered_set<std::string> applyProjectionToFields(
        const stdx::unordered_set<std::string>& fields) const {
        stdx::unordered_set<std::string> out;

        for (const auto& field : fields) {
            if (applyProjectionToOneField(field)) {
                out.insert(field);
            }
        }

        return out;
    }

private:
    Document applyTransformation(Document inputDoc) const {
        return _projection->applyTransformation(inputDoc);
    }

    std::unique_ptr<ParsedAggregationProjection> _projection;
    std::set<FieldRef> _exhaustivePaths;
};

// ProjectionExecAgg's constructor and destructor are defined here, at a point where the
// implementation of ProjectionExecutor is known, so that std::unique_ptr can be used with the
// forward-declared ProjectionExecutor class.
ProjectionExecAgg::ProjectionExecAgg(BSONObj projSpec, std::unique_ptr<ProjectionExecutor> exec)
    : _exec(std::move(exec)), _projSpec(std::move(projSpec)){};

ProjectionExecAgg::~ProjectionExecAgg() = default;

std::unique_ptr<ProjectionExecAgg> ProjectionExecAgg::create(BSONObj projSpec,
                                                             DefaultIdPolicy defaultIdPolicy,
                                                             ArrayRecursionPolicy recursionPolicy) {
    return std::unique_ptr<ProjectionExecAgg>(new ProjectionExecAgg(
        projSpec,
        std::make_unique<ProjectionExecutor>(projSpec, defaultIdPolicy, recursionPolicy)));
}

ProjectionExecAgg::ProjectionType ProjectionExecAgg::getType() const {
    return _exec->getType();
}

BSONObj ProjectionExecAgg::applyProjection(BSONObj inputDoc) const {
    return _exec->applyProjection(inputDoc);
}

bool ProjectionExecAgg::applyProjectionToOneField(StringData field) const {
    return _exec->applyProjectionToOneField(field);
}

stdx::unordered_set<std::string> ProjectionExecAgg::applyProjectionToFields(
    const stdx::unordered_set<std::string>& fields) const {
    return _exec->applyProjectionToFields(fields);
}

const std::set<FieldRef>& ProjectionExecAgg::getExhaustivePaths() const {
    return _exec->getExhaustivePaths();
}
}  // namespace mongo

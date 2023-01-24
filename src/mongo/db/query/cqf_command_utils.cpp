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

#include "mongo/db/query/cqf_command_utils.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/exclusion_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_current_op.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_densify.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_geo_near_cursor.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"
#include "mongo/db/pipeline/document_source_internal_compute_geo_near_distance.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_list_cached_and_active_users.h"
#include "mongo/db/pipeline/document_source_list_catalog.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/document_source_operation_metrics.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sample.h"
#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_streaming_group.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/document_source_telemetry.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry_mongod.h"
#include "mongo/db/pipeline/visitors/document_source_walker.h"
#include "mongo/db/pipeline/visitors/transformer_interface_walker.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/document_source_merge_cursors.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery
namespace mongo {

using namespace optimizer;

namespace {

/**
 * Visitor that is responsible for indicating whether a MatchExpression is eligible for Bonsai by
 * setting the '_eligible' member variable. Expressions which are "test-only" and not officially
 * supported should set _eligible to false.
 */
class ABTMatchExpressionVisitor : public MatchExpressionConstVisitor {
public:
    ABTMatchExpressionVisitor(bool& eligible) : _eligible(eligible) {}

    void visit(const LTEMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const LTMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const EqualityMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const GTEMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const GTMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const InMatchExpression* expr) override {
        assertSupportedPathExpression(expr);

        // $in over a regex predicate is not supported.
        if (!expr->getRegexes().empty()) {
            _eligible = false;
        }
    }
    void visit(const ExistsMatchExpression* expr) override {
        assertSupportedPathExpression(expr);
    }
    void visit(const AndMatchExpression* expr) override {}
    void visit(const OrMatchExpression* expr) override {}

    void visit(const GeoMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const GeoNearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalBucketGeoWithinMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprGTMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprGTEMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprLTMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprLTEMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataSubTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaCondMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaFmodMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxLengthMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinLengthMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaObjectMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaRootDocEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaXorMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const ModMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const NorMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const NotMatchExpression* expr) override {}

    void visit(const RegexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const SizeMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TextMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TextNoOpMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TwoDPtInAnnulusExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const WhereMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const WhereNoOpMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAllClearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAllSetMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAnyClearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAnySetMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TypeMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const AlwaysFalseMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const AlwaysTrueMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const ExprMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) {
        _eligible = false;
    }

    void assertSupportedPathExpression(const PathMatchExpression* expr) {
        if (FieldRef(expr->path()).hasNumericPathComponents())
            _eligible = false;
    }

    bool& _eligible;
};

class ABTUnsupportedAggExpressionVisitor : public ExpressionConstVisitor {
public:
    ABTUnsupportedAggExpressionVisitor(bool& eligible) : _eligible(eligible) {}

    void visit(const ExpressionConstant* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionAbs* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionAdd* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionAllElementsTrue* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionAnd* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionAnyElementTrue* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArray* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArrayElemAt* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionBitAnd* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionBitOr* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionBitXor* expr) override final {
        unsupportedExpression();
    }
    void visit(const ExpressionBitNot* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFirst* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionLast* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionObjectToArray* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArrayToObject* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionBsonSize* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionCeil* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionCoerceToBool* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionCompare* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionConcat* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionConcatArrays* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionCond* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateFromString* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateFromParts* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateDiff* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateToParts* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateToString* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateTrunc* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDivide* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionExp* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFieldPath* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFilter* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFloor* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIfNull* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIn* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIndexOfArray* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIndexOfBytes* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIndexOfCP* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIsNumber* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionLet* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionLn* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionLog* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionLog10* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalFLEEqual* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalFLEBetween* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMap* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMeta* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMod* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMultiply* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionNot* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionObject* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionOr* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionPow* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRange* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionReduce* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionReplaceOne* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionReplaceAll* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSetDifference* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSetEquals* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSetIntersection* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSetIsSubset* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSetUnion* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSize* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionReverseArray* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSortArray* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSlice* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIsArray* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalFindAllValuesAtPath* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRound* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSplit* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSqrt* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionStrcasecmp* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSubstrBytes* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSubstrCP* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionStrLenBytes* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionBinarySize* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionStrLenCP* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSubtract* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSwitch* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTestApiVersion* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionToLower* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionToUpper* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTrim* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTrunc* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionType* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionZip* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionConvert* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRegexFind* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRegexFindAll* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRegexMatch* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionCosine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTangent* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArcCosine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArcSine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArcTangent* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionArcTangent2* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHyperbolicArcTangent* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHyperbolicArcCosine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHyperbolicArcSine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHyperbolicTangent* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHyperbolicCosine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHyperbolicSine* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDegreesToRadians* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRadiansToDegrees* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDayOfMonth* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDayOfWeek* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDayOfYear* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionHour* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMillisecond* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMinute* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionMonth* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSecond* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionWeek* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIsoWeekYear* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIsoDayOfWeek* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionIsoWeek* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionYear* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTests::Testable* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalJsEmit* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalFindSlice* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalFindPositional* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalFindElemMatch* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionFunction* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionRandom* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionToHashedIndexKey* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateAdd* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionDateSubtract* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionSetField* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionGetField* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTsSecond* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionTsIncrement* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalOwningShard* expr) override final {
        unsupportedExpression();
    }

    void visit(const ExpressionInternalIndexKey* expr) override final {
        unsupportedExpression();
    }

private:
    void unsupportedExpression() {
        _eligible = false;
    }

    bool& _eligible;
};

class ABTTransformerVisitor : public TransformerInterfaceConstVisitor {
public:
    ABTTransformerVisitor(bool& eligible) : _eligible(eligible) {}

    void visit(const projection_executor::ExclusionProjectionExecutor* transformer) override {
        checkUnsupportedInclusionExclusion(transformer);
    }

    void visit(const projection_executor::InclusionProjectionExecutor* transformer) override {
        checkUnsupportedInclusionExclusion(transformer);
    }

    void visit(const projection_executor::AddFieldsProjectionExecutor* transformer) override {
        unsupportedTransformer(transformer);
    }

    void visit(const GroupFromFirstDocumentTransformation* transformer) override {
        unsupportedTransformer(transformer);
    }

    void visit(const ReplaceRootTransformation* transformer) override {
        unsupportedTransformer(transformer);
    }

private:
    void unsupportedTransformer(const TransformerInterface* transformer) {
        _eligible = false;
    }

    template <typename T>
    void checkUnsupportedInclusionExclusion(const T* transformer) {
        OrderedPathSet computedPaths;
        StringMap<std::string> renamedPaths;
        transformer->getRoot()->reportComputedPaths(&computedPaths, &renamedPaths);

        // Non-simple projections are supported under test only.
        if (computedPaths.size() > 0 || renamedPaths.size() > 0) {
            unsupportedTransformer(transformer);
            return;
        }

        OrderedPathSet preservedPaths;
        transformer->getRoot()->reportProjectedPaths(&preservedPaths);

        for (const std::string& path : preservedPaths) {
            if (FieldRef(path).hasNumericPathComponents()) {
                unsupportedTransformer(transformer);
                return;
            }
        }

        ABTUnsupportedAggExpressionVisitor aggVisitor(_eligible);
        stage_builder::ExpressionWalker walker{&aggVisitor, nullptr, nullptr};
        expression_walker::walk(transformer->rootReplacementExpression().get(), &walker);
    }

    bool& _eligible;
};

template <class RequestType>
bool isEligibleCommon(const RequestType& request,
                      OperationContext* opCtx,
                      const CollectionPtr& collection) {
    // The FindCommandRequest defaults some parameters to BSONObj() instead of boost::none.
    auto noneOrDefaultEmpty = [&](auto param) {
        if constexpr (std::is_same_v<decltype(param), boost::optional<BSONObj>>) {
            return param && !param->isEmpty();
        } else {
            return !param.isEmpty();
        }
    };
    bool unsupportedCmdOption = noneOrDefaultEmpty(request.getHint()) ||
        noneOrDefaultEmpty(request.getCollation()) || noneOrDefaultEmpty(request.getLet()) ||
        request.getLegacyRuntimeConstants();

    bool unsupportedIndexType = [&]() {
        if (collection == nullptr)
            return false;

        const IndexCatalog& indexCatalog = *collection->getIndexCatalog();
        auto indexIterator =
            indexCatalog.getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);

        while (indexIterator->more()) {
            const IndexDescriptor& descriptor = *indexIterator->next()->descriptor();
            if (descriptor.hidden()) {
                // An index that is hidden will not be considered by the optimizer, so we don't need
                // to check its eligibility further.
                continue;
            }

            if (descriptor.infoObj().hasField(IndexDescriptor::kExpireAfterSecondsFieldName) ||
                descriptor.isPartial() || descriptor.isSparse() ||
                descriptor.getIndexType() != IndexType::INDEX_BTREE ||
                !descriptor.collation().isEmpty()) {
                return true;
            }
        }
        return false;
    }();

    bool unsupportedCollectionType = [&]() {
        if (collection == nullptr)
            return false;

        if (collection->isClustered() || !collection->getCollectionOptions().collation.isEmpty() ||
            collection->getTimeseriesOptions() || collection->isCapped()) {
            return true;
        }

        return false;
    }();

    return !unsupportedCmdOption && !unsupportedIndexType && !unsupportedCollectionType &&
        !storageGlobalParams.noTableScan.load();
}

boost::optional<bool> shouldForceEligibility() {
    // Without the feature flag set, no queries are eligible for Bonsai.
    if (!serverGlobalParams.featureCompatibility.isVersionInitialized() ||
        !feature_flags::gFeatureFlagCommonQueryFramework.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        LOGV2_DEBUG(7325100,
                    4,
                    "Bonsai will not be forced because FCV initialized={fcv} and CQF flag={cqf}",
                    "Bonsai will not be forced because FCV isn't initialized or CQF isn't enabled.",
                    "fcv"_attr = serverGlobalParams.featureCompatibility.isVersionInitialized(),
                    "cqf"_attr = !serverGlobalParams.featureCompatibility.isVersionInitialized() ||
                        feature_flags::gFeatureFlagCommonQueryFramework.isEnabled(
                            serverGlobalParams.featureCompatibility));
        return false;
    }

    auto queryControl = ServerParameterSet::getNodeParameterSet()->get<QueryFrameworkControl>(
        "internalQueryFrameworkControl");

    LOGV2_DEBUG(7325101,
                4,
                "internalQueryFrameworkControl={knob}",
                "logging internalQueryFrameworkControl",
                "knob"_attr = QueryFrameworkControl_serializer(queryControl->_data.get()));

    switch (queryControl->_data.get()) {
        case QueryFrameworkControlEnum::kForceClassicEngine:
        case QueryFrameworkControlEnum::kTrySbeEngine:
            return false;
        case QueryFrameworkControlEnum::kTryBonsai:
            // Return boost::none to indicate that we should not force eligibility of bonsai nor the
            // classic engine.
            return boost::none;
        case QueryFrameworkControlEnum::kForceBonsai:
            // This option is only supported with test commands enabled.
            return getTestCommandsEnabled();
    }

    MONGO_UNREACHABLE;
}

bool isEligibleForBonsai(ServiceContext* serviceCtx, const Pipeline& pipeline) {
    ABTUnsupportedDocumentSourceVisitorContext visitorCtx;
    auto& reg = getDocumentSourceVisitorRegistry(serviceCtx);
    DocumentSourceWalker walker(reg, &visitorCtx);
    walker.walk(pipeline);
    return visitorCtx.eligible;
}

bool isEligibleForBonsai(const CanonicalQuery& cq) {
    auto expression = cq.root();
    bool eligible = true;
    ABTMatchExpressionVisitor visitor(eligible);
    MatchExpressionWalker walker(nullptr /*preVisitor*/, nullptr /*inVisitor*/, &visitor);
    tree_walker::walk<true, MatchExpression>(expression, &walker);

    if (cq.getProj() && eligible) {
        auto projExecutor = projection_executor::buildProjectionExecutor(
            cq.getExpCtx(),
            cq.getProj(),
            ProjectionPolicies::findProjectionPolicies(),
            projection_executor::BuilderParamsBitSet{projection_executor::kDefaultBuilderParams});
        ABTTransformerVisitor visitor(eligible);
        TransformerInterfaceWalker walker(&visitor);
        walker.walk(projExecutor.get());
    }

    return eligible;
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(enableExplainInBonsai);

bool isEligibleForBonsai(const AggregateCommandRequest& request,
                         const Pipeline& pipeline,
                         OperationContext* opCtx,
                         const CollectionPtr& collection) {
    if (auto forceBonsai = shouldForceEligibility(); forceBonsai.has_value()) {
        return *forceBonsai;
    }

    // Explain is not currently supported but is allowed if the failpoint is set
    // for testing purposes.
    if (!MONGO_unlikely(enableExplainInBonsai.shouldFail()) && request.getExplain()) {
        return false;
    }

    bool commandOptionsEligible = isEligibleCommon(request, opCtx, collection) &&
        !request.getRequestReshardingResumeToken().has_value() && !request.getExchange();

    // Early return to avoid unnecessary work of walking the input pipeline.
    if (!commandOptionsEligible) {
        return false;
    }

    return isEligibleForBonsai(opCtx->getServiceContext(), pipeline);
}

bool isEligibleForBonsai(const CanonicalQuery& cq,
                         OperationContext* opCtx,
                         const CollectionPtr& collection) {
    if (auto forceBonsai = shouldForceEligibility(); forceBonsai.has_value()) {
        return *forceBonsai;
    }

    // Explain is not currently supported but is allowed if the failpoint is set
    // for testing purposes.
    if (!MONGO_unlikely(enableExplainInBonsai.shouldFail()) && cq.getExplain()) {
        return false;
    }

    auto request = cq.getFindCommandRequest();
    bool commandOptionsEligible = isEligibleCommon(request, opCtx, collection) &&
        request.getSort().isEmpty() && request.getMin().isEmpty() && request.getMax().isEmpty() &&
        !request.getReturnKey() && !request.getSingleBatch() && !request.getTailable() &&
        !request.getSkip() && !request.getLimit() && !request.getNoCursorTimeout() &&
        !request.getRequestResumeToken() && !request.getAllowPartialResults() &&
        !request.getAllowSpeculativeMajorityRead() && !request.getAwaitData() &&
        !request.getReadOnce() && !request.getShowRecordId() && !request.getTerm();

    // Early return to avoid unnecessary work of walking the input expression.
    if (!commandOptionsEligible || !cq.useCqfIfEligible()) {
        return false;
    }

    return isEligibleForBonsai(cq);
}

bool isEligibleForBonsai_forTesting(const CanonicalQuery& cq) {
    return isEligibleForBonsai(cq);
}

bool isEligibleForBonsai_forTesting(ServiceContext* serviceCtx, const Pipeline& pipeline) {
    return isEligibleForBonsai(serviceCtx, pipeline);
}

}  // namespace mongo

namespace mongo::optimizer {
// Templated visit function to mark DocumentSources as ineligible for CQF.
template <typename T>
void visit(ABTUnsupportedDocumentSourceVisitorContext* ctx, const T&) {
    ctx->eligible = false;
}

void visit(ABTUnsupportedDocumentSourceVisitorContext* ctx, const DocumentSourceMatch& source) {
    ABTMatchExpressionVisitor visitor(ctx->eligible);
    MatchExpressionWalker walker(nullptr, nullptr, &visitor);
    tree_walker::walk<true, MatchExpression>(source.getMatchExpression(), &walker);
}

void visit(ABTUnsupportedDocumentSourceVisitorContext* ctx,
           const DocumentSourceSingleDocumentTransformation& source) {
    ABTTransformerVisitor visitor(ctx->eligible);
    TransformerInterfaceWalker walker(&visitor);
    walker.walk(&source.getTransformer());
}

const ServiceContext::ConstructorActionRegisterer abtUnsupportedRegisterer{
    "ABTUnsupportedRegisterer", [](ServiceContext* service) {
        registerMongodVisitor<ABTUnsupportedDocumentSourceVisitorContext>(service);
    }};
}  // namespace mongo::optimizer

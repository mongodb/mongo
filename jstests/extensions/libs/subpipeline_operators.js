/**
 * Shared table of subpipeline operators for extension tests.
 *
 * Each entry builds an outer pipeline that runs `sub` against `target` and surfaces the
 * subpipeline's output as the top-level result stream, so one set of assertions serves every
 * operator.
 *
 * Intentionally excludes $graphLookup, which has no user subpipeline; see
 * extension_in_view_with_graph_lookup.js for its coverage.
 */
import {getLookupStage, getUnionWithStage} from "jstests/libs/query/analyze_plan.js";

export const unionWithPassthrough = (target, sub) => [
    {$match: {__never_matches__: 1}},
    {$unionWith: {coll: target, pipeline: sub}},
];

export const lookupPassthrough = (target, sub) => [
    {$limit: 1},
    {$lookup: {from: target, as: "__joined__", pipeline: sub}},
    {$unwind: "$__joined__"},
    {$replaceRoot: {newRoot: "$__joined__"}},
];

export const SUBPIPELINE_OPS = [
    {name: "$unionWith", build: unionWithPassthrough, getStage: getUnionWithStage},
    {name: "$lookup", build: lookupPassthrough, getStage: getLookupStage},
];

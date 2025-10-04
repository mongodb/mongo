/**
 * This file is part of SPM-3816 "Plan Quality and Stability Testing". It contains
 * a set of pipelines that are used to test the stability of query plans in MongoDB.
 *
 * The file was generated using the following procedure:
 *
 * 1. A large set of pipelines was generated using libfuzz and the grammar from
 * https://github.com/10gen/jstestfuzz/tree/master/src/fuzzers/plan_stability
 *
 * 2. The initial pipelines were reduced to ~1000 pipelines by applying clustering that groups
 * pipelines with similar features together. The clustering was performed with the scripts
 * described in
 * https://github.com/10gen/feature-extractor/blob/main/scripts/cbr/README.md
 *
 * Within each cluster, the best pipeline was selected for inclusion in this file. Usually this
 * is the least complex pipeline that has the highest number of alternative query plans.
 *
 * If the inital round of clustering did not cause all features to be covered, additional
 * rounds are performed and this file will contain some additional pipelines.
 *
 * This file is best not modified manually, except where individual pipelines need to be disabled
 * because they cause test flakes/BFs. Extending the coverage of the test would be accomplished by
 * running the generation procedure again and producing a new file with a new set of pipelines
 * to be committed into the repository separately.
 */

// Input featuresets: 1000000
// Filtered featuresets: 126538

/*
Starting featuresets: 126538
Desired clusters: 1000

Features before clustering:

{ConstantOperator.$all: 24838,
ConstantOperator.$and: 21662,
ConstantOperator.$elemMatch: 48535,
ConstantOperator.$eq: 15278,
ConstantOperator.$exists: 63353,
ConstantOperator.$gt: 18972,
ConstantOperator.$gte: 19340,
ConstantOperator.$in: 44081,
ConstantOperator.$lt: 20760,
ConstantOperator.$lte: 21082,
ConstantOperator.$ne: 22463,
ConstantOperator.$nin: 55308,
ConstantOperator.$nor: 18940,
ConstantOperator.$or: 29067,
IndexCardinalityEstimate.-1: 14419,
IndexCardinalityEstimate.-10: 1,
IndexCardinalityEstimate.-2: 6046,
IndexCardinalityEstimate.-3: 11618,
IndexCardinalityEstimate.-4: 1309,
IndexCardinalityEstimate.-5: 3954,
IndexCardinalityEstimate.-6: 5425,
IndexCardinalityEstimate.-7: 7333,
IndexCardinalityEstimate.-8: 12,
IndexCardinalityEstimate.-9: 4,
IndexCardinalityEstimate.0: 71401,
<IndexProperty.Calculated_keyPattern_compound: 8>: 1070,
<IndexProperty.Calculated_keyPattern_single: 7>: 121909,
<IndexProperty.Calculated_multiBoundField: 6>: 50485,
<IndexProperty.Calculated_multiFieldBound: 5>: 1070,
<IndexProperty.isMultiKey: 0>: 56826,
PipelineStage.$limit: 62644,
PipelineStage.$match: 126530,
PipelineStage.$project: 63248,
PipelineStage.$skip: 19720,
PipelineStage.$sort: 71587,
PlanStage.FETCH: 126475,
PlanStage.IXSCAN: 122979,
PlanStage.LIMIT: 39563,
PlanStage.OR: 3551,
PlanStage.PROJECTION_COVERED: 38,
PlanStage.PROJECTION_SIMPLE: 63210,
PlanStage.SKIP: 19720,
PlanStage.SORT: 43902,
PlanStage.SUBPLAN: 1528,
PlanStageRelationship.FETCH->FETCH: 177,
PlanStageRelationship.FETCH->IXSCAN: 121570,
PlanStageRelationship.FETCH->OR: 3534,
PlanStageRelationship.FETCH->SKIP: 1371,
PlanStageRelationship.LIMIT->FETCH: 17110,
PlanStageRelationship.LIMIT->PROJECTION_COVERED: 14,
PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 19530,
PlanStageRelationship.LIMIT->SKIP: 2909,
PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 30,
PlanStageRelationship.PROJECTION_COVERED->SKIP: 8,
PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 34491,
PlanStageRelationship.PROJECTION_SIMPLE->OR: 1,
PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 9249,
PlanStageRelationship.PROJECTION_SIMPLE->SORT: 19469,
PlanStageRelationship.SKIP->FETCH: 13505,
PlanStageRelationship.SKIP->IXSCAN: 1379,
PlanStageRelationship.SKIP->SORT: 4836,
PlanStageRelationship.SORT->FETCH: 43544,
PlanStageRelationship.SORT->OR: 16,
PlanStageRelationship.SORT->PROJECTION_SIMPLE: 342,
PlanStageRelationship.SUBPLAN->FETCH: 266,
PlanStageRelationship.SUBPLAN->LIMIT: 741,
PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 403,
PlanStageRelationship.SUBPLAN->SKIP: 87,
PlanStageRelationship.SUBPLAN->SORT: 31,
<RangeProperty.entire: 6>: 25506,
<RangeProperty.leftClosed: 1>: 118046,
<RangeProperty.leftLimited: 8>: 36298,
<RangeProperty.leftOpen: 0>: 32086,
<RangeProperty.partial: 5>: 62180,
<RangeProperty.point: 7>: 35845,
<RangeProperty.rightClosed: 4>: 116844,
<RangeProperty.rightLimited: 9>: 38682,
<RangeProperty.rightOpen: 3>: 33063,
<ResultsetSizeRelative.IDENTICAL: 2>: 16910,
<ResultsetSizeRelative.SMALLER: 1>: 109620}

Features after clustering:

{ConstantOperator.$all: 635,
ConstantOperator.$and: 338,
ConstantOperator.$elemMatch: 437,
ConstantOperator.$eq: 189,
ConstantOperator.$exists: 622,
ConstantOperator.$gt: 238,
ConstantOperator.$gte: 249,
ConstantOperator.$in: 469,
ConstantOperator.$lt: 241,
ConstantOperator.$lte: 260,
ConstantOperator.$ne: 225,
ConstantOperator.$nin: 531,
ConstantOperator.$nor: 368,
ConstantOperator.$or: 539,
IndexCardinalityEstimate.-1: 81,
IndexCardinalityEstimate.-2: 47,
IndexCardinalityEstimate.-3: 99,
IndexCardinalityEstimate.-4: 10,
IndexCardinalityEstimate.-5: 35,
IndexCardinalityEstimate.-6: 42,
IndexCardinalityEstimate.-7: 56,
IndexCardinalityEstimate.0: 587,
<IndexProperty.Calculated_keyPattern_compound: 8>: 17,
<IndexProperty.Calculated_keyPattern_single: 7>: 966,
<IndexProperty.Calculated_multiBoundField: 6>: 282,
<IndexProperty.Calculated_multiFieldBound: 5>: 17,
<IndexProperty.isMultiKey: 0>: 422,
PipelineStage.$limit: 337,
PipelineStage.$match: 1000,
PipelineStage.$project: 439,
PipelineStage.$skip: 72,
PipelineStage.$sort: 633,
PlanStage.FETCH: 1000,
PlanStage.IXSCAN: 983,
PlanStage.LIMIT: 196,
PlanStage.OR: 17,
PlanStage.PROJECTION_SIMPLE: 439,
PlanStage.SKIP: 72,
PlanStage.SORT: 288,
PlanStage.SUBPLAN: 27,
PlanStageRelationship.FETCH->FETCH: 3,
PlanStageRelationship.FETCH->IXSCAN: 983,
PlanStageRelationship.FETCH->OR: 17,
PlanStageRelationship.LIMIT->FETCH: 97,
PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 94,
PlanStageRelationship.LIMIT->SKIP: 5,
PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 308,
PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 28,
PlanStageRelationship.PROJECTION_SIMPLE->SORT: 103,
PlanStageRelationship.SKIP->FETCH: 49,
PlanStageRelationship.SKIP->SORT: 23,
PlanStageRelationship.SORT->FETCH: 283,
PlanStageRelationship.SORT->PROJECTION_SIMPLE: 5,
PlanStageRelationship.SUBPLAN->FETCH: 6,
PlanStageRelationship.SUBPLAN->LIMIT: 8,
PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 11,
PlanStageRelationship.SUBPLAN->SKIP: 1,
PlanStageRelationship.SUBPLAN->SORT: 1,
<RangeProperty.entire: 6>: 223,
<RangeProperty.leftClosed: 1>: 955,
<RangeProperty.leftLimited: 8>: 183,
<RangeProperty.leftOpen: 0>: 130,
<RangeProperty.partial: 5>: 496,
<RangeProperty.point: 7>: 275,
<RangeProperty.rightClosed: 4>: 944,
<RangeProperty.rightLimited: 9>: 199,
<RangeProperty.rightOpen: 3>: 139,
<ResultsetSizeRelative.IDENTICAL: 2>: 95,
<ResultsetSizeRelative.SMALLER: 1>: 905}

Features remaining:

{IndexCardinalityEstimate.-10,
IndexCardinalityEstimate.-8,
IndexCardinalityEstimate.-9,
PlanStage.PROJECTION_COVERED,
PlanStageRelationship.FETCH->SKIP,
PlanStageRelationship.LIMIT->PROJECTION_COVERED,
PlanStageRelationship.PROJECTION_COVERED->IXSCAN,
PlanStageRelationship.PROJECTION_COVERED->SKIP,
PlanStageRelationship.PROJECTION_SIMPLE->OR,
PlanStageRelationship.SKIP->IXSCAN,
PlanStageRelationship.SORT->OR}
*/

const True = true;
const False = false;
const None = null;

export const pipelines = [
    /* clusterSize: 1456, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_compound": {"$exists": False}},
                    {"d_idx": {"$gte": 19}},
                    {"a_compound": {"$all": [13, 19, 6]}},
                ],
                "i_compound": {"$in": [2, 7]},
                "z_compound": {"$nin": [5, 14]},
            },
        },
        {"$limit": 31},
    ],
    /* clusterSize: 1310, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [15, 7, 14, 16, 16]}}, {"h_idx": {"$exists": False}}],
                "d_noidx": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "z_idx": 1}},
    ],
    /* clusterSize: 1216, queryRank: 17.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [5, 12, 10]}},
                    {"c_compound": {"$exists": False}},
                    {"a_idx": {"$ne": 9}},
                    {"k_compound": {"$gt": 14}},
                ],
                "h_compound": {"$in": [19, 13, 6, 9]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 239},
        {"$project": {"c_compound": 1, "d_compound": 1}},
    ],
    /* clusterSize: 1174, queryRank: 15.03 */ [
        {
            "$match": {
                "$nor": [{"i_idx": {"$exists": False}}, {"a_compound": {"$all": [19, 18, 6, 2]}}],
                "a_compound": {"$elemMatch": {"$exists": True}},
                "c_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 1150, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"i_idx": {"$nin": [17, 4, 16]}},
                    {"a_compound": {"$all": [12, 9]}},
                    {"k_compound": {"$in": [2, 6, 12]}},
                ],
                "a_compound": {"$nin": [5, 12, 17, 9]},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 1087, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_compound": {"$nin": [18, 16, 18]}},
                    {
                        "$or": [
                            {"d_idx": {"$nin": [20, 6]}},
                            {"a_idx": {"$all": [11, 1]}},
                            {"a_compound": {"$exists": False}},
                            {
                                "$and": [
                                    {"a_compound": {"$all": [17, 19, 3]}},
                                    {"i_compound": {"$in": [16, 9, 18, 4, 11]}},
                                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                                    {"a_noidx": {"$all": [14, 17, 1]}},
                                ],
                            },
                        ],
                    },
                ],
                "h_compound": {"$exists": True},
            },
        },
        {"$skip": 6},
        {"$project": {"a_idx": 1, "d_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 1054, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [18, 14, 14, 16]}}, {"z_compound": {"$in": [13, 4, 14]}}],
                "d_compound": {"$exists": True},
                "d_idx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$limit": 145},
        {"$skip": 7},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 1011, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_noidx": {"$exists": True}},
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"a_idx": {"$elemMatch": {"$nin": [9, 8, 6, 4, 4, 18]}}},
                    {"$nor": [{"a_compound": {"$all": [9, 17, 2]}}, {"k_compound": {"$exists": False}}]},
                ],
                "a_noidx": {"$gt": 19},
            },
        },
        {"$sort": {"d_idx": -1, "h_idx": 1}},
        {"$limit": 19},
        {"$skip": 6},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 903, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"h_idx": {"$gt": 1}},
                    {"a_compound": {"$nin": [1, 20, 1]}},
                    {"a_compound": {"$all": [15, 14, 11, 10, 17]}},
                ],
                "i_idx": {"$nin": [14, 20]},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$project": {"a_noidx": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 894, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [16, 9, 12, 10]}}, {"k_idx": {"$gt": 1}}],
                "a_idx": {"$nin": [10, 8]},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 867, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$exists": True}},
                    {"c_compound": {"$gt": 13}},
                    {"a_compound": {"$ne": 16}},
                    {"i_idx": {"$exists": True}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
                "a_idx": {"$in": [1, 11, 8, 5]},
            },
        },
        {"$limit": 238},
    ],
    /* clusterSize: 853, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$and": [
                            {"a_compound": {"$elemMatch": {"$exists": True, "$lt": 12, "$nin": [14, 11]}}},
                            {"a_compound": {"$exists": True}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_compound": {"$elemMatch": {"$lt": 14}}},
                    {"k_compound": {"$exists": True}},
                    {"z_compound": {"$nin": [20, 11]}},
                ],
                "a_idx": {"$exists": True},
                "c_compound": {"$exists": True},
            },
        },
        {"$limit": 95},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 823, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [7, 13, 3]}},
                    {"i_compound": {"$in": [17, 18]}},
                    {"a_idx": {"$exists": True}},
                ],
                "d_compound": {"$exists": True},
                "k_compound": {"$exists": True},
            },
        },
        {"$limit": 218},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "h_compound": 1}},
    ],
    /* clusterSize: 821, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_noidx": {"$lt": 5}},
                    {
                        "$or": [
                            {
                                "$and": [
                                    {"a_noidx": {"$lt": 2}},
                                    {"d_noidx": {"$exists": True}},
                                    {"a_idx": {"$all": [6, 4, 6]}},
                                    {"c_compound": {"$ne": 13}},
                                ],
                            },
                            {"a_compound": {"$elemMatch": {"$nin": [14, 3]}}},
                        ],
                    },
                    {"d_idx": {"$lt": 7}},
                ],
                "a_compound": {"$gt": 10},
            },
        },
        {"$limit": 165},
        {"$skip": 33},
    ],
    /* clusterSize: 819, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [4, 10, 16]}}, {"h_idx": {"$lte": 9}}],
                "c_compound": {"$nin": [8, 16, 7]},
            },
        },
        {"$limit": 215},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 794, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [1, 13, 11]}}, {"c_idx": {"$exists": False}}],
                "h_compound": {"$eq": 14},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"a_compound": 1, "c_noidx": 1}},
    ],
    /* clusterSize: 781, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$exists": False}},
                    {
                        "$and": [
                            {"a_compound": {"$all": [6, 2]}},
                            {"i_idx": {"$exists": True}},
                            {"a_compound": {"$all": [14, 19]}},
                        ],
                    },
                ],
                "i_compound": {"$in": [2, 14, 4, 7]},
            },
        },
        {"$limit": 191},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 709, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_idx": {"$ne": 14}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [20, 13, 2, 8]}},
                            {"a_compound": {"$all": [1, 1]}},
                            {"k_noidx": {"$in": [14, 17]}},
                        ],
                    },
                ],
                "a_idx": {"$elemMatch": {"$ne": 5}},
            },
        },
        {"$limit": 217},
        {"$project": {"_id": 0, "a_compound": 1, "c_compound": 1}},
    ],
    /* clusterSize: 691, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$in": [3, 1, 19, 17, 1]}},
                    {"i_idx": {"$in": [11, 16, 15]}},
                    {"a_compound": {"$all": [8, 12, 13]}},
                ],
                "a_compound": {"$elemMatch": {"$in": [17, 4, 17], "$lt": 13}},
            },
        },
        {"$sort": {"d_idx": 1, "h_idx": -1, "i_idx": -1}},
        {"$project": {"_id": 0, "z_idx": 1}},
    ],
    /* clusterSize: 691, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [1, 12, 7, 9]}},
                    {"a_idx": {"$all": [19, 3, 2, 7]}},
                    {"a_compound": {"$gt": 11}},
                ],
                "i_compound": {"$gte": 1},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 196},
        {"$skip": 52},
    ],
    /* clusterSize: 661, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"c_compound": {"$ne": 18}},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$exists": True, "$nin": [3, 4]}}},
                            {"c_compound": {"$ne": 10}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$nin": [3, 20]}}},
                            {"k_compound": {"$exists": False}},
                            {"a_idx": {"$lt": 15}},
                        ],
                    },
                ],
                "a_compound": {"$lte": 4},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 237},
        {"$skip": 14},
    ],
    /* clusterSize: 655, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$nin": [13, 11]}},
                    {"$or": [{"a_idx": {"$all": [1, 2, 2, 12]}}, {"c_idx": {"$gt": 7}}]},
                ],
                "c_compound": {"$nin": [14, 13, 16]},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 200},
    ],
    /* clusterSize: 608, queryRank: 13.03 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"i_idx": {"$in": [2, 11]}}, {"a_compound": {"$all": [8, 16]}}]},
                    {"k_compound": {"$gt": 9}},
                ],
                "a_idx": {"$gt": 16},
            },
        },
        {"$sort": {"c_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 603, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [15, 8, 3, 9]}}, {"k_idx": {"$gte": 15}}],
                "c_compound": {"$gte": 1},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$skip": 67},
        {"$project": {"a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 596, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [10, 14, 3, 20]}}, {"d_compound": {"$eq": 2}}],
                "a_compound": {"$ne": 15},
            },
        },
    ],
    /* clusterSize: 591, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$nor": [
                            {"a_compound": {"$gt": 15}},
                            {"z_compound": {"$nin": [13, 15]}},
                            {"a_compound": {"$lt": 3}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$eq": 13, "$exists": True, "$lt": 10}}},
                    {"a_noidx": {"$exists": False}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$skip": 5},
    ],
    /* clusterSize: 591, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$nin": [11, 15]}},
                    {"$nor": [{"a_compound": {"$all": [1, 2, 6]}}, {"a_compound": {"$gt": 9}}]},
                    {"$nor": [{"a_compound": {"$lte": 3}}, {"a_noidx": {"$exists": False}}]},
                ],
                "a_compound": {"$elemMatch": {"$gt": 5}},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"i_noidx": 1}},
    ],
    /* clusterSize: 585, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {"d_idx": {"$exists": True}},
                            {"a_idx": {"$exists": False}},
                            {"d_compound": {"$in": [13, 5]}},
                            {"a_compound": {"$all": [12, 5, 9, 15]}},
                        ],
                    },
                    {"i_compound": {"$exists": False}},
                ],
                "z_compound": {"$exists": True},
            },
        },
        {"$limit": 251},
    ],
    /* clusterSize: 577, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$and": [{"h_compound": {"$in": [6, 9]}}, {"a_compound": {"$all": [18, 14, 12]}}]},
                    {"h_idx": {"$in": [18, 13, 17, 20]}},
                    {"k_compound": {"$in": [12, 10, 3]}},
                ],
                "a_compound": {"$gt": 20},
                "a_noidx": {"$gt": 3},
                "k_idx": {"$ne": 19},
            },
        },
        {"$limit": 192},
        {"$project": {"a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 569, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"i_compound": {"$exists": True}}, {"z_compound": {"$nin": [12, 16, 3, 18, 2]}}],
                "d_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 567, queryRank: 15.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$all": [1, 12]}},
                    {"$or": [{"a_idx": {"$all": [19, 10]}}, {"i_idx": {"$gte": 19}}, {"i_compound": {"$ne": 5}}]},
                ],
            },
        },
        {"$limit": 44},
    ],
    /* clusterSize: 563, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [8, 16]}}, {"a_idx": {"$gt": 20}}],
                "i_noidx": {"$gte": 20},
                "k_compound": {"$ne": 9},
            },
        },
        {"$limit": 9},
    ],
    /* clusterSize: 559, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [7, 19, 12, 3]}},
                    {"c_idx": {"$eq": 6}},
                    {"a_idx": {"$exists": False}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1, "i_idx": 1}},
        {"$skip": 54},
    ],
    /* clusterSize: 552, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [17, 11]}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [8, 2]}},
                            {"h_idx": {"$nin": [5, 18, 9, 5]}},
                            {"z_compound": {"$exists": True}},
                        ],
                    },
                    {"h_compound": {"$exists": False}},
                ],
                "a_compound": {"$elemMatch": {"$ne": 3}},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 67},
    ],
    /* clusterSize: 540, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$exists": True}}, {"a_compound": {"$exists": False}}, {"a_compound": {"$gte": 1}}],
                "a_idx": {"$nin": [17, 1, 20]},
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 171},
        {"$skip": 11},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 533, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [8, 6, 5]}}, {"z_compound": {"$exists": False}}],
                "c_compound": {"$lt": 5},
            },
        },
        {"$limit": 197},
    ],
    /* clusterSize: 531, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [10, 10, 12, 19]}}, {"k_compound": {"$lt": 15}}, {"k_compound": {"$eq": 8}}],
                "a_idx": {"$in": [13, 9, 12]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 525, queryRank: 22.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_idx": {"$exists": True}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [4, 17]}},
                            {
                                "$and": [
                                    {"h_idx": {"$in": [7, 4]}},
                                    {"i_compound": {"$lte": 9}},
                                    {"h_noidx": {"$gte": 8}},
                                    {"c_noidx": {"$ne": 1}},
                                    {"a_idx": {"$all": [7, 17, 8, 14]}},
                                    {"h_idx": {"$nin": [13, 7]}},
                                ],
                            },
                        ],
                    },
                    {"$or": [{"a_idx": {"$all": [7, 7, 11, 2]}}, {"k_compound": {"$eq": 17}}]},
                ],
                "a_compound": {"$in": [6, 20, 4]},
            },
        },
        {"$limit": 54},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 513, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$ne": 9}}, {"a_compound": {"$all": [8, 19, 8, 18]}}],
                "k_compound": {"$in": [16, 8, 8]},
            },
        },
        {"$limit": 167},
    ],
    /* clusterSize: 508, queryRank: 5.03 */ [
        {"$match": {"c_compound": {"$nin": [19, 8]}, "z_compound": {"$nin": [17, 15]}}},
        {"$sort": {"z_idx": 1}},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 499, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$nin": [12, 13, 11]}}, {"a_compound": {"$all": [14, 4, 12]}}],
                "a_compound": {"$elemMatch": {"$nin": [3, 4]}},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"k_idx": 1}},
    ],
    /* clusterSize: 497, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$elemMatch": {"$nin": [13, 1, 18, 5]}}}, {"a_compound": {"$all": [4, 3]}}],
                "a_idx": {"$in": [10, 20, 2]},
            },
        },
        {"$limit": 43},
    ],
    /* clusterSize: 476, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [18, 14]}},
                    {
                        "$nor": [
                            {"a_noidx": {"$all": [15, 7, 7]}},
                            {"z_compound": {"$gt": 14}},
                            {"a_noidx": {"$all": [9, 15]}},
                        ],
                    },
                ],
                "c_idx": {"$exists": True},
            },
        },
        {"$skip": 76},
    ],
    /* clusterSize: 466, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [4, 18, 3]}},
                    {"a_compound": {"$exists": False}},
                    {"a_idx": {"$elemMatch": {"$exists": False, "$lt": 20}}},
                ],
                "a_idx": {"$nin": [18, 7, 19, 13]},
                "c_noidx": {"$nin": [4, 2, 7, 5]},
                "z_noidx": {"$nin": [3, 5]},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$skip": 66},
    ],
    /* clusterSize: 466, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [5, 16, 12]}},
                    {"z_idx": {"$in": [12, 3]}},
                    {"a_compound": {"$in": [20, 3]}},
                ],
                "a_idx": {"$all": [9, 1]},
                "i_noidx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"z_compound": 1}},
    ],
    /* clusterSize: 460, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"z_compound": {"$exists": True}},
                    {"z_idx": {"$exists": False}},
                    {"a_compound": {"$all": [11, 9]}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 455, queryRank: 13.04 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [18, 17]}}, {"z_compound": {"$gte": 4}}],
                "a_compound": {"$nin": [10, 13, 17]},
                "c_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 444, queryRank: 7.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$nin": [1, 18]}}, {"a_compound": {"$all": [6, 17]}}],
                "a_compound": {"$nin": [13, 10, 18]},
            },
        },
        {"$sort": {"h_idx": 1, "i_idx": -1}},
        {"$limit": 202},
    ],
    /* clusterSize: 442, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [12, 9, 2, 5, 17]}}, {"a_idx": {"$lt": 1}}],
                "k_idx": {"$lt": 2},
                "k_noidx": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 228},
    ],
    /* clusterSize: 442, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$in": [6, 16, 16, 11]}},
                    {"a_idx": {"$all": [13, 8, 16, 20, 17]}},
                    {"c_compound": {"$exists": True}},
                ],
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 124},
    ],
    /* clusterSize: 438, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [12, 10, 20]}}, {"a_compound": {"$exists": True}}],
                "a_idx": {"$nin": [19, 14]},
                "k_compound": {"$nin": [15, 15, 15]},
            },
        },
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 437, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$in": [5, 17, 2]}},
                    {"a_idx": {"$all": [11, 20, 7]}},
                    {"z_compound": {"$nin": [13, 14, 10, 10, 11]}},
                ],
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$limit": 240},
        {"$project": {"c_compound": 1, "c_noidx": 1}},
    ],
    /* clusterSize: 434, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [19, 20]}}, {"k_compound": {"$exists": False}}],
                "i_idx": {"$gte": 11},
            },
        },
        {"$sort": {"a_idx": 1, "i_idx": 1}},
        {"$skip": 16},
        {"$project": {"_id": 0, "h_noidx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 434, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$exists": True}},
                    {"d_compound": {"$gt": 12}},
                    {"a_compound": {"$in": [16, 10, 18]}},
                    {"k_idx": {"$in": [14, 18]}},
                    {"i_idx": {"$lt": 19}},
                ],
                "a_compound": {"$lt": 16},
            },
        },
        {"$limit": 168},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 427, queryRank: 9.03 */ [
        {"$match": {"$or": [{"z_compound": {"$exists": True}}, {"a_idx": {"$all": [6, 20, 6, 18]}}]}},
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "k_noidx": 1}},
    ],
    /* clusterSize: 420, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"h_noidx": {"$gt": 6}}, {"a_compound": {"$all": [5, 11, 6, 14]}}],
                "a_compound": {"$lt": 2},
            },
        },
    ],
    /* clusterSize: 415, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"k_noidx": {"$lt": 11}}, {"a_compound": {"$all": [8, 20, 3, 6, 8]}}],
                "k_idx": {"$in": [12, 8]},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 412, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [13, 13, 9, 18, 5]}}, {"z_compound": {"$in": [8, 20]}}],
                "a_compound": {"$gt": 19},
            },
        },
        {"$limit": 96},
    ],
    /* clusterSize: 409, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {
                                "$and": [
                                    {"a_compound": {"$elemMatch": {"$exists": False, "$gte": 8}}},
                                    {"a_noidx": {"$elemMatch": {"$in": [11, 2]}}},
                                    {"a_idx": {"$elemMatch": {"$exists": True, "$gte": 7, "$lte": 2}}},
                                    {"a_idx": {"$eq": 8}},
                                ],
                            },
                            {"a_compound": {"$elemMatch": {"$exists": False}}},
                        ],
                    },
                    {"z_idx": {"$exists": True}},
                ],
                "a_compound": {"$elemMatch": {"$ne": 3}},
                "z_idx": {"$ne": 7},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 76},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 407, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"k_compound": {"$nin": [14, 9, 14]}},
                            {"a_compound": {"$all": [7, 2, 4, 19, 19]}},
                            {"a_idx": {"$all": [3, 8]}},
                        ],
                    },
                    {"c_idx": {"$ne": 7}},
                    {"k_compound": {"$gte": 6}},
                ],
                "d_compound": {"$gte": 4},
            },
        },
        {"$limit": 214},
        {"$project": {"a_compound": 1, "z_compound": 1}},
    ],
    /* clusterSize: 402, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$all": [1, 7]}}, {"z_compound": {"$eq": 1}}],
                "c_compound": {"$lte": 17},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 253},
        {"$project": {"_id": 0, "a_idx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 402, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$in": [11, 7]}},
                    {"a_idx": {"$ne": 9}},
                    {"a_compound": {"$all": [5, 16, 20, 8]}},
                ],
                "a_compound": {"$elemMatch": {"$nin": [3, 16]}},
                "a_noidx": {"$nin": [14, 11, 17]},
                "k_idx": {"$nin": [7, 15]},
            },
        },
        {"$sort": {"a_idx": 1, "c_idx": 1}},
        {"$limit": 41},
        {"$project": {"_id": 0, "h_compound": 1}},
    ],
    /* clusterSize: 399, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"i_noidx": {"$gt": 4}}, {"a_compound": {"$gt": 16}}, {"a_compound": {"$all": [12, 20, 11]}}],
                "d_idx": {"$ne": 1},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 218},
    ],
    /* clusterSize: 397, queryRank: 18.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"i_idx": {"$eq": 19}},
                            {"c_compound": {"$gt": 8}},
                            {"a_compound": {"$all": [4, 16, 8, 17, 19]}},
                            {"a_compound": {"$all": [19, 12]}},
                        ],
                    },
                    {
                        "$or": [
                            {"z_idx": {"$eq": 8}},
                            {"i_compound": {"$in": [19, 18, 9, 7]}},
                            {"a_compound": {"$exists": True}},
                            {"a_idx": {"$in": [19, 20, 18]}},
                            {"a_compound": {"$all": [10, 2, 11]}},
                        ],
                    },
                ],
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 392, queryRank: 9.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [15, 9, 3, 8, 17]}}, {"d_noidx": {"$gt": 15}}],
                "$or": [{"a_idx": {"$all": [3, 4, 2]}}, {"a_compound": {"$lt": 16}}],
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 162},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "d_noidx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 389, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$gte": 2}},
                    {"a_idx": {"$nin": [19, 5, 11]}},
                    {"a_idx": {"$all": [4, 6]}},
                    {"a_idx": {"$all": [20, 16]}},
                ],
                "c_idx": {"$nin": [12, 6, 11, 11]},
            },
        },
    ],
    /* clusterSize: 379, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [12, 5, 5, 1]}}, {"h_noidx": {"$nin": [2, 19, 16, 12]}}],
                "a_compound": {"$elemMatch": {"$gt": 9}},
                "c_compound": {"$exists": True},
                "c_noidx": {"$lt": 14},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 67},
        {"$project": {"a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 374, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [{"i_idx": {"$nin": [8, 14]}}, {"a_compound": {"$all": [7, 12, 8, 5, 16, 6]}}],
                "d_idx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$limit": 251},
        {"$project": {"_id": 0, "z_idx": 1}},
    ],
    /* clusterSize: 373, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$in": [12, 6, 5]}},
                    {"z_compound": {"$exists": False}},
                    {"c_compound": {"$in": [9, 15]}},
                ],
                "a_compound": {"$nin": [12, 15]},
            },
        },
        {"$limit": 236},
    ],
    /* clusterSize: 372, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [{"c_compound": {"$exists": False}}, {"d_idx": {"$eq": 11}}],
                "$or": [{"d_compound": {"$exists": False}}, {"a_compound": {"$elemMatch": {"$in": [20, 16, 9, 16]}}}],
                "a_compound": {"$exists": True},
                "k_compound": {"$nin": [10, 16]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 49},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 372, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [9, 2]}}, {"a_compound": {"$lte": 19}}],
                "a_compound": {"$in": [10, 12, 12]},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$limit": 63},
    ],
    /* clusterSize: 369, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [11, 9, 1]}},
                    {"$or": [{"i_compound": {"$exists": True}}, {"a_compound": {"$all": [15, 8, 8]}}]},
                    {"$or": [{"d_idx": {"$exists": False}}, {"a_idx": {"$exists": False}}]},
                    {"a_idx": {"$all": [15, 9, 16, 13, 1, 11]}},
                ],
                "i_compound": {"$lt": 16},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 369, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [6, 19, 14]}}, {"a_compound": {"$elemMatch": {"$lte": 9}}}],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$skip": 80},
    ],
    /* clusterSize: 366, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$lte": 14}},
                    {"z_compound": {"$lt": 20}},
                    {"$and": [{"a_compound": {"$all": [18, 10, 7]}}, {"a_compound": {"$all": [15, 12, 3]}}]},
                ],
                "a_idx": {"$elemMatch": {"$lte": 11, "$nin": [16, 8, 12]}},
                "a_noidx": {"$all": [3, 19, 9]},
            },
        },
        {"$sort": {"h_idx": -1, "i_idx": 1}},
        {"$limit": 36},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 366, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [4, 2, 14, 8]}}, {"d_idx": {"$exists": False}}],
                "a_compound": {"$exists": True},
            },
        },
        {"$skip": 66},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 364, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_idx": {"$in": [2, 4]}},
                    {"$or": [{"a_idx": {"$elemMatch": {"$lt": 6}}}, {"a_idx": {"$gte": 13}}, {"i_noidx": {"$eq": 14}}]},
                    {
                        "$or": [
                            {"a_idx": {"$all": [8, 16, 15]}},
                            {"z_compound": {"$nin": [5, 16]}},
                            {"a_idx": {"$all": [3, 10, 14]}},
                        ],
                    },
                ],
                "$or": [{"a_idx": {"$ne": 8}}, {"k_idx": {"$in": [18, 4, 10]}}, {"a_idx": {"$all": [5, 1]}}],
                "a_idx": {"$nin": [14, 7, 9, 7]},
            },
        },
        {"$limit": 218},
        {"$project": {"i_noidx": 1}},
    ],
    /* clusterSize: 363, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$ne": 18}}, {"a_idx": {"$all": [16, 2]}}, {"c_compound": {"$gt": 18}}],
                "h_noidx": {"$ne": 2},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 180},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 361, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"$and": [{"d_compound": {"$nin": [2, 11]}}, {"a_idx": {"$exists": True}}]},
                    {"a_compound": {"$exists": True}},
                ],
                "$nor": [{"h_noidx": {"$nin": [9, 20]}}, {"a_compound": {"$all": [2, 5, 7]}}],
                "i_compound": {"$lt": 18},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 180},
        {"$project": {"a_noidx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 356, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"c_compound": {"$exists": True}},
                    {"i_compound": {"$nin": [4, 8, 18, 13]}},
                    {"a_idx": {"$elemMatch": {"$nin": [6, 18]}}},
                ],
                "$nor": [{"a_compound": {"$nin": [12, 14, 2]}}, {"a_noidx": {"$elemMatch": {"$exists": False}}}],
                "a_compound": {"$all": [2, 11]},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 350, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [15, 19, 4]}}, {"a_idx": {"$exists": False}}],
                "d_idx": {"$ne": 13},
            },
        },
        {"$project": {"h_noidx": 1}},
    ],
    /* clusterSize: 342, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$eq": 13}},
                    {"$nor": [{"a_idx": {"$eq": 4}}, {"a_compound": {"$in": [4, 12]}}]},
                    {"$and": [{"a_compound": {"$exists": True}}, {"h_idx": {"$exists": True}}]},
                ],
                "h_compound": {"$nin": [14, 11, 15]},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 336, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$lte": 11}},
                    {"$or": [{"a_compound": {"$exists": True}}, {"a_idx": {"$all": [4, 20, 5]}}]},
                ],
                "a_noidx": {"$exists": True},
                "i_noidx": {"$ne": 18},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 110},
        {"$project": {"_id": 0, "a_compound": 1, "h_compound": 1}},
    ],
    /* clusterSize: 336, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$in": [1, 20]}},
                    {"a_compound": {"$all": [2, 19, 4]}},
                    {"a_compound": {"$elemMatch": {"$gt": 19}}},
                ],
                "a_idx": {"$exists": True},
            },
        },
        {"$limit": 19},
    ],
    /* clusterSize: 332, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [16, 10, 15]}},
                    {"a_compound": {"$all": [20, 16, 5, 6]}},
                    {"h_idx": {"$gt": 8}},
                ],
                "a_compound": {"$gt": 3},
            },
        },
        {"$project": {"a_compound": 1, "d_noidx": 1}},
    ],
    /* clusterSize: 332, queryRank: 6.02 */ [
        {"$match": {"a_compound": {"$lte": 9}, "d_compound": {"$in": [4, 4, 2, 9]}, "d_idx": {"$lt": 16}}},
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1, "c_idx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 331, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$all": [6, 12]}}, {"k_compound": {"$lte": 1}}]},
                    {"a_idx": {"$nin": [8, 5]}},
                    {"i_noidx": {"$nin": [6, 15]}},
                ],
                "a_idx": {"$elemMatch": {"$gt": 9}},
                "z_noidx": {"$lte": 19},
            },
        },
        {"$limit": 104},
    ],
    /* clusterSize: 329, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [8, 8]}}, {"a_idx": {"$all": [1, 15, 14, 15]}}, {"z_compound": {"$lt": 13}}],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": -1, "z_idx": 1}},
    ],
    /* clusterSize: 324, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [{"i_compound": {"$exists": True}}, {"a_idx": {"$elemMatch": {"$in": [18, 9, 17]}}}],
                "$or": [
                    {
                        "$and": [
                            {"z_idx": {"$gt": 7}},
                            {"a_idx": {"$eq": 2}},
                            {"a_idx": {"$all": [7, 18]}},
                            {"a_compound": {"$all": [14, 9, 14]}},
                        ],
                    },
                    {"i_idx": {"$lt": 19}},
                    {"a_compound": {"$all": [15, 3]}},
                    {"a_compound": {"$all": [7, 4]}},
                    {"z_idx": {"$exists": True}},
                ],
            },
        },
        {"$limit": 157},
        {"$project": {"_id": 0, "c_compound": 1}},
    ],
    /* clusterSize: 324, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_idx": {"$exists": True}},
                    {"i_compound": {"$eq": 5}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [17, 10, 20, 15]}},
                            {"a_compound": {"$nin": [5, 9]}},
                            {"a_idx": {"$gte": 8}},
                        ],
                    },
                ],
                "h_noidx": {"$exists": True},
            },
        },
        {"$limit": 48},
        {"$project": {"_id": 0, "a_compound": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 312, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$exists": False}},
                    {"a_compound": {"$gte": 8}},
                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                ],
                "$or": [
                    {"z_idx": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$gt": 11}}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "d_compound": {"$nin": [18, 2]},
            },
        },
        {"$limit": 236},
    ],
    /* clusterSize: 310, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [{"c_idx": {"$nin": [20, 3, 7, 17, 6]}}, {"i_compound": {"$nin": [13, 20, 6]}}],
                "$nor": [{"k_idx": {"$exists": False}}, {"a_compound": {"$all": [6, 16, 18]}}],
                "a_noidx": {"$in": [19, 15]},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 172},
    ],
    /* clusterSize: 309, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_noidx": {"$gt": 1}},
                    {"a_idx": {"$elemMatch": {"$lte": 16}}},
                    {"a_compound": {"$in": [16, 3]}},
                    {"c_compound": {"$nin": [20, 11, 15, 9]}},
                    {"$nor": [{"a_idx": {"$nin": [8, 16, 3]}}, {"a_compound": {"$all": [20, 6, 2]}}]},
                ],
                "a_noidx": {"$gt": 14},
            },
        },
        {"$limit": 73},
        {"$project": {"_id": 0, "i_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 309, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [10, 18, 12]}}, {"i_compound": {"$exists": True}}],
                "k_idx": {"$exists": True},
                "z_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1, "d_idx": 1, "h_idx": -1, "i_idx": -1, "z_idx": -1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 308, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [10, 3, 13, 19]}}, {"h_idx": {"$in": [12, 12]}}],
                "i_idx": {"$in": [6, 19]},
            },
        },
        {"$project": {"a_compound": 1, "z_compound": 1}},
    ],
    /* clusterSize: 301, queryRank: 16.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [1, 1, 6, 3]}}, {"a_compound": {"$exists": True}}],
                "a_compound": {"$all": [13, 2, 3]},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 169},
        {"$project": {"d_compound": 1}},
    ],
    /* clusterSize: 295, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [13, 19, 7]}}, {"a_idx": {"$in": [12, 20]}}],
                "a_compound": {"$elemMatch": {"$exists": True}},
                "k_compound": {"$exists": True},
            },
        },
        {"$limit": 122},
        {"$project": {"_id": 0, "a_noidx": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 295, queryRank: 15.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True, "$nin": [5, 6]}}},
                    {
                        "$and": [
                            {"a_noidx": {"$exists": False}},
                            {"a_idx": {"$all": [1, 19, 6]}},
                            {"a_compound": {"$exists": True}},
                        ],
                    },
                ],
                "a_compound": {"$all": [15, 5]},
            },
        },
        {"$limit": 39},
    ],
    /* clusterSize: 293, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$gt": 8}},
                    {"a_compound": {"$all": [13, 3, 4]}},
                    {"a_idx": {"$elemMatch": {"$lte": 3}}},
                ],
                "a_idx": {"$elemMatch": {"$eq": 19}},
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 293, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_compound": {"$lte": 18}}, {"a_idx": {"$all": [1, 5, 13, 19]}}]},
                    {"$and": [{"h_noidx": {"$eq": 2}}, {"i_noidx": {"$lt": 16}}, {"a_compound": {"$gt": 11}}]},
                ],
                "a_idx": {"$in": [15, 3, 14]},
                "i_compound": {"$gt": 18},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 232},
    ],
    /* clusterSize: 292, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [2, 18]}},
                    {"a_compound": {"$all": [15, 14, 8, 14]}},
                    {
                        "$or": [
                            {"h_idx": {"$nin": [1, 13]}},
                            {"$and": [{"k_compound": {"$exists": True}}, {"z_noidx": {"$exists": False}}]},
                            {"a_compound": {"$all": [14, 3]}},
                        ],
                    },
                ],
                "a_compound": {"$gt": 4},
            },
        },
        {"$limit": 228},
    ],
    /* clusterSize: 291, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$gt": 14}}, {"i_compound": {"$lte": 17}}],
                "a_compound": {"$eq": 6},
                "k_compound": {"$ne": 5},
            },
        },
        {"$sort": {"h_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 291, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$exists": False}},
                    {"a_idx": {"$nin": [11, 15]}},
                    {"a_idx": {"$elemMatch": {"$in": [12, 16]}}},
                ],
                "$or": [
                    {"i_idx": {"$nin": [6, 20, 14]}},
                    {"i_compound": {"$nin": [17, 17, 4]}},
                    {"c_compound": {"$in": [16, 5]}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "c_idx": 1}},
    ],
    /* clusterSize: 288, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_compound": {"$lt": 7}},
                    {"i_idx": {"$eq": 5}},
                    {"$and": [{"a_compound": {"$all": [16, 20]}}, {"a_compound": {"$nin": [10, 10, 5, 10]}}]},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$limit": 197},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 286, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"k_compound": {"$gte": 2}},
                    {"a_idx": {"$all": [2, 2, 15]}},
                    {
                        "$or": [
                            {"k_idx": {"$lte": 11}},
                            {
                                "$or": [
                                    {"a_compound": {"$exists": True}},
                                    {"a_idx": {"$all": [8, 12, 9, 10]}},
                                    {"d_compound": {"$exists": True}},
                                ],
                            },
                            {"a_idx": {"$exists": True}},
                        ],
                    },
                    {"$or": [{"a_noidx": {"$lt": 6}}, {"i_compound": {"$exists": True}}]},
                ],
            },
        },
        {"$limit": 19},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 285, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [10, 7]}}, {"a_compound": {"$nin": [12, 3, 6]}}],
                "k_idx": {"$eq": 14},
            },
        },
        {"$sort": {"a_idx": 1, "z_idx": -1}},
    ],
    /* clusterSize: 284, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$gt": 13}}, {"a_compound": {"$all": [13, 14]}}, {"h_idx": {"$gte": 12}}],
                "h_idx": {"$lte": 11},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "d_compound": 1}},
    ],
    /* clusterSize: 283, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"z_compound": {"$gte": 14}},
                    {"k_compound": {"$nin": [8, 17, 2]}},
                    {"h_idx": {"$exists": True}},
                    {"i_compound": {"$exists": False}},
                ],
                "a_compound": {"$elemMatch": {"$lt": 6}},
            },
        },
        {"$sort": {"d_idx": -1, "z_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1, "c_noidx": 1}},
    ],
    /* clusterSize: 282, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"z_compound": {"$gte": 9}},
                    {"c_noidx": {"$in": [5, 17, 20]}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [11, 2, 11, 7]}},
                            {"a_noidx": {"$elemMatch": {"$in": [15, 4, 13]}}},
                        ],
                    },
                    {"c_noidx": {"$in": [12, 3, 14, 13]}},
                ],
                "a_compound": {"$exists": True},
                "i_compound": {"$exists": True},
            },
        },
        {"$project": {"a_noidx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 282, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$or": [{"c_idx": {"$eq": 17}}, {"a_noidx": {"$eq": 14}}]},
                    {"a_compound": {"$in": [3, 3]}},
                    {"c_idx": {"$exists": False}},
                    {
                        "$nor": [
                            {"a_idx": {"$all": [16, 13, 1]}},
                            {"a_compound": {"$all": [8, 14]}},
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                        ],
                    },
                ],
                "i_idx": {"$lt": 16},
            },
        },
        {"$limit": 144},
        {"$project": {"d_noidx": 1}},
    ],
    /* clusterSize: 279, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$nin": [9, 12]}}, {"a_compound": {"$lte": 18}}],
                "a_compound": {"$nin": [18, 11]},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 278, queryRank: 17.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_compound": {"$exists": True}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [17, 10, 16]}},
                            {"c_idx": {"$gt": 9}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$lte": 2}}},
                            {"c_idx": {"$nin": [1, 18, 6]}},
                        ],
                    },
                ],
                "a_compound": {"$exists": True},
                "k_compound": {"$in": [5, 14, 12, 16]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 277, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"i_compound": {"$in": [8, 14, 2]}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [14, 15, 7, 17]}},
                            {"a_idx": {"$elemMatch": {"$exists": False}}},
                            {"a_noidx": {"$elemMatch": {"$gte": 12, "$lte": 5}}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$in": [3, 9, 18]}},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$limit": 74},
        {"$skip": 30},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 276, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_idx": {"$in": [13, 1]}},
                    {"a_compound": {"$lte": 15}},
                    {"a_idx": {"$all": [5, 10, 8]}},
                    {"d_compound": {"$gt": 16}},
                ],
                "k_idx": {"$in": [2, 14, 20]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 32},
    ],
    /* clusterSize: 275, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$exists": False}},
                    {"$and": [{"a_compound": {"$all": [10, 8, 3]}}, {"k_compound": {"$ne": 13}}]},
                ],
                "a_idx": {"$all": [2, 3]},
            },
        },
        {"$limit": 187},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 274, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"c_compound": {"$in": [15, 17, 1, 8]}}, {"a_compound": {"$all": [4, 6, 4]}}],
                "a_idx": {"$all": [4, 18]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 274, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$gt": 1}}},
                    {"$or": [{"a_idx": {"$all": [16, 11, 2, 1]}}, {"a_compound": {"$exists": True}}]},
                ],
                "a_noidx": {"$nin": [1, 18]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 113},
    ],
    /* clusterSize: 271, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [{"i_compound": {"$ne": 18}}, {"k_compound": {"$nin": [10, 4, 5]}}],
                "a_compound": {"$all": [2, 8]},
            },
        },
    ],
    /* clusterSize: 268, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [9, 11]}},
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {
                        "$and": [
                            {"a_compound": {"$lt": 17}},
                            {"a_noidx": {"$all": [15, 4]}},
                            {"h_noidx": {"$exists": False}},
                        ],
                    },
                ],
                "a_compound": {"$in": [2, 11]},
                "a_idx": {"$elemMatch": {"$gte": 20}},
            },
        },
        {"$sort": {"d_idx": 1, "k_idx": 1}},
        {"$limit": 181},
        {"$project": {"_id": 0, "a_idx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 268, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$nin": [7, 7, 6]}, "c_compound": {"$nin": [18, 13, 18]}}},
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 268, queryRank: 6.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "c_compound": {"$exists": True}, "h_compound": {"$exists": True}}},
        {"$sort": {"c_idx": -1}},
        {"$limit": 226},
    ],
    /* clusterSize: 267, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [
                    {"h_idx": {"$ne": 10}},
                    {"d_compound": {"$eq": 2}},
                    {
                        "$nor": [
                            {"a_compound": {"$gte": 3}},
                            {"a_compound": {"$ne": 8}},
                            {"a_idx": {"$lte": 10}},
                            {"a_compound": {"$gt": 13}},
                        ],
                    },
                ],
                "c_noidx": {"$ne": 15},
            },
        },
        {"$sort": {"a_idx": 1, "c_idx": -1, "k_idx": -1, "z_idx": 1}},
        {"$limit": 109},
    ],
    /* clusterSize: 267, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [2, 15, 3]}},
                    {"h_noidx": {"$eq": 9}},
                    {"d_noidx": {"$exists": False}},
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 160},
    ],
    /* clusterSize: 264, queryRank: 5.03 */ [
        {"$match": {"i_compound": {"$nin": [2, 8]}, "z_compound": {"$nin": [18, 5]}}},
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 262, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [4, 2, 16, 10, 15]}},
                    {"a_idx": {"$all": [16, 8, 20, 13, 20]}},
                    {"a_idx": {"$eq": 5}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 261, queryRank: 19.02 */ [
        {
            "$match": {
                "$and": [{"c_compound": {"$in": [1, 6]}}, {"k_compound": {"$ne": 10}}],
                "$nor": [
                    {"i_noidx": {"$lt": 12}},
                    {
                        "$or": [
                            {"k_idx": {"$in": [16, 11]}},
                            {"a_compound": {"$eq": 1}},
                            {"a_compound": {"$in": [10, 9]}},
                            {"a_compound": {"$all": [20, 15, 8]}},
                        ],
                    },
                ],
            },
        },
        {"$project": {"a_idx": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 260, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$nin": [6, 11]}},
                    {"a_compound": {"$in": [5, 19, 14]}},
                    {"a_compound": {"$all": [4, 14, 15]}},
                ],
                "c_compound": {"$lt": 15},
            },
        },
        {"$sort": {"c_idx": 1, "k_idx": 1}},
        {"$limit": 113},
    ],
    /* clusterSize: 260, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_idx": {"$nin": [5, 12]}},
                    {"a_compound": {"$all": [4, 14, 4]}},
                    {"$or": [{"a_idx": {"$all": [1, 5]}}, {"a_compound": {"$elemMatch": {"$in": [19, 20, 14]}}}]},
                ],
            },
        },
        {"$sort": {"a_idx": 1, "z_idx": -1}},
        {"$limit": 158},
        {"$project": {"a_compound": 1, "h_compound": 1}},
    ],
    /* clusterSize: 260, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"i_compound": {"$nin": [8, 18, 13]}},
                    {"a_compound": {"$exists": True}},
                    {
                        "$or": [
                            {"z_noidx": {"$gt": 1}},
                            {"a_noidx": {"$elemMatch": {"$exists": True, "$in": [20, 20], "$nin": [2, 10]}}},
                            {"a_compound": {"$exists": True}},
                        ],
                    },
                    {
                        "$nor": [
                            {"z_noidx": {"$gte": 9}},
                            {"a_compound": {"$all": [8, 16, 12, 3, 17]}},
                            {"a_compound": {"$in": [5, 15, 12]}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"d_idx": 1, "z_idx": 1}},
        {"$limit": 86},
    ],
    /* clusterSize: 259, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$exists": False}}},
                    {
                        "$and": [
                            {"a_compound": {"$exists": True}},
                            {"d_idx": {"$exists": True}},
                            {"c_compound": {"$lte": 18}},
                        ],
                    },
                ],
                "d_idx": {"$lte": 8},
            },
        },
        {"$limit": 135},
    ],
    /* clusterSize: 259, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [{"i_compound": {"$nin": [20, 10, 2, 14]}}, {"a_compound": {"$in": [8, 18, 4]}}],
                "c_noidx": {"$nin": [2, 15, 17]},
                "k_compound": {"$nin": [12, 15]},
            },
        },
    ],
    /* clusterSize: 258, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [18, 14, 13, 1, 1]}}, {"c_noidx": {"$in": [9, 13, 5]}}],
                "a_compound": {"$lte": 9},
            },
        },
    ],
    /* clusterSize: 257, queryRank: 15.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [8, 6, 16]}},
                    {"a_compound": {"$nin": [16, 3, 19]}},
                    {"k_compound": {"$nin": [20, 3]}},
                ],
                "i_noidx": {"$lte": 3},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 257, queryRank: 15.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [5, 15, 13]}}, {"k_compound": {"$exists": True}}],
                "a_compound": {"$all": [6, 1]},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 255, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$elemMatch": {"$gte": 9, "$ne": 1}}},
                            {"k_compound": {"$in": [8, 13, 11, 17]}},
                        ],
                    },
                    {"z_compound": {"$nin": [20, 18, 19]}},
                    {
                        "$or": [
                            {"c_idx": {"$eq": 16}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$gt": 7, "$nin": [19, 13, 17]}}},
                            {"a_compound": {"$all": [19, 12]}},
                            {"h_idx": {"$lt": 2}},
                        ],
                    },
                ],
                "$or": [{"c_compound": {"$nin": [15, 7]}}, {"a_noidx": {"$exists": True}}],
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 132},
    ],
    /* clusterSize: 253, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_idx": {"$eq": 14}},
                    {"a_compound": {"$nin": [9, 11, 2]}},
                    {"$and": [{"k_compound": {"$in": [5, 10, 19]}}, {"a_compound": {"$ne": 6}}]},
                ],
                "a_noidx": {"$in": [1, 10, 13]},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 252, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$exists": False}},
                    {"k_idx": {"$exists": True}},
                    {"$or": [{"d_compound": {"$exists": True}}, {"a_idx": {"$all": [9, 5, 10]}}]},
                ],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1, "d_idx": 1, "h_idx": -1}},
        {"$limit": 186},
    ],
    /* clusterSize: 251, queryRank: 18.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$exists": False}},
                    {"k_compound": {"$lte": 1}},
                    {"a_compound": {"$all": [14, 10, 20, 12, 20]}},
                ],
                "a_compound": {"$all": [9, 19, 19]},
                "h_compound": {"$gt": 16},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1, "h_noidx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 249, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$nin": [19, 13]}}},
                    {"a_idx": {"$all": [5, 18]}},
                    {"i_compound": {"$gt": 11}},
                    {"d_idx": {"$gt": 18}},
                ],
                "h_idx": {"$nin": [18, 11, 19]},
            },
        },
    ],
    /* clusterSize: 248, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$lte": 2}}},
                            {
                                "$and": [
                                    {
                                        "$nor": [
                                            {"a_noidx": {"$elemMatch": {"$exists": False}}},
                                            {"z_noidx": {"$exists": True}},
                                        ],
                                    },
                                    {"a_idx": {"$elemMatch": {"$gte": 10}}},
                                ],
                            },
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                            {"i_compound": {"$exists": False}},
                        ],
                    },
                    {"k_compound": {"$gte": 20}},
                ],
                "a_compound": {"$eq": 2},
                "d_noidx": {"$eq": 8},
            },
        },
        {"$sort": {"d_idx": 1, "z_idx": 1}},
        {"$limit": 86},
        {"$skip": 5},
    ],
    /* clusterSize: 246, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$nin": [16, 2, 9, 15, 16]}},
                    {
                        "$and": [
                            {"c_compound": {"$exists": False}},
                            {"a_idx": {"$exists": False}},
                            {"a_idx": {"$all": [14, 7, 7, 4]}},
                        ],
                    },
                ],
                "a_compound": {"$gt": 15},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "c_idx": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 246, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$nor": [
                            {"i_compound": {"$exists": True}},
                            {"a_idx": {"$exists": True}},
                            {"a_noidx": {"$all": [8, 16, 14]}},
                            {"a_idx": {"$exists": False}},
                        ],
                    },
                    {
                        "$and": [
                            {"d_idx": {"$nin": [8, 3, 14]}},
                            {"a_compound": {"$all": [6, 3, 11, 1]}},
                            {"d_idx": {"$nin": [3, 9]}},
                        ],
                    },
                ],
                "c_noidx": {"$in": [1, 2]},
                "k_compound": {"$gt": 17},
            },
        },
    ],
    /* clusterSize: 243, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [15, 1, 15, 3]}},
                    {"k_compound": {"$nin": [19, 15]}},
                    {"a_compound": {"$all": [7, 9]}},
                ],
                "k_compound": {"$lte": 10},
            },
        },
        {"$limit": 209},
    ],
    /* clusterSize: 243, queryRank: 6.02 */ [
        {
            "$match": {
                "$nor": [{"d_idx": {"$eq": 8}}, {"a_idx": {"$eq": 2}}],
                "a_compound": {"$nin": [1, 1, 10]},
                "d_compound": {"$lte": 4},
            },
        },
        {"$limit": 146},
        {"$project": {"_id": 0, "a_idx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 243, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$gte": 19}}, {"h_noidx": {"$ne": 7}}, {"a_compound": {"$all": [6, 16, 1]}}],
                "i_compound": {"$lte": 18},
            },
        },
        {"$limit": 40},
    ],
    /* clusterSize: 238, queryRank: 9.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$exists": True}}, {"a_idx": {"$all": [11, 3, 7]}}],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1, "h_idx": -1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 234, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$gt": 15}}, {"a_compound": {"$ne": 8}}, {"a_idx": {"$all": [17, 17, 8]}}],
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 242},
        {"$project": {"_id": 0, "c_idx": 1}},
    ],
    /* clusterSize: 233, queryRank: 17.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"$and": [{"a_idx": {"$elemMatch": {"$exists": False}}}, {"c_compound": {"$nin": [7, 5]}}]},
                            {"c_compound": {"$in": [19, 14]}},
                        ],
                    },
                    {
                        "$or": [
                            {"a_idx": {"$all": [14, 5, 5]}},
                            {"a_idx": {"$exists": True}},
                            {"c_compound": {"$in": [16, 16]}},
                        ],
                    },
                    {"c_noidx": {"$exists": True}},
                ],
                "$nor": [
                    {"c_compound": {"$eq": 5}},
                    {"h_compound": {"$lt": 2}},
                    {
                        "$and": [
                            {"a_noidx": {"$lt": 19}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$in": [14, 16]}}},
                        ],
                    },
                    {"i_compound": {"$lt": 2}},
                ],
                "$or": [
                    {"i_idx": {"$exists": False}},
                    {"a_compound": {"$all": [12, 8, 14]}},
                    {"a_idx": {"$elemMatch": {"$eq": 9}}},
                    {"z_compound": {"$nin": [12, 20, 19, 11, 3]}},
                ],
            },
        },
    ],
    /* clusterSize: 233, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "c_idx": {"$exists": True}, "d_compound": {"$exists": True}}},
        {"$limit": 36},
    ],
    /* clusterSize: 232, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$nin": [13, 12]}}, {"a_compound": {"$all": [7, 6]}}],
                "h_idx": {"$nin": [3, 7, 6]},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 94},
        {"$project": {"_id": 0, "i_noidx": 1}},
    ],
    /* clusterSize: 229, queryRank: 17.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [8, 7]}},
                    {"d_compound": {"$lte": 20}},
                    {"$or": [{"a_compound": {"$nin": [14, 12, 3]}}, {"a_compound": {"$lt": 11}}]},
                ],
                "$or": [
                    {"a_compound": {"$all": [17, 8, 4]}},
                    {"k_idx": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$nin": [9, 13, 17]}}},
                ],
            },
        },
    ],
    /* clusterSize: 229, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [19, 9, 2]}}, {"k_compound": {"$exists": False}}],
                "a_compound": {"$eq": 5},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 229, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$ne": 3}}, {"k_compound": {"$exists": True}}],
                "d_compound": {"$lte": 19},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 229, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_compound": {"$gt": 1}},
                    {"$and": [{"h_compound": {"$ne": 18}}, {"i_idx": {"$ne": 12}}]},
                    {"a_idx": {"$lt": 11}},
                    {"d_idx": {"$nin": [6, 5]}},
                ],
                "$or": [
                    {"$and": [{"a_compound": {"$in": [10, 16]}}, {"a_idx": {"$all": [15, 6, 17]}}]},
                    {"$and": [{"c_compound": {"$exists": True}}, {"d_compound": {"$exists": False}}]},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"z_compound": {"$lte": 2}},
                ],
            },
        },
        {"$limit": 235},
    ],
    /* clusterSize: 228, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_noidx": {"$lte": 10}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$in": [8, 4, 1]}}},
                        ],
                    },
                    {
                        "$and": [
                            {"i_compound": {"$exists": True}},
                            {"i_idx": {"$exists": True}},
                            {"a_compound": {"$all": [14, 3, 4]}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "a_idx": {"$all": [1, 3]},
            },
        },
        {"$sort": {"a_idx": -1, "d_idx": -1}},
        {"$limit": 107},
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 227, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [18, 11]}}, {"d_compound": {"$exists": True}}],
                "a_idx": {"$all": [11, 1]},
            },
        },
    ],
    /* clusterSize: 225, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"i_noidx": {"$in": [7, 16]}}, {"a_compound": {"$all": [4, 3, 6, 15]}}],
                "a_compound": {"$elemMatch": {"$lt": 15}},
            },
        },
        {"$skip": 17},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 224, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [{"c_noidx": {"$gte": 19}}, {"a_compound": {"$all": [18, 1, 2]}}],
                "a_compound": {"$elemMatch": {"$lt": 18}},
            },
        },
        {"$skip": 13},
    ],
    /* clusterSize: 224, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [{"i_idx": {"$nin": [16, 6]}}, {"a_compound": {"$all": [13, 4, 3, 20]}}],
                "k_compound": {"$nin": [1, 7]},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 195},
    ],
    /* clusterSize: 222, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$in": [4, 17, 8]}},
                    {"$nor": [{"a_compound": {"$all": [16, 5, 18, 3]}}, {"d_idx": {"$exists": False}}]},
                ],
                "a_idx": {"$nin": [18, 2, 9]},
            },
        },
        {"$limit": 144},
        {"$project": {"_id": 0, "a_noidx": 1, "d_compound": 1}},
    ],
    /* clusterSize: 221, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$or": [
                            {"a_compound": {"$exists": False}},
                            {"a_idx": {"$elemMatch": {"$eq": 2, "$in": [11, 4, 7]}}},
                            {"a_compound": {"$all": [6, 4, 11, 19]}},
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$exists": False, "$gte": 19}}},
                ],
                "a_compound": {"$elemMatch": {"$in": [13, 11]}},
            },
        },
        {"$limit": 113},
    ],
    /* clusterSize: 219, queryRank: 12.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"z_compound": {"$lt": 19}},
                    {"a_compound": {"$in": [10, 15, 17]}},
                    {"c_idx": {"$exists": True}},
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 219, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"a_idx": {"$all": [13, 18]}},
                    {"a_compound": {"$all": [10, 1, 3, 12]}},
                ],
                "a_compound": {"$gte": 8},
                "a_idx": {"$elemMatch": {"$nin": [13, 7]}},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 218, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$nin": [9, 10]}}},
                    {"a_compound": {"$exists": False}},
                    {"c_compound": {"$exists": False}},
                ],
                "h_compound": {"$nin": [8, 13, 1]},
                "z_compound": {"$exists": True},
            },
        },
        {"$skip": 66},
    ],
    /* clusterSize: 217, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_noidx": {"$exists": False}},
                    {"z_compound": {"$eq": 18}},
                    {"a_noidx": {"$elemMatch": {"$in": [12, 9], "$nin": [17, 12]}}},
                    {"$or": [{"c_noidx": {"$eq": 15}}, {"h_idx": {"$in": [18, 18]}}]},
                ],
                "$or": [
                    {"a_idx": {"$elemMatch": {"$gt": 18, "$nin": [4, 5, 17, 7]}}},
                    {"a_compound": {"$in": [6, 10, 4]}},
                    {"a_compound": {"$lte": 11}},
                    {"c_compound": {"$exists": False}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 254},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 216, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [19, 2, 19]}},
                    {"i_noidx": {"$lte": 6}},
                    {"z_compound": {"$in": [19, 4, 8]}},
                ],
                "a_compound": {"$nin": [20, 20, 7]},
                "k_compound": {"$ne": 4},
            },
        },
        {"$sort": {"i_idx": 1, "k_idx": -1}},
        {"$limit": 70},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 214, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$in": [11, 11, 16, 14]}},
                    {"a_noidx": {"$eq": 3}},
                    {"a_compound": {"$all": [20, 2, 8]}},
                ],
                "a_compound": {"$in": [7, 12]},
            },
        },
        {"$project": {"d_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 213, queryRank: 6.03 */ [
        {
            "$match": {
                "c_compound": {"$nin": [13, 6]},
                "k_compound": {"$eq": 1},
                "z_compound": {"$nin": [16, 14, 18, 18]},
            },
        },
        {"$project": {"h_compound": 1}},
    ],
    /* clusterSize: 213, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$gte": 3}},
                    {"z_compound": {"$exists": True}},
                    {
                        "$or": [
                            {"a_compound": {"$lt": 16}},
                            {"a_compound": {"$in": [18, 1]}},
                            {"a_compound": {"$nin": [14, 4, 2, 20]}},
                            {"a_idx": {"$all": [3, 2]}},
                        ],
                    },
                ],
                "z_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$skip": 32},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 213, queryRank: 15.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$exists": True}},
                    {"z_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"a_idx": {"$all": [5, 8, 12]}},
                ],
                "a_compound": {"$all": [4, 3]},
            },
        },
        {"$sort": {"a_idx": 1, "c_idx": -1, "i_idx": 1}},
        {"$project": {"a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 213, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_compound": {"$all": [20, 13, 2, 18]}}, {"z_idx": {"$lt": 4}}]},
                    {"a_idx": {"$in": [11, 14]}},
                ],
                "i_compound": {"$lte": 11},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 213, queryRank: 7.03 */ [
        {
            "$match": {
                "$and": [
                    {"i_noidx": {"$in": [4, 4, 14]}},
                    {"$or": [{"a_compound": {"$lt": 17}}, {"c_compound": {"$gte": 13}}]},
                    {"a_compound": {"$elemMatch": {"$in": [20, 18, 4, 12]}}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 212, queryRank: 9.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_noidx": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"a_compound": {"$all": [13, 9, 3]}},
                ],
                "a_noidx": {"$elemMatch": {"$lte": 12}},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 212, queryRank: 6.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "k_compound": {"$exists": True}, "z_compound": {"$exists": True}}},
        {"$limit": 32},
        {"$project": {"a_idx": 1, "a_noidx": 1, "d_compound": 1, "z_compound": 1}},
    ],
    /* clusterSize: 212, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [2, 8]}},
                    {"a_idx": {"$all": [13, 11, 17, 14]}},
                    {"a_idx": {"$in": [13, 4]}},
                ],
                "h_idx": {"$eq": 9},
            },
        },
    ],
    /* clusterSize: 211, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [15, 5]}},
                    {"z_compound": {"$gt": 3}},
                    {
                        "$or": [
                            {"i_compound": {"$nin": [12, 6, 16, 19, 13, 10]}},
                            {"a_compound": {"$elemMatch": {"$eq": 17, "$exists": True}}},
                        ],
                    },
                    {"a_compound": {"$lte": 15}},
                ],
                "a_idx": {"$nin": [5, 7]},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 211, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_compound": {"$elemMatch": {"$in": [6, 14]}}}, {"a_idx": {"$eq": 6}}]},
                    {"a_idx": {"$elemMatch": {"$lte": 17, "$nin": [19, 16, 2]}}},
                    {"$or": [{"a_compound": {"$all": [17, 5]}}, {"a_compound": {"$all": [15, 15, 16, 5, 13]}}]},
                ],
                "a_compound": {"$elemMatch": {"$lte": 17}},
                "z_compound": {"$in": [15, 5]},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 211, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$nin": [6, 15]}}},
                    {"a_idx": {"$elemMatch": {"$gt": 9, "$gte": 6}}},
                    {
                        "$and": [
                            {
                                "$or": [
                                    {"a_idx": {"$all": [14, 1, 6]}},
                                    {"a_idx": {"$elemMatch": {"$lte": 3}}},
                                    {"d_idx": {"$in": [2, 19, 5]}},
                                    {"a_idx": {"$all": [15, 9, 3]}},
                                ],
                            },
                            {"a_compound": {"$nin": [8, 4, 18]}},
                        ],
                    },
                ],
                "k_idx": {"$nin": [3, 4]},
            },
        },
        {"$limit": 164},
        {"$skip": 55},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 210, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$nin": [17, 7, 16]}}, {"a_compound": {"$all": [17, 19, 3]}}],
                "a_compound": {"$gt": 15},
            },
        },
    ],
    /* clusterSize: 210, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [1, 1, 6, 11]}},
                    {"a_idx": {"$all": [7, 16, 4]}},
                    {"a_compound": {"$exists": True}},
                ],
                "a_compound": {"$lt": 8},
            },
        },
        {"$sort": {"a_idx": -1, "k_idx": -1}},
    ],
    /* clusterSize: 210, queryRank: 8.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_idx": {"$in": [16, 1, 8, 12]}},
                    {
                        "$or": [
                            {"h_idx": {"$exists": False}},
                            {"a_idx": {"$all": [16, 18]}},
                            {"a_compound": {"$exists": True}},
                        ],
                    },
                ],
                "a_idx": {"$gt": 11},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 15},
        {"$project": {"_id": 0, "a_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 208, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [9, 12]}}, {"a_compound": {"$exists": False}}],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1, "z_idx": -1}},
        {"$limit": 51},
    ],
    /* clusterSize: 208, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$in": [17, 8]}},
                    {"k_compound": {"$in": [17, 1, 8]}},
                    {"i_idx": {"$in": [19, 5]}},
                    {"a_compound": {"$all": [1, 8, 12]}},
                ],
                "h_compound": {"$in": [6, 7, 16]},
            },
        },
        {"$sort": {"k_idx": 1, "z_idx": 1}},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 208, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_compound": {"$exists": True}},
                    {"z_noidx": {"$exists": True}},
                    {"h_idx": {"$nin": [17, 10, 2, 18]}},
                ],
                "$or": [{"k_compound": {"$gt": 11}}, {"a_idx": {"$all": [15, 11, 2]}}, {"a_idx": {"$exists": True}}],
                "c_compound": {"$nin": [13, 12]},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 18},
        {"$project": {"a_compound": 1, "a_noidx": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 207, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"k_compound": {"$nin": [1, 3]}},
                    {"$or": [{"a_compound": {"$exists": True}}, {"a_compound": {"$all": [13, 3]}}]},
                    {"a_compound": {"$elemMatch": {"$in": [4, 1, 16, 11]}}},
                    {"a_idx": {"$elemMatch": {"$exists": True, "$nin": [9, 9, 14, 12, 13]}}},
                ],
                "$nor": [{"i_noidx": {"$eq": 1}}, {"d_compound": {"$exists": False}}],
            },
        },
        {"$limit": 106},
        {"$project": {"a_compound": 1, "a_idx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 206, queryRank: 9.03 */ [
        {"$match": {"a_compound": {"$all": [1, 1, 6]}, "h_idx": {"$nin": [7, 14, 16]}, "z_compound": {"$ne": 18}}},
        {"$sort": {"k_idx": -1}},
        {"$limit": 54},
    ],
    /* clusterSize: 206, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [5, 18, 3]}},
                    {"$or": [{"h_idx": {"$gte": 15}}, {"a_compound": {"$all": [16, 10]}}]},
                ],
                "i_idx": {"$nin": [4, 13]},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$skip": 3},
    ],
    /* clusterSize: 205, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [2, 10, 8, 5]}},
                    {"a_noidx": {"$eq": 15}},
                    {"k_compound": {"$in": [16, 14]}},
                    {"a_compound": {"$ne": 9}},
                ],
                "a_idx": {"$in": [12, 9, 10]},
            },
        },
        {"$limit": 81},
    ],
    /* clusterSize: 204, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [18, 4, 10]}},
                    {"a_idx": {"$elemMatch": {"$lte": 1}}},
                    {"h_idx": {"$nin": [9, 11]}},
                ],
                "a_compound": {"$gt": 18},
                "a_idx": {"$in": [5, 5]},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 36},
        {"$project": {"k_compound": 1, "k_idx": 1}},
    ],
    /* clusterSize: 203, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$exists": True}}, {"a_idx": {"$all": [5, 19, 12]}}],
                "h_compound": {"$in": [3, 6, 15, 9]},
            },
        },
        {"$project": {"a_noidx": 1, "c_idx": 1}},
    ],
    /* clusterSize: 203, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$in": [11, 3]}},
                    {"h_compound": {"$nin": [8, 14, 18]}},
                    {"a_compound": {"$all": [18, 17, 1]}},
                ],
                "z_compound": {"$exists": True},
            },
        },
        {"$limit": 211},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 201, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_idx": {"$exists": False}},
                    {
                        "$and": [
                            {"a_compound": {"$all": [14, 17, 18, 20]}},
                            {"a_compound": {"$all": [7, 13]}},
                            {"a_compound": {"$all": [2, 1, 5]}},
                            {"d_compound": {"$exists": True}},
                        ],
                    },
                ],
                "a_idx": {"$elemMatch": {"$ne": 16}},
                "d_compound": {"$lt": 6},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 201, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$exists": True}},
                    {"$and": [{"a_compound": {"$all": [19, 5]}}, {"k_noidx": {"$gte": 10}}]},
                    {"h_compound": {"$lt": 16}},
                ],
                "a_noidx": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 198, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$gte": 13}, "a_idx": {"$gt": 12}, "k_compound": {"$lt": 16}}},
        {"$limit": 37},
    ],
    /* clusterSize: 197, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [13, 17, 4]}},
                    {"a_compound": {"$all": [19, 20]}},
                    {"c_compound": {"$exists": True}},
                ],
                "c_compound": {"$nin": [20, 20, 13]},
                "d_idx": {"$exists": True},
            },
        },
        {"$limit": 144},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 196, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$lte": 2}}, {"i_compound": {"$eq": 20}}, {"a_idx": {"$all": [14, 17, 17, 1]}}],
                "a_idx": {"$gte": 10},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 196, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_idx": {"$eq": 3}},
                    {
                        "$nor": [
                            {"z_compound": {"$nin": [9, 20, 5, 6]}},
                            {"d_compound": {"$gte": 5}},
                            {"a_idx": {"$all": [4, 12]}},
                            {"c_compound": {"$exists": True}},
                        ],
                    },
                ],
                "a_compound": {"$all": [4, 4, 9]},
                "z_noidx": {"$gte": 4},
            },
        },
        {"$limit": 186},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 196, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [14, 17, 18, 12]}},
                    {"a_idx": {"$nin": [17, 12]}},
                    {"c_compound": {"$lt": 2}},
                ],
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 195, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$nin": [12, 1]}}, {"a_compound": {"$in": [8, 1]}}],
                "$or": [
                    {"z_compound": {"$gt": 12}},
                    {"a_idx": {"$all": [11, 1, 7]}},
                    {"a_compound": {"$nin": [9, 1, 13]}},
                ],
            },
        },
        {"$limit": 98},
        {"$project": {"a_idx": 1, "c_compound": 1}},
    ],
    /* clusterSize: 195, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$exists": True}},
                    {"c_idx": {"$lt": 10}},
                    {"a_compound": {"$all": [14, 10, 9]}},
                ],
                "a_compound": {"$lt": 20},
            },
        },
        {"$project": {"a_idx": 1, "c_idx": 1, "d_idx": 1}},
    ],
    /* clusterSize: 195, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [16, 1, 9]}}, {"d_compound": {"$exists": False}}],
                "a_compound": {"$exists": True},
                "a_idx": {"$elemMatch": {"$ne": 9}},
                "a_noidx": {"$elemMatch": {"$in": [11, 6], "$lt": 7}},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 49},
    ],
    /* clusterSize: 194, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [1, 8, 15]}},
                    {"a_idx": {"$exists": False}},
                    {"h_idx": {"$ne": 6}},
                    {"a_compound": {"$exists": False}},
                ],
                "h_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "i_idx": -1}},
        {"$limit": 112},
        {"$project": {"_id": 0, "z_compound": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 194, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [6, 5]}},
                    {"a_idx": {"$exists": True}},
                    {"i_compound": {"$exists": False}},
                ],
                "a_idx": {"$elemMatch": {"$exists": True, "$in": [2, 20]}},
                "k_compound": {"$in": [8, 16]},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 59},
        {"$project": {"c_idx": 1}},
    ],
    /* clusterSize: 194, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [8, 20, 3, 5]}},
                    {"a_idx": {"$exists": False}},
                    {"a_idx": {"$nin": [1, 14, 20]}},
                ],
                "h_compound": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 117},
    ],
    /* clusterSize: 193, queryRank: 18.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"a_idx": {"$nin": [10, 11]}},
                            {"a_compound": {"$all": [11, 1, 14]}},
                            {"k_compound": {"$in": [10, 10]}},
                        ],
                    },
                    {"z_compound": {"$nin": [5, 11]}},
                ],
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 7},
    ],
    /* clusterSize: 191, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"h_idx": {"$nin": [5, 3]}},
                    {"d_compound": {"$exists": True}},
                    {"i_compound": {"$nin": [6, 12, 18]}},
                    {"a_compound": {"$elemMatch": {"$eq": 15, "$nin": [6, 19, 7]}}},
                ],
                "a_compound": {"$in": [15, 20]},
                "k_idx": {"$gt": 4},
            },
        },
        {"$sort": {"d_idx": 1, "h_idx": -1, "k_idx": 1}},
    ],
    /* clusterSize: 191, queryRank: 17.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"a_idx": {"$elemMatch": {"$exists": False, "$lte": 10}}},
                            {"a_compound": {"$exists": False}},
                        ],
                    },
                    {"a_compound": {"$in": [14, 6, 5]}},
                    {"z_compound": {"$exists": True}},
                ],
                "$or": [
                    {"$and": [{"c_compound": {"$exists": False}}, {"d_compound": {"$exists": False}}]},
                    {"c_idx": {"$nin": [17, 5]}},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$in": [17, 9, 5], "$nin": [10, 1]}}},
                ],
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 185},
        {"$project": {"a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 189, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$in": [19, 7]}, "z_compound": {"$in": [15, 3, 13, 9, 2]}}},
    ],
    /* clusterSize: 188, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$in": [16, 3]}}, {"d_compound": {"$gte": 8}}],
                "c_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 54},
    ],
    /* clusterSize: 187, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [13, 6, 11]}}, {"a_idx": {"$nin": [5, 13, 13, 3]}}],
                "k_compound": {"$lt": 8},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 187, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$lt": 16}},
                            {"a_idx": {"$all": [9, 8, 12]}},
                            {"c_idx": {"$nin": [1, 5]}},
                            {"k_idx": {"$lt": 18}},
                        ],
                    },
                    {"a_compound": {"$nin": [20, 5, 13, 14]}},
                    {"a_idx": {"$nin": [2, 1]}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$skip": 76},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 187, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_idx": {"$all": [2, 5]}},
                            {"i_noidx": {"$exists": False}},
                            {"z_compound": {"$in": [10, 12]}},
                            {"a_idx": {"$nin": [11, 20]}},
                        ],
                    },
                    {"i_compound": {"$nin": [3, 6]}},
                ],
                "a_compound": {"$gte": 2},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$skip": 18},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 186, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [10, 1]}}, {"k_compound": {"$exists": True}}],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"h_idx": 1, "i_idx": -1}},
        {"$limit": 48},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 186, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$nin": [14, 16]}},
                    {"z_compound": {"$lte": 18}},
                    {"$or": [{"c_idx": {"$exists": False}}, {"a_compound": {"$gt": 16}}]},
                ],
                "i_compound": {"$nin": [5, 6, 7]},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$skip": 65},
    ],
    /* clusterSize: 186, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$gte": 20}}},
                    {"a_noidx": {"$gt": 15}},
                    {"$nor": [{"z_compound": {"$lte": 12}}, {"a_compound": {"$all": [18, 1, 7, 12, 6]}}]},
                ],
                "a_compound": {"$in": [14, 8, 13]},
            },
        },
        {"$sort": {"a_idx": -1, "c_idx": -1}},
        {"$limit": 173},
    ],
    /* clusterSize: 186, queryRank: 7.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$lte": 4}},
                    {"a_idx": {"$all": [20, 2]}},
                    {"$or": [{"h_compound": {"$gte": 10}}, {"h_idx": {"$gte": 3}}]},
                ],
                "d_noidx": {"$ne": 4},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$skip": 18},
        {"$project": {"_id": 0, "z_idx": 1}},
    ],
    /* clusterSize: 185, queryRank: 12.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$gt": 12}},
                    {"$or": [{"k_compound": {"$ne": 15}}, {"d_idx": {"$ne": 12}}, {"a_compound": {"$all": [12, 15]}}]},
                    {"a_compound": {"$in": [16, 13, 2, 14]}},
                ],
            },
        },
    ],
    /* clusterSize: 185, queryRank: 9.02 */ [
        {
            "$match": {
                "$or": [
                    {"h_compound": {"$gte": 17}},
                    {
                        "$and": [
                            {"z_compound": {"$gt": 13}},
                            {"z_noidx": {"$exists": True}},
                            {"k_idx": {"$exists": True}},
                        ],
                    },
                    {"d_compound": {"$nin": [6, 7, 16, 19]}},
                ],
                "a_compound": {"$gte": 15},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 9},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 183, queryRank: 8.03 */ [
        {"$match": {"a_compound": {"$all": [4, 14, 2]}}},
        {"$sort": {"a_idx": 1}},
        {"$limit": 22},
    ],
    /* clusterSize: 181, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$in": [14, 18, 12, 10]}},
                    {
                        "$and": [
                            {"c_compound": {"$gt": 12}},
                            {"z_noidx": {"$exists": True}},
                            {"a_noidx": {"$exists": False}},
                        ],
                    },
                    {"a_compound": {"$all": [20, 7]}},
                    {"a_compound": {"$gt": 5}},
                ],
                "a_idx": {"$gt": 5},
            },
        },
        {"$skip": 42},
        {"$project": {"_id": 0, "z_noidx": 1}},
    ],
    /* clusterSize: 180, queryRank: 5.03 */ [
        {"$match": {"i_compound": {"$nin": [11, 20, 7, 16]}, "k_compound": {"$in": [1, 3, 17]}}},
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 179, queryRank: 10.02 */ [
        {"$match": {"$or": [{"i_compound": {"$exists": True}}, {"a_compound": {"$all": [13, 16, 8]}}]}},
        {"$sort": {"k_idx": -1}},
        {"$skip": 87},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 179, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$all": [12, 1, 6]}},
                            {"a_idx": {"$elemMatch": {"$exists": True, "$in": [1, 1], "$ne": 1, "$nin": [16, 5]}}},
                            {"h_idx": {"$in": [11, 3, 11]}},
                            {"a_idx": {"$elemMatch": {"$in": [8, 20]}}},
                            {"a_idx": {"$exists": False}},
                            {"z_idx": {"$in": [7, 10]}},
                        ],
                    },
                    {
                        "$or": [
                            {
                                "$or": [
                                    {
                                        "$and": [
                                            {"d_idx": {"$lte": 5}},
                                            {"c_compound": {"$gte": 20}},
                                            {"d_noidx": {"$in": [18, 12, 5, 19]}},
                                        ],
                                    },
                                    {"a_compound": {"$all": [11, 9, 11]}},
                                    {"k_compound": {"$exists": True}},
                                    {"d_compound": {"$in": [14, 2, 11]}},
                                ],
                            },
                            {
                                "$and": [
                                    {"a_noidx": {"$elemMatch": {"$exists": True, "$gt": 1, "$in": [2, 12], "$lt": 19}}},
                                    {"a_noidx": {"$elemMatch": {"$lte": 4}}},
                                    {"a_idx": {"$exists": True}},
                                ],
                            },
                        ],
                    },
                ],
            },
        },
        {"$sort": {"k_idx": -1, "z_idx": 1}},
        {"$limit": 203},
    ],
    /* clusterSize: 178, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [{"c_idx": {"$ne": 10}}, {"a_compound": {"$all": [19, 20, 11]}}],
                "h_compound": {"$gte": 8},
            },
        },
        {"$project": {"_id": 0, "i_idx": 1}},
    ],
    /* clusterSize: 177, queryRank: 11.03 */ [
        {"$match": {"$or": [{"i_compound": {"$gt": 8}}, {"a_idx": {"$all": [1, 13, 2, 7]}}]}},
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 176, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$gt": 18}},
                    {
                        "$nor": [
                            {"c_compound": {"$nin": [16, 4, 14, 2, 12]}},
                            {"i_compound": {"$in": [2, 14, 11]}},
                            {"k_compound": {"$in": [8, 7]}},
                        ],
                    },
                ],
                "d_compound": {"$gte": 3},
            },
        },
        {"$limit": 28},
    ],
    /* clusterSize: 175, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {
                                "$and": [
                                    {"a_compound": {"$nin": [19, 19, 17]}},
                                    {"a_compound": {"$all": [7, 18]}},
                                    {"a_idx": {"$all": [20, 15]}},
                                ],
                            },
                            {"a_compound": {"$exists": False}},
                            {"c_compound": {"$in": [11, 3]}},
                        ],
                    },
                    {"h_compound": {"$nin": [14, 19]}},
                ],
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 92},
        {"$project": {"a_idx": 1, "c_idx": 1}},
    ],
    /* clusterSize: 175, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_idx": {"$nin": [4, 9, 7]}},
                    {"a_idx": {"$all": [9, 16]}},
                    {"a_compound": {"$exists": True}},
                ],
                "k_compound": {"$ne": 7},
            },
        },
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 174, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$gte": 15}}, {"a_compound": {"$all": [11, 5, 7, 12]}}],
                "a_compound": {"$elemMatch": {"$exists": True, "$in": [11, 6, 15]}},
                "d_idx": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1, "d_idx": -1, "z_idx": -1}},
        {"$project": {"_id": 0, "z_idx": 1}},
    ],
    /* clusterSize: 174, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [
                    {"i_idx": {"$ne": 16}},
                    {
                        "$or": [
                            {"a_idx": {"$all": [17, 12, 10]}},
                            {"d_compound": {"$ne": 5}},
                            {"a_compound": {"$ne": 16}},
                        ],
                    },
                ],
                "z_compound": {"$lte": 3},
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 174, queryRank: 7.03 */ [
        {
            "$match": {
                "$nor": [
                    {"d_noidx": {"$nin": [9, 5]}},
                    {"$nor": [{"a_idx": {"$all": [17, 2, 13, 20]}}, {"i_idx": {"$nin": [20, 11]}}]},
                ],
                "k_compound": {"$gte": 10},
            },
        },
    ],
    /* clusterSize: 170, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"i_compound": {"$exists": False}}, {"a_compound": {"$all": [14, 17, 15]}}],
                "d_compound": {"$in": [14, 12, 9, 8, 5]},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$limit": 229},
        {"$skip": 21},
    ],
    /* clusterSize: 169, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_idx": {"$nin": [11, 9, 16]}},
                    {"a_compound": {"$in": [2, 11]}},
                    {"d_compound": {"$nin": [19, 13]}},
                ],
                "$nor": [{"a_compound": {"$in": [3, 18]}}, {"a_compound": {"$in": [17, 12]}}],
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$project": {"h_compound": 1, "k_compound": 1}},
    ],
    /* clusterSize: 169, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"a_compound": {"$elemMatch": {"$exists": False}}},
                            {"h_compound": {"$in": [9, 4, 7]}},
                            {"c_noidx": {"$in": [15, 9]}},
                        ],
                    },
                    {
                        "$or": [
                            {"a_compound": {"$all": [1, 2, 4]}},
                            {"a_compound": {"$elemMatch": {"$ne": 4}}},
                            {"a_idx": {"$exists": True}},
                        ],
                    },
                ],
                "i_compound": {"$exists": True},
                "z_idx": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 169, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$nin": [4, 18]}},
                    {"a_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$nin": [7, 12, 16]}}},
                    {"a_compound": {"$exists": False}},
                ],
                "a_compound": {"$lt": 18},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$skip": 63},
    ],
    /* clusterSize: 166, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$in": [20, 6, 6]}},
                            {"a_compound": {"$all": [3, 20, 4]}},
                            {"a_compound": {"$nin": [16, 20, 14]}},
                        ],
                    },
                    {"k_compound": {"$ne": 9}},
                ],
                "$or": [
                    {
                        "$or": [
                            {"$or": [{"h_idx": {"$nin": [3, 1]}}, {"a_noidx": {"$gt": 2}}]},
                            {"i_noidx": {"$exists": True}},
                        ],
                    },
                    {
                        "$or": [
                            {"a_idx": {"$elemMatch": {"$exists": False}}},
                            {"h_noidx": {"$exists": False}},
                            {"c_compound": {"$nin": [13, 10]}},
                            {"h_idx": {"$exists": True}},
                        ],
                    },
                    {"$and": [{"a_compound": {"$in": [8, 19]}}, {"a_idx": {"$exists": True}}]},
                ],
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$skip": 20},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 166, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$lt": 13}},
                    {"d_compound": {"$lt": 16}},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$nin": [16, 5]}}},
                            {"z_idx": {"$gte": 1}},
                            {"a_compound": {"$all": [12, 18]}},
                        ],
                    },
                ],
                "a_idx": {"$lt": 7},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 211},
        {"$skip": 27},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 166, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$eq": 11}},
                    {"k_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$lte": 20}}},
                    {"a_compound": {"$in": [15, 18]}},
                ],
                "d_compound": {"$lte": 9},
            },
        },
        {"$limit": 241},
        {"$project": {"_id": 0, "a_compound": 1, "k_noidx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 165, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [2, 14]}}, {"z_idx": {"$exists": True}}, {"a_idx": {"$all": [19, 14, 13]}}],
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 165, queryRank: 10.02 */ [
        {"$match": {"$or": [{"a_compound": {"$all": [13, 7, 15]}}, {"k_compound": {"$ne": 6}}], "a_idx": {"$lte": 14}}},
        {"$limit": 243},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 165, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [11, 17, 8, 7, 12]}}, {"a_noidx": {"$eq": 13}}],
                "a_idx": {"$in": [14, 11, 4, 19]},
                "k_idx": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 164, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$nin": [3, 8]}},
                    {"a_compound": {"$in": [9, 5, 19]}},
                    {"a_noidx": {"$in": [11, 1, 18]}},
                ],
                "$or": [
                    {"i_compound": {"$ne": 6}},
                    {"a_compound": {"$all": [11, 8, 1]}},
                    {"$and": [{"k_idx": {"$ne": 2}}, {"a_noidx": {"$in": [2, 1]}}]},
                    {"z_idx": {"$gte": 10}},
                ],
                "c_compound": {"$nin": [19, 11, 19]},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 163, queryRank: 11.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$lt": 5}},
                            {"a_compound": {"$elemMatch": {"$ne": 4}}},
                            {"a_compound": {"$elemMatch": {"$eq": 4}}},
                            {"a_compound": {"$all": [13, 17, 16]}},
                        ],
                    },
                    {"c_idx": {"$in": [8, 1]}},
                ],
                "h_noidx": {"$in": [7, 15]},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 162, queryRank: 20.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {
                                "$nor": [
                                    {"z_compound": {"$exists": True}},
                                    {"a_idx": {"$elemMatch": {"$gt": 5}}},
                                    {"a_noidx": {"$all": [9, 15]}},
                                ],
                            },
                            {"i_compound": {"$gt": 7}},
                            {"a_idx": {"$elemMatch": {"$in": [12, 18]}}},
                            {"h_idx": {"$eq": 19}},
                            {
                                "$or": [
                                    {"z_compound": {"$eq": 10}},
                                    {
                                        "$or": [
                                            {"a_compound": {"$lte": 12}},
                                            {
                                                "$or": [
                                                    {"d_compound": {"$in": [16, 10, 8]}},
                                                    {"a_compound": {"$ne": 17}},
                                                ],
                                            },
                                            {"a_idx": {"$gte": 15}},
                                        ],
                                    },
                                ],
                            },
                        ],
                    },
                    {
                        "$or": [
                            {"k_compound": {"$exists": False}},
                            {"a_idx": {"$ne": 20}},
                            {"a_idx": {"$all": [4, 4, 7, 9]}},
                            {"k_compound": {"$lt": 8}},
                        ],
                    },
                ],
                "$or": [
                    {"a_idx": {"$in": [6, 14, 1]}},
                    {"$and": [{"a_noidx": {"$elemMatch": {"$nin": [20, 1, 11]}}}, {"a_compound": {"$all": [4, 10]}}]},
                    {"a_compound": {"$gte": 15}},
                ],
                "d_idx": {"$nin": [14, 17]},
            },
        },
    ],
    /* clusterSize: 161, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$nin": [4, 11, 6]}},
                    {"a_compound": {"$all": [16, 7]}},
                    {"a_noidx": {"$elemMatch": {"$in": [9, 17, 10]}}},
                ],
                "c_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"a_idx": 1, "a_noidx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 161, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$lt": 13}},
                    {"i_compound": {"$nin": [13, 11]}},
                    {"a_idx": {"$elemMatch": {"$in": [14, 4]}}},
                    {"a_compound": {"$all": [17, 2]}},
                ],
                "a_compound": {"$lt": 18},
                "z_compound": {"$in": [4, 13]},
            },
        },
    ],
    /* clusterSize: 159, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$nor": [{"a_compound": {"$lt": 8}}, {"a_idx": {"$all": [5, 2, 19, 4]}}]},
                    {"d_idx": {"$eq": 11}},
                ],
                "h_compound": {"$gte": 20},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 236},
    ],
    /* clusterSize: 159, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [19, 11, 4]}}, {"a_compound": {"$exists": False}}],
                "k_noidx": {"$gte": 8},
            },
        },
        {"$sort": {"a_idx": -1, "k_idx": -1}},
        {"$limit": 255},
        {"$skip": 44},
    ],
    /* clusterSize: 158, queryRank: 20.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"k_compound": {"$ne": 3}},
                            {"i_compound": {"$in": [3, 6]}},
                            {"a_compound": {"$exists": False}},
                        ],
                    },
                    {
                        "$or": [
                            {"a_idx": {"$all": [14, 13, 17]}},
                            {"a_idx": {"$exists": True}},
                            {"k_compound": {"$nin": [11, 5]}},
                        ],
                    },
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$skip": 55},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 156, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$exists": True}},
                    {"h_compound": {"$gte": 10}},
                    {"k_compound": {"$exists": False}},
                    {"i_compound": {"$nin": [10, 2]}},
                ],
                "i_noidx": {"$nin": [7, 4]},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$project": {"c_compound": 1, "c_idx": 1}},
    ],
    /* clusterSize: 156, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [2, 4]}},
                    {"a_compound": {"$elemMatch": {"$exists": True, "$in": [20, 2, 9, 18], "$nin": [20, 17]}}},
                    {"z_noidx": {"$exists": False}},
                ],
                "k_compound": {"$lt": 13},
            },
        },
    ],
    /* clusterSize: 155, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [14, 6, 15, 1]}}, {"a_idx": {"$gt": 7}}],
                "a_compound": {"$lte": 15},
            },
        },
        {"$sort": {"k_idx": -1, "z_idx": -1}},
    ],
    /* clusterSize: 154, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$exists": True}},
                    {"a_compound": {"$in": [9, 5, 2, 5]}},
                    {"k_idx": {"$gt": 10}},
                    {"d_idx": {"$exists": False}},
                ],
                "a_noidx": {"$gt": 18},
                "z_compound": {"$in": [17, 20, 1]},
            },
        },
        {"$sort": {"c_idx": 1, "h_idx": 1}},
        {"$limit": 162},
    ],
    /* clusterSize: 153, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [10, 3, 1]}}, {"a_compound": {"$eq": 14}}],
                "a_compound": {"$in": [1, 13, 8]},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 151, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"c_idx": {"$gte": 7}},
                    {"h_idx": {"$ne": 7}},
                    {"h_compound": {"$lt": 6}},
                    {"a_compound": {"$eq": 9}},
                ],
                "$or": [
                    {"k_compound": {"$exists": False}},
                    {"a_idx": {"$all": [6, 7]}},
                    {"z_idx": {"$nin": [5, 12, 11, 16]}},
                    {"$or": [{"a_idx": {"$elemMatch": {"$eq": 12}}}, {"d_compound": {"$gt": 8}}]},
                ],
                "a_idx": {"$elemMatch": {"$exists": True, "$lt": 20, "$ne": 14}},
            },
        },
        {"$limit": 241},
        {"$project": {"_id": 0, "k_noidx": 1}},
    ],
    /* clusterSize: 148, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_idx": {"$exists": False}},
                    {"k_compound": {"$exists": True}},
                    {"c_compound": {"$gt": 2}},
                    {"a_compound": {"$exists": False}},
                ],
                "z_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 84},
    ],
    /* clusterSize: 148, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$in": [6, 2]}},
                    {"a_compound": {"$all": [15, 16, 2, 3]}},
                    {"c_idx": {"$gt": 9}},
                ],
                "a_idx": {"$elemMatch": {"$gt": 8}},
                "z_compound": {"$nin": [7, 19, 18]},
            },
        },
        {"$sort": {"k_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 147, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"h_compound": {"$exists": False}},
                    {"a_idx": {"$elemMatch": {"$exists": False, "$nin": [7, 10]}}},
                ],
                "$or": [
                    {"k_compound": {"$exists": True}},
                    {"a_compound": {"$all": [16, 10]}},
                    {"a_idx": {"$elemMatch": {"$gt": 13}}},
                ],
                "a_idx": {"$nin": [7, 14, 15]},
                "d_compound": {"$in": [12, 18, 1]},
            },
        },
        {"$skip": 2},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 146, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"d_compound": {"$exists": False}}, {"a_compound": {"$all": [11, 8, 12, 15]}}],
                "a_noidx": {"$exists": True},
                "k_compound": {"$eq": 6},
            },
        },
        {"$limit": 89},
    ],
    /* clusterSize: 146, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_noidx": {"$nin": [10, 18, 10]}},
                    {"a_compound": {"$nin": [11, 12, 16, 4]}},
                    {"z_compound": {"$nin": [12, 16]}},
                ],
                "$nor": [{"a_compound": {"$all": [13, 5, 9, 3]}}, {"c_compound": {"$exists": False}}],
                "c_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "c_idx": -1}},
        {"$limit": 109},
        {"$project": {"_id": 0, "i_noidx": 1}},
    ],
    /* clusterSize: 145, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$exists": True}},
                    {"$and": [{"i_compound": {"$nin": [17, 8, 13]}}, {"z_noidx": {"$eq": 2}}]},
                    {"i_compound": {"$eq": 2}},
                ],
                "a_compound": {"$gte": 10},
            },
        },
        {"$limit": 131},
        {"$project": {"c_compound": 1, "d_compound": 1}},
    ],
    /* clusterSize: 144, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$lte": 9}},
                    {"$or": [{"a_idx": {"$all": [15, 10]}}, {"a_compound": {"$lte": 14}}]},
                    {"h_idx": {"$eq": 15}},
                ],
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 139},
    ],
    /* clusterSize: 143, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$gte": 4}},
                    {"a_compound": {"$all": [17, 5]}},
                    {"a_compound": {"$all": [8, 8]}},
                ],
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 142, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$ne": 16}},
                    {
                        "$nor": [
                            {"a_idx": {"$exists": False}},
                            {"a_idx": {"$lt": 2}},
                            {"k_idx": {"$eq": 14}},
                            {"a_compound": {"$all": [17, 12, 8]}},
                        ],
                    },
                ],
                "a_idx": {"$exists": True},
                "c_compound": {"$nin": [11, 7, 18, 17]},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"d_compound": 1}},
    ],
    /* clusterSize: 141, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [14, 16]}}, {"a_compound": {"$elemMatch": {"$in": [3, 18, 11, 2]}}}],
                "c_idx": {"$lt": 10},
                "h_noidx": {"$gte": 16},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$project": {"_id": 0, "z_noidx": 1}},
    ],
    /* clusterSize: 141, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$gt": 14}}},
                    {"k_idx": {"$exists": False}},
                    {"a_idx": {"$all": [18, 12, 6]}},
                    {"z_idx": {"$eq": 3}},
                    {"a_compound": {"$all": [15, 11, 7, 4]}},
                ],
                "a_compound": {"$in": [15, 11]},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 28},
    ],
    /* clusterSize: 141, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$lte": 10}},
                    {"$and": [{"c_compound": {"$lte": 7}}, {"z_compound": {"$nin": [19, 9]}}]},
                ],
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 140, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"a_noidx": {"$exists": False}}, {"d_idx": {"$in": [18, 5, 17]}}]},
                    {
                        "$or": [
                            {"k_compound": {"$exists": True}},
                            {"a_idx": {"$all": [6, 3, 8, 4]}},
                            {"a_idx": {"$all": [4, 1, 12]}},
                        ],
                    },
                ],
                "a_idx": {"$lt": 19},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$project": {"_id": 0, "c_compound": 1}},
    ],
    /* clusterSize: 140, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"c_idx": {"$gt": 3}},
                            {"a_compound": {"$nin": [2, 8]}},
                            {"c_idx": {"$in": [10, 20]}},
                            {
                                "$or": [
                                    {"a_compound": {"$all": [4, 15, 6]}},
                                    {"a_idx": {"$in": [12, 15, 15, 16]}},
                                    {"k_compound": {"$exists": False}},
                                    {"a_compound": {"$all": [14, 6, 5]}},
                                ],
                            },
                        ],
                    },
                    {"h_noidx": {"$in": [10, 3]}},
                ],
                "$or": [{"d_compound": {"$lte": 16}}, {"i_compound": {"$exists": False}}],
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 140, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$in": [5, 11]}},
                    {"$and": [{"h_noidx": {"$exists": False}}, {"i_compound": {"$in": [14, 12, 13]}}]},
                    {"z_compound": {"$nin": [7, 2]}},
                ],
                "a_compound": {"$nin": [9, 2, 17]},
            },
        },
        {"$sort": {"d_idx": -1, "i_idx": 1, "k_idx": -1}},
    ],
    /* clusterSize: 139, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [7, 19, 17]}}, {"i_compound": {"$exists": True}}],
                "a_compound": {"$lte": 8},
            },
        },
        {"$sort": {"k_idx": 1, "z_idx": -1}},
        {"$limit": 168},
    ],
    /* clusterSize: 139, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_idx": {"$all": [8, 16, 9, 19, 8, 6]}},
                            {
                                "$or": [
                                    {"a_compound": {"$all": [10, 17, 1, 16]}},
                                    {"a_noidx": {"$nin": [20, 6]}},
                                    {"a_noidx": {"$all": [3, 9]}},
                                    {"a_compound": {"$all": [10, 2, 20]}},
                                    {"a_idx": {"$ne": 9}},
                                ],
                            },
                        ],
                    },
                    {"i_compound": {"$nin": [1, 8, 10]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$gt": 8}},
            },
        },
        {"$skip": 76},
    ],
    /* clusterSize: 139, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"k_idx": {"$in": [11, 17]}},
                            {"d_compound": {"$lt": 10}},
                            {"a_compound": {"$all": [6, 5]}},
                        ],
                    },
                    {"c_compound": {"$ne": 14}},
                ],
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 152},
    ],
    /* clusterSize: 139, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$or": [{"a_compound": {"$all": [17, 16, 19, 20]}}, {"z_idx": {"$exists": False}}]},
                    {"z_idx": {"$exists": False}},
                    {"z_compound": {"$exists": False}},
                ],
                "a_compound": {"$nin": [14, 16]},
            },
        },
        {"$sort": {"a_idx": -1, "k_idx": -1}},
    ],
    /* clusterSize: 138, queryRank: 7.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$in": [9, 10, 4]}}, {"a_compound": {"$all": [13, 17]}}],
                "a_idx": {"$in": [20, 17, 2]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 137, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$and": [
                            {"a_noidx": {"$elemMatch": {"$ne": 2}}},
                            {"k_compound": {"$gte": 2}},
                            {"h_noidx": {"$lt": 5}},
                        ],
                    },
                    {"a_compound": {"$lte": 18}},
                    {"a_idx": {"$exists": True}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"h_compound": 1}},
    ],
    /* clusterSize: 137, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"c_idx": {"$lt": 12}},
                    {
                        "$and": [
                            {
                                "$or": [
                                    {"a_idx": {"$all": [10, 14, 1]}},
                                    {"a_compound": {"$in": [10, 18, 9]}},
                                    {"d_compound": {"$exists": True}},
                                ],
                            },
                            {"d_noidx": {"$lt": 13}},
                        ],
                    },
                ],
                "a_idx": {"$all": [16, 6]},
            },
        },
        {"$sort": {"h_idx": 1, "k_idx": -1}},
        {"$limit": 227},
        {"$project": {"z_idx": 1}},
    ],
    /* clusterSize: 136, queryRank: 7.03 */ [
        {"$match": {"$or": [{"a_idx": {"$all": [19, 15]}}, {"a_compound": {"$lt": 3}}], "z_noidx": {"$in": [2, 2, 1]}}},
        {"$sort": {"d_idx": -1}},
        {"$limit": 40},
    ],
    /* clusterSize: 136, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$nin": [10, 13]}}},
                    {"$and": [{"z_noidx": {"$nin": [7, 10, 4]}}, {"i_compound": {"$in": [6, 5, 17]}}]},
                    {"h_idx": {"$in": [3, 6, 7]}},
                    {"z_idx": {"$in": [13, 1]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "c_noidx": 1, "d_idx": 1}},
    ],
    /* clusterSize: 136, queryRank: 15.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$ne": 13}},
                    {
                        "$and": [
                            {"a_compound": {"$all": [10, 3, 19]}},
                            {"a_idx": {"$all": [4, 12, 5, 19]}},
                            {"i_compound": {"$ne": 7}},
                            {"a_noidx": {"$all": [2, 11]}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$in": [6, 3], "$lt": 9, "$nin": [9, 9]}},
                "a_noidx": {"$eq": 5},
                "k_compound": {"$nin": [11, 20]},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 135, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"z_idx": {"$ne": 6}},
                    {"a_idx": {"$all": [6, 18, 2]}},
                    {"a_compound": {"$all": [9, 8, 17, 15]}},
                ],
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$skip": 78},
    ],
    /* clusterSize: 135, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": False}},
                    {"z_compound": {"$exists": True}},
                    {"a_compound": {"$lte": 15}},
                ],
                "a_compound": {"$elemMatch": {"$in": [2, 14, 3]}},
            },
        },
        {"$sort": {"a_idx": -1, "d_idx": -1, "k_idx": -1}},
        {"$limit": 237},
    ],
    /* clusterSize: 134, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$nin": [14, 13]}}, {"a_idx": {"$all": [7, 1, 16, 5]}}],
                "a_idx": {"$eq": 16},
            },
        },
    ],
    /* clusterSize: 133, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"z_compound": {"$exists": False}}, {"a_compound": {"$all": [9, 13, 8, 9]}}],
                "a_compound": {"$elemMatch": {"$gte": 19}},
            },
        },
        {"$sort": {"i_idx": -1, "k_idx": 1}},
        {"$limit": 223},
        {"$skip": 66},
        {"$project": {"a_noidx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 130, queryRank: 11.03 */ [
        {"$match": {"$or": [{"a_idx": {"$all": [9, 7]}}, {"a_idx": {"$all": [1, 4]}}], "i_compound": {"$lte": 11}}},
        {"$sort": {"c_idx": -1, "i_idx": 1}},
        {"$limit": 12},
    ],
    /* clusterSize: 129, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$elemMatch": {"$ne": 14}}}, {"a_compound": {"$all": [8, 14]}}],
                "a_compound": {"$elemMatch": {"$exists": True}},
                "a_noidx": {"$in": [5, 12]},
                "d_idx": {"$lt": 14},
            },
        },
        {"$limit": 27},
    ],
    /* clusterSize: 129, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$nin": [20, 14, 1]}},
                    {"z_compound": {"$nin": [8, 11, 12]}},
                    {"a_idx": {"$all": [16, 4, 5, 14, 7]}},
                ],
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 129, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"a_compound": {"$exists": False}}, {"a_idx": {"$exists": False}}]},
                    {"d_compound": {"$lte": 18}},
                ],
                "k_compound": {"$nin": [9, 11, 15]},
            },
        },
        {"$sort": {"d_idx": 1, "z_idx": -1}},
    ],
    /* clusterSize: 128, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_idx": {"$lt": 11}},
                    {"a_idx": {"$elemMatch": {"$exists": True, "$in": [9, 5]}}},
                    {"a_compound": {"$gt": 3}},
                ],
                "$or": [
                    {"a_compound": {"$all": [4, 15, 10, 12, 19]}},
                    {"a_compound": {"$all": [9, 18, 17, 2]}},
                    {"a_compound": {"$nin": [14, 4, 6]}},
                ],
                "z_idx": {"$nin": [17, 3, 4, 12, 11]},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 114},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 128, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$ne": 7}},
                    {"a_compound": {"$elemMatch": {"$nin": [6, 16]}}},
                    {"k_compound": {"$gt": 7}},
                    {"z_compound": {"$in": [13, 11, 13, 10]}},
                ],
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 229},
    ],
    /* clusterSize: 128, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"c_compound": {"$lt": 7}}, {"a_idx": {"$all": [12, 2, 17]}}, {"a_idx": {"$all": [5, 18]}}],
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 127, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$nin": [16, 12, 19]}}, {"a_compound": {"$nin": [1, 4, 2, 6]}}],
                "$nor": [
                    {"$and": [{"a_compound": {"$all": [5, 10]}}, {"i_compound": {"$in": [9, 17, 2, 14]}}]},
                    {"i_noidx": {"$in": [6, 2, 5]}},
                    {"h_noidx": {"$exists": False}},
                ],
                "a_idx": {"$elemMatch": {"$exists": True}},
            },
        },
    ],
    /* clusterSize: 127, queryRank: 8.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [4, 11]}}, {"z_compound": {"$exists": False}}],
                "a_compound": {"$nin": [5, 5, 17]},
            },
        },
    ],
    /* clusterSize: 127, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [12, 5, 14]}},
                    {"a_compound": {"$gte": 6}},
                    {"k_compound": {"$exists": False}},
                ],
                "z_noidx": {"$nin": [5, 18]},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 125, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [7, 17]}},
                    {"a_compound": {"$all": [17, 14]}},
                    {"c_compound": {"$lt": 16}},
                ],
                "a_compound": {"$lte": 14},
                "h_noidx": {"$in": [8, 6]},
            },
        },
        {"$sort": {"h_idx": -1, "i_idx": -1}},
        {"$project": {"h_noidx": 1}},
    ],
    /* clusterSize: 125, queryRank: 11.03 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$all": [1, 6]}}, {"a_idx": {"$all": [14, 9, 3]}}]},
                    {"k_compound": {"$lte": 18}},
                ],
            },
        },
        {"$project": {"i_compound": 1, "z_compound": 1}},
    ],
    /* clusterSize: 125, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$lte": 4}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {
                        "$or": [
                            {"k_compound": {"$exists": True}},
                            {
                                "$or": [
                                    {"a_idx": {"$lte": 19}},
                                    {"c_idx": {"$nin": [16, 11, 4, 10]}},
                                    {"a_idx": {"$all": [2, 8, 16]}},
                                ],
                            },
                        ],
                    },
                    {"a_idx": {"$all": [6, 12, 8]}},
                ],
                "a_noidx": {"$elemMatch": {"$ne": 7}},
                "k_compound": {"$lte": 14},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 184},
        {"$project": {"_id": 0, "c_compound": 1}},
    ],
    /* clusterSize: 124, queryRank: 7.03 */ [
        {"$match": {"$nor": [{"a_compound": {"$all": [13, 13, 5]}}, {"d_idx": {"$gte": 17}}], "a_idx": {"$ne": 17}}},
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 124, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [
                    {"c_compound": {"$lte": 14}},
                    {"a_compound": {"$nin": [6, 8]}},
                    {"a_compound": {"$in": [16, 15, 14]}},
                    {"a_idx": {"$in": [14, 11]}},
                ],
                "a_idx": {"$nin": [17, 10]},
            },
        },
        {"$sort": {"a_idx": 1, "c_idx": 1}},
        {"$limit": 55},
    ],
    /* clusterSize: 124, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$in": [11, 15, 9]}},
                    {"a_noidx": {"$elemMatch": {"$ne": 15, "$nin": [20, 15, 13]}}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [19, 12, 20]}},
                            {"i_compound": {"$in": [8, 16]}},
                            {"a_compound": {"$exists": False}},
                        ],
                    },
                ],
                "a_idx": {"$in": [3, 1]},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"_id": 0, "z_compound": 1}},
    ],
    /* clusterSize: 124, queryRank: 15.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$nin": [15, 2]}},
                    {"$or": [{"a_compound": {"$all": [15, 8, 10]}}, {"i_compound": {"$in": [10, 4]}}]},
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$nin": [19, 11]}},
                "a_noidx": {"$exists": True},
                "i_compound": {"$ne": 15},
                "k_compound": {"$lt": 3},
            },
        },
        {"$sort": {"a_idx": -1, "k_idx": -1, "z_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 124, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$exists": True}}, {"a_idx": {"$all": [3, 7, 2, 11]}}],
                "i_compound": {"$in": [18, 1]},
            },
        },
        {"$limit": 36},
    ],
    /* clusterSize: 124, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_compound": {"$lte": 8}},
                    {"a_idx": {"$all": [12, 8]}},
                ],
                "a_compound": {"$elemMatch": {"$lt": 5, "$lte": 20, "$ne": 5}},
            },
        },
        {"$skip": 48},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1, "h_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 123, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [
                    {"h_compound": {"$nin": [8, 18, 7]}},
                    {
                        "$and": [
                            {"i_idx": {"$nin": [3, 16]}},
                            {
                                "$and": [
                                    {"a_idx": {"$in": [10, 11]}},
                                    {"a_compound": {"$all": [11, 3, 8, 17]}},
                                    {"h_compound": {"$in": [1, 13, 19, 6, 15]}},
                                ],
                            },
                            {"h_idx": {"$exists": True}},
                        ],
                    },
                ],
                "d_noidx": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "c_idx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 123, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [14, 6, 1, 5]}},
                    {"a_compound": {"$elemMatch": {"$lte": 16}}},
                    {"a_compound": {"$all": [7, 18, 20, 17]}},
                ],
                "a_compound": {"$elemMatch": {"$gte": 8}},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 206},
        {"$skip": 51},
        {"$project": {"_id": 0, "a_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 123, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_noidx": {"$exists": False}}, {"a_compound": {"$all": [9, 16, 15, 18]}}],
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "h_idx": -1}},
        {"$limit": 103},
    ],
    /* clusterSize: 122, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"c_compound": {"$lt": 2}},
                    {"a_noidx": {"$exists": True}},
                ],
                "$or": [{"a_compound": {"$exists": True}}, {"a_compound": {"$all": [5, 13]}}],
            },
        },
        {"$sort": {"a_idx": -1, "c_idx": 1}},
    ],
    /* clusterSize: 122, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [
                    {"c_idx": {"$in": [9, 6, 2]}},
                    {"$nor": [{"a_compound": {"$in": [20, 6]}}, {"a_compound": {"$in": [10, 8]}}]},
                    {"a_idx": {"$eq": 20}},
                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                ],
                "a_compound": {"$elemMatch": {"$in": [14, 3, 14, 18]}},
                "a_idx": {"$elemMatch": {"$nin": [8, 7, 20]}},
            },
        },
    ],
    /* clusterSize: 120, queryRank: 15.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_idx": {"$eq": 10}},
                    {"a_idx": {"$all": [2, 17, 16]}},
                    {"a_compound": {"$exists": True}},
                    {
                        "$or": [
                            {
                                "$and": [
                                    {"z_idx": {"$nin": [20, 14, 13, 14]}},
                                    {
                                        "$or": [
                                            {"a_compound": {"$elemMatch": {"$exists": True}}},
                                            {"d_compound": {"$exists": True}},
                                            {"a_idx": {"$exists": True}},
                                            {"a_noidx": {"$in": [4, 8]}},
                                        ],
                                    },
                                    {
                                        "$nor": [
                                            {"a_compound": {"$nin": [17, 14]}},
                                            {"a_compound": {"$nin": [12, 18, 17]}},
                                        ],
                                    },
                                    {"i_noidx": {"$nin": [14, 8, 2]}},
                                    {"d_noidx": {"$nin": [15, 10]}},
                                ],
                            },
                            {"a_idx": {"$all": [17, 14]}},
                        ],
                    },
                ],
                "a_compound": {"$all": [1, 4]},
            },
        },
        {"$sort": {"i_idx": 1, "k_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 120, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$exists": True, "$in": [6, 6]}}},
                    {"$or": [{"a_compound": {"$all": [4, 9, 3]}}, {"a_compound": {"$ne": 19}}]},
                    {"a_compound": {"$exists": True}},
                ],
                "$nor": [{"z_idx": {"$nin": [5, 20, 4]}}, {"k_idx": {"$eq": 11}}],
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 120, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$nin": [10, 7, 9, 19]}}, {"a_compound": {"$all": [11, 7]}}],
                "a_compound": {"$elemMatch": {"$in": [16, 1], "$nin": [4, 4]}},
                "c_noidx": {"$nin": [16, 7, 13]},
            },
        },
        {"$sort": {"a_idx": -1, "i_idx": 1}},
        {"$limit": 169},
        {"$project": {"_id": 0, "i_noidx": 1}},
    ],
    /* clusterSize: 120, queryRank: 15.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"k_noidx": {"$gte": 9}},
                            {"a_idx": {"$all": [10, 2]}},
                            {"k_idx": {"$in": [18, 14]}},
                            {"c_idx": {"$exists": False}},
                            {"k_idx": {"$ne": 11}},
                        ],
                    },
                    {"$or": [{"c_idx": {"$gte": 2}}, {"a_idx": {"$all": [11, 12]}}, {"d_compound": {"$exists": True}}]},
                ],
                "a_compound": {"$all": [2, 5]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 120, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"k_idx": {"$exists": False}}, {"a_idx": {"$all": [16, 13]}}, {"c_compound": {"$ne": 4}}]},
                    {"a_idx": {"$in": [18, 7, 8]}},
                ],
                "a_compound": {"$in": [11, 5, 9]},
            },
        },
    ],
    /* clusterSize: 119, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"k_idx": {"$lte": 2}},
                    {"a_idx": {"$lte": 12}},
                    {"$or": [{"a_idx": {"$all": [20, 12, 1]}}, {"i_compound": {"$nin": [19, 15]}}]},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$exists": True, "$nin": [5, 14]}}},
                            {"a_compound": {"$exists": False}},
                            {"i_idx": {"$eq": 3}},
                        ],
                    },
                ],
                "$or": [
                    {"$nor": [{"k_idx": {"$gt": 20}}, {"d_noidx": {"$gte": 12}}, {"h_noidx": {"$exists": False}}]},
                    {"a_idx": {"$nin": [4, 11, 18, 1]}},
                ],
                "c_compound": {"$nin": [9, 12]},
            },
        },
        {"$project": {"z_compound": 1}},
    ],
    /* clusterSize: 118, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$ne": 19}},
                    {
                        "$or": [
                            {"i_compound": {"$in": [15, 17, 10]}},
                            {"z_compound": {"$gt": 16}},
                            {"i_compound": {"$gt": 11}},
                            {"a_idx": {"$in": [20, 12]}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$nin": [14, 11, 6]}},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 117, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_idx": {"$eq": 1}},
                    {"c_compound": {"$nin": [13, 16, 16, 16, 7]}},
                    {"a_noidx": {"$nin": [14, 3, 8, 11]}},
                ],
                "$nor": [
                    {
                        "$nor": [
                            {"a_compound": {"$all": [15, 20, 14]}},
                            {"a_compound": {"$nin": [17, 17]}},
                            {
                                "$nor": [
                                    {"a_compound": {"$exists": True}},
                                    {"a_compound": {"$exists": True}},
                                    {"z_noidx": {"$exists": False}},
                                ],
                            },
                            {
                                "$or": [
                                    {"a_idx": {"$elemMatch": {"$exists": False, "$lte": 7, "$nin": [15, 9]}}},
                                    {"a_compound": {"$in": [3, 20, 14]}},
                                    {"i_idx": {"$gt": 3}},
                                    {"a_compound": {"$lt": 4}},
                                    {
                                        "$and": [
                                            {"a_compound": {"$elemMatch": {"$exists": False}}},
                                            {"c_noidx": {"$exists": False}},
                                            {"k_compound": {"$in": [6, 11, 2]}},
                                        ],
                                    },
                                ],
                            },
                        ],
                    },
                    {"h_compound": {"$lte": 6}},
                ],
                "i_compound": {"$exists": True},
            },
        },
        {"$limit": 41},
    ],
    /* clusterSize: 116, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [12, 7, 1, 3]}},
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$lt": 11}}},
                ],
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 115, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$lt": 17}},
                    {
                        "$nor": [
                            {"a_idx": {"$in": [18, 5, 20, 11]}},
                            {"z_compound": {"$exists": False}},
                            {"a_idx": {"$exists": False}},
                        ],
                    },
                ],
                "$or": [
                    {"a_compound": {"$all": [17, 3]}},
                    {
                        "$and": [
                            {"a_noidx": {"$elemMatch": {"$in": [15, 20]}}},
                            {"z_idx": {"$exists": True}},
                            {"i_noidx": {"$exists": False}},
                        ],
                    },
                    {"a_compound": {"$exists": True}},
                ],
            },
        },
        {"$limit": 19},
    ],
    /* clusterSize: 115, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$eq": 1}},
                    {"$and": [{"a_idx": {"$eq": 19}}, {"a_noidx": {"$all": [2, 13, 10]}}]},
                    {"c_compound": {"$ne": 13}},
                    {"a_idx": {"$all": [16, 2, 16]}},
                ],
                "z_compound": {"$exists": True},
                "z_noidx": {"$lt": 3},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 222},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 115, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"k_compound": {"$exists": True}},
                    {"a_compound": {"$lt": 17}},
                    {"$nor": [{"a_noidx": {"$nin": [7, 16]}}, {"a_compound": {"$all": [9, 12]}}]},
                ],
            },
        },
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 115, queryRank: 18.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$or": [
                            {"i_idx": {"$lt": 6}},
                            {"a_compound": {"$all": [14, 20, 15, 10]}},
                            {"d_noidx": {"$in": [8, 8]}},
                            {"c_compound": {"$gt": 6}},
                        ],
                    },
                    {"z_idx": {"$in": [12, 5, 17]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$in": [12, 19, 4, 13], "$nin": [2, 3]}},
                "a_idx": {"$lte": 6},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 210},
        {"$skip": 15},
    ],
    /* clusterSize: 114, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"z_compound": {"$nin": [10, 8]}},
                    {"a_compound": {"$exists": True}},
                    {"d_compound": {"$eq": 20}},
                ],
                "a_idx": {"$elemMatch": {"$lte": 17}},
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1, "d_idx": 1}},
        {"$limit": 100},
        {"$project": {"k_idx": 1}},
    ],
    /* clusterSize: 114, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_noidx": {"$lte": 8}},
                    {"$nor": [{"k_idx": {"$exists": False}}, {"c_idx": {"$exists": False}}]},
                ],
                "$or": [{"a_compound": {"$nin": [4, 19, 19]}}, {"a_idx": {"$all": [18, 15, 17, 16]}}],
                "k_compound": {"$nin": [2, 3, 11]},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 114, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [
                    {"d_compound": {"$lt": 13}},
                    {"a_idx": {"$lte": 2}},
                    {
                        "$or": [
                            {"d_compound": {"$ne": 18}},
                            {
                                "$and": [
                                    {"a_noidx": {"$all": [19, 3, 11]}},
                                    {"a_idx": {"$elemMatch": {"$nin": [2, 6]}}},
                                    {"a_idx": {"$all": [20, 14, 8]}},
                                ],
                            },
                        ],
                    },
                    {"h_compound": {"$lte": 16}},
                ],
                "a_compound": {"$in": [18, 12, 6]},
            },
        },
    ],
    /* clusterSize: 114, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$gte": 9}},
                    {"a_compound": {"$elemMatch": {"$in": [1, 20, 3], "$nin": [12, 12, 16]}}},
                ],
                "a_compound": {"$elemMatch": {"$in": [12, 4], "$lt": 16}},
                "k_compound": {"$nin": [14, 12, 4, 2]},
            },
        },
        {"$sort": {"d_idx": -1, "h_idx": 1}},
        {"$limit": 37},
    ],
    /* clusterSize: 113, queryRank: 6.02 */ [
        {"$match": {"$nor": [{"a_compound": {"$all": [8, 4]}}, {"k_noidx": {"$ne": 17}}], "a_idx": {"$lte": 17}}},
        {"$sort": {"c_idx": -1}},
        {"$limit": 83},
        {"$project": {"_id": 0, "d_idx": 1}},
    ],
    /* clusterSize: 113, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$in": [2, 8, 18]}}, {"a_compound": {"$in": [10, 10]}}],
                "a_compound": {"$lte": 1},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 113, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$lt": 10}}, {"a_idx": {"$all": [3, 14, 7]}}, {"a_idx": {"$all": [15, 7, 1]}}],
                "i_idx": {"$in": [20, 14]},
            },
        },
    ],
    /* clusterSize: 113, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"z_idx": {"$gt": 20}}, {"a_compound": {"$all": [3, 8, 11, 10]}}],
                "a_idx": {"$in": [13, 6]},
            },
        },
        {"$limit": 81},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 112, queryRank: 4.03 */ [{"$match": {"a_compound": {"$gte": 17}, "d_compound": {"$gte": 1}}}],
    /* clusterSize: 112, queryRank: 16.03 */ [
        {
            "$match": {
                "$or": [{"i_compound": {"$exists": True}}, {"a_idx": {"$all": [15, 5]}}],
                "a_compound": {"$all": [3, 19]},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 51},
    ],
    /* clusterSize: 112, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"c_noidx": {"$exists": False}}, {"a_compound": {"$all": [8, 18, 10, 3]}}],
                "a_compound": {"$elemMatch": {"$nin": [9, 7, 7]}},
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 111, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"h_noidx": {"$nin": [2, 2, 8]}}, {"d_compound": {"$exists": False}}]},
                    {"k_compound": {"$lte": 4}},
                    {"a_idx": {"$elemMatch": {"$nin": [16, 2]}}},
                    {"a_compound": {"$elemMatch": {"$nin": [13, 11, 18, 17]}}},
                ],
                "a_compound": {"$elemMatch": {"$in": [16, 2]}},
            },
        },
        {"$sort": {"d_idx": 1, "i_idx": -1, "k_idx": -1, "z_idx": 1}},
    ],
    /* clusterSize: 110, queryRank: 9.02 */ [
        {
            "$match": {
                "$nor": [{"d_noidx": {"$gte": 4}}, {"a_compound": {"$all": [15, 9, 18]}}],
                "i_idx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1, "k_idx": -1}},
        {"$skip": 3},
    ],
    /* clusterSize: 110, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$gte": 11}, "d_compound": {"$nin": [16, 6, 6]}}},
        {"$sort": {"i_idx": -1}},
        {"$limit": 120},
    ],
    /* clusterSize: 109, queryRank: 6.03 */ [
        {"$match": {"a_compound": {"$eq": 1}, "c_compound": {"$eq": 1}}},
        {"$sort": {"z_idx": 1}},
        {"$limit": 72},
    ],
    /* clusterSize: 109, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"h_idx": {"$lte": 13}},
                    {"a_compound": {"$all": [11, 12, 18, 4]}},
                    {"a_compound": {"$exists": False}},
                ],
                "a_idx": {"$lte": 6},
            },
        },
        {"$skip": 70},
    ],
    /* clusterSize: 109, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"$and": [{"a_noidx": {"$lt": 6}}, {"k_compound": {"$exists": True}}]},
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$lte": 20}},
                ],
                "$or": [
                    {
                        "$and": [
                            {"$nor": [{"a_noidx": {"$all": [8, 20, 3]}}, {"i_idx": {"$lt": 15}}]},
                            {"a_noidx": {"$exists": True}},
                            {"a_idx": {"$elemMatch": {"$exists": True, "$gt": 20, "$in": [12, 1]}}},
                            {
                                "$or": [
                                    {"z_idx": {"$gte": 14}},
                                    {"a_noidx": {"$in": [17, 1]}},
                                    {"k_idx": {"$ne": 7}},
                                    {"c_compound": {"$gt": 2}},
                                    {"c_idx": {"$ne": 16}},
                                ],
                            },
                            {"k_noidx": {"$nin": [6, 5, 2]}},
                        ],
                    },
                    {"$or": [{"a_idx": {"$all": [14, 18, 10]}}, {"i_compound": {"$gte": 3}}]},
                ],
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 108, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$gt": 3}}},
                    {"a_idx": {"$all": [6, 4, 13]}},
                    {"a_idx": {"$all": [7, 8]}},
                ],
                "a_compound": {"$elemMatch": {"$nin": [7, 12]}},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 231},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "d_idx": 1}},
    ],
    /* clusterSize: 108, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$nin": [6, 10, 3]}}},
                    {"i_compound": {"$exists": True}},
                    {
                        "$or": [
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                            {"a_compound": {"$elemMatch": {"$lt": 11}}},
                        ],
                    },
                ],
                "a_compound": {"$lt": 18},
            },
        },
    ],
    /* clusterSize: 108, queryRank: 19.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$elemMatch": {"$in": [6, 4]}}},
                    {"a_noidx": {"$exists": True}},
                    {
                        "$or": [
                            {"c_compound": {"$nin": [7, 3, 17]}},
                            {
                                "$and": [
                                    {"a_compound": {"$all": [12, 2, 16, 20]}},
                                    {"h_idx": {"$in": [15, 20, 3, 16]}},
                                    {"a_compound": {"$all": [14, 5, 4]}},
                                ],
                            },
                        ],
                    },
                ],
                "$or": [{"z_compound": {"$lt": 13}}, {"d_idx": {"$exists": True}}, {"a_compound": {"$all": [6, 7]}}],
            },
        },
        {"$sort": {"a_idx": -1, "z_idx": -1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 107, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$all": [19, 17, 8, 1]}}, {"i_compound": {"$ne": 11}}]},
                    {"i_noidx": {"$eq": 16}},
                ],
                "z_noidx": {"$in": [3, 16]},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$limit": 252},
        {"$project": {"a_noidx": 1, "c_noidx": 1}},
    ],
    /* clusterSize: 107, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"h_idx": {"$in": [13, 4, 11, 2]}},
                    {"i_noidx": {"$in": [20, 2, 6, 9]}},
                    {"a_compound": {"$all": [9, 1]}},
                    {"a_idx": {"$elemMatch": {"$exists": False, "$in": [19, 20]}}},
                ],
                "k_compound": {"$lt": 5},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 107, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [
                    {"c_compound": {"$gt": 12}},
                    {
                        "$nor": [
                            {
                                "$nor": [
                                    {"c_compound": {"$exists": False}},
                                    {"a_idx": {"$elemMatch": {"$exists": True, "$nin": [3, 5]}}},
                                ],
                            },
                            {"a_compound": {"$nin": [1, 7]}},
                        ],
                    },
                    {"a_compound": {"$in": [17, 12]}},
                ],
                "a_compound": {"$elemMatch": {"$in": [3, 16]}},
            },
        },
        {"$sort": {"c_idx": -1, "i_idx": -1}},
    ],
    /* clusterSize: 107, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$gte": 5}, "i_compound": {"$exists": True}}},
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 106, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [15, 20, 4]}}, {"h_compound": {"$gt": 13}}],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"a_idx": -1, "d_idx": -1, "k_idx": 1}},
        {"$limit": 85},
        {"$project": {"a_compound": 1, "a_idx": 1, "c_idx": 1}},
    ],
    /* clusterSize: 105, queryRank: 7.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$nin": [3, 20, 13]}},
                    {
                        "$and": [
                            {"d_compound": {"$in": [9, 19, 19, 15]}},
                            {"i_compound": {"$nin": [10, 3]}},
                            {"h_idx": {"$in": [2, 18, 8]}},
                            {"h_idx": {"$exists": False}},
                        ],
                    },
                    {"a_idx": {"$gt": 14}},
                ],
                "z_idx": {"$exists": True},
            },
        },
        {"$limit": 6},
    ],
    /* clusterSize: 105, queryRank: 9.03 */ [
        {"$match": {"$or": [{"a_idx": {"$all": [5, 7, 5, 12]}}, {"a_compound": {"$gt": 5}}]}},
        {"$sort": {"d_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 105, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$nin": [15, 15]}},
                    {"a_idx": {"$in": [1, 14]}},
                    {"a_noidx": {"$nin": [6, 5]}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [8, 11, 16]}},
                            {"a_compound": {"$in": [11, 16]}},
                            {"k_compound": {"$in": [8, 17, 3, 4]}},
                        ],
                    },
                ],
            },
        },
        {"$limit": 178},
    ],
    /* clusterSize: 105, queryRank: 7.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$nin": [4, 20]}}, {"a_idx": {"$all": [9, 11, 15, 4]}}],
                "a_compound": {"$elemMatch": {"$eq": 8, "$nin": [9, 18]}},
            },
        },
        {"$project": {"k_compound": 1}},
    ],
    /* clusterSize: 104, queryRank: 15.03 */ [
        {
            "$match": {
                "$and": [
                    {"d_compound": {"$exists": True}},
                    {"a_noidx": {"$exists": True}},
                    {"a_idx": {"$in": [16, 18, 11]}},
                ],
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$exists": False, "$in": [9, 17]}}},
                    {"c_idx": {"$eq": 19}},
                    {"a_compound": {"$all": [10, 12, 3]}},
                ],
                "a_compound": {"$elemMatch": {"$eq": 11}},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 104, queryRank: 6.02 */ [
        {"$match": {"$or": [{"z_compound": {"$ne": 15}}, {"z_compound": {"$eq": 16}}], "a_compound": {"$lt": 2}}},
        {"$skip": 9},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1, "c_compound": 1}},
    ],
    /* clusterSize: 104, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$nin": [10, 15]}}, {"a_compound": {"$all": [7, 6, 10]}}],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1, "z_idx": 1}},
        {"$project": {"a_compound": 1, "a_noidx": 1, "z_compound": 1}},
    ],
    /* clusterSize: 104, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$lt": 2}}, {"a_compound": {"$all": [8, 4, 8]}}],
                "a_compound": {"$in": [6, 14, 8]},
            },
        },
        {"$limit": 159},
    ],
    /* clusterSize: 104, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"c_compound": {"$in": [20, 18]}},
                    {"a_compound": {"$all": [11, 18, 1]}},
                    {"k_compound": {"$exists": False}},
                ],
                "h_idx": {"$nin": [4, 20, 14]},
            },
        },
        {"$project": {"k_compound": 1}},
    ],
    /* clusterSize: 104, queryRank: 5.02 */ [
        {
            "$match": {
                "a_idx": {"$nin": [5, 11]},
                "i_compound": {"$exists": True},
                "k_idx": {"$nin": [8, 13, 18, 7, 19]},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 114},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 103, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$or": [
                            {"z_compound": {"$exists": False}},
                            {"a_compound": {"$nin": [10, 9]}},
                            {"a_compound": {"$in": [17, 20, 20]}},
                            {"a_noidx": {"$elemMatch": {"$in": [4, 5]}}},
                        ],
                    },
                    {"a_noidx": {"$all": [16, 14, 9]}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 102, queryRank: 7.02 */ [
        {
            "$match": {
                "$nor": [{"z_compound": {"$nin": [12, 11, 20, 1]}}, {"d_compound": {"$nin": [9, 17, 10]}}],
                "c_compound": {"$nin": [20, 4, 7]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"a_idx": 1, "k_noidx": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 102, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$gte": 7}},
                    {"a_idx": {"$all": [17, 17, 16]}},
                    {"a_idx": {"$all": [13, 16, 19]}},
                ],
                "a_idx": {"$elemMatch": {"$nin": [14, 2]}},
            },
        },
        {"$limit": 174},
        {"$project": {"i_compound": 1, "z_idx": 1}},
    ],
    /* clusterSize: 102, queryRank: 9.02 */ [
        {
            "$match": {
                "$and": [
                    {"i_idx": {"$nin": [6, 19, 19]}},
                    {"$or": [{"a_idx": {"$lte": 4}}, {"a_idx": {"$all": [20, 5, 4, 4]}}]},
                    {"$and": [{"a_noidx": {"$lte": 19}}, {"a_compound": {"$ne": 18}}]},
                    {"a_idx": {"$elemMatch": {"$exists": True, "$in": [3, 19]}}},
                    {"a_idx": {"$nin": [8, 8, 1, 12, 13]}},
                ],
            },
        },
        {"$limit": 145},
        {"$skip": 4},
    ],
    /* clusterSize: 102, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$in": [14, 8, 15]}}},
                            {"a_idx": {"$nin": [11, 17]}},
                            {"z_idx": {"$exists": True}},
                            {"i_compound": {"$nin": [17, 14]}},
                            {"a_idx": {"$all": [4, 19]}},
                        ],
                    },
                    {"c_compound": {"$exists": True}},
                ],
                "i_compound": {"$nin": [8, 18]},
            },
        },
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 102, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [15, 12]}}, {"a_noidx": {"$in": [11, 9, 20]}}],
                "k_compound": {"$nin": [12, 6, 4]},
            },
        },
    ],
    /* clusterSize: 102, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$elemMatch": {"$in": [19, 4, 14]}}}, {"a_compound": {"$all": [7, 3, 17]}}],
                "k_compound": {"$nin": [8, 13]},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 34},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 101, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_compound": {"$elemMatch": {"$nin": [19, 14]}}},
                            {"a_idx": {"$all": [1, 6]}},
                            {"a_noidx": {"$in": [9, 7, 3]}},
                            {"a_noidx": {"$elemMatch": {"$nin": [7, 5, 7, 10, 19]}}},
                        ],
                    },
                    {"c_compound": {"$nin": [20, 19]}},
                ],
                "d_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 101, queryRank: 15.03 */ [
        {
            "$match": {
                "$and": [
                    {"d_compound": {"$exists": True}},
                    {"$or": [{"z_compound": {"$gte": 10}}, {"d_idx": {"$in": [5, 20]}}]},
                    {"a_compound": {"$in": [6, 3, 19, 10]}},
                ],
                "$or": [{"a_compound": {"$exists": True}}, {"a_compound": {"$all": [8, 9, 11]}}],
            },
        },
        {"$limit": 105},
    ],
    /* clusterSize: 101, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$nin": [11, 9]}}},
                    {"a_compound": {"$lte": 15}},
                    {"a_compound": {"$all": [8, 20]}},
                ],
                "a_compound": {"$elemMatch": {"$in": [16, 14, 15]}},
            },
        },
    ],
    /* clusterSize: 101, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"i_compound": {"$eq": 15}},
                    {"a_compound": {"$all": [13, 12]}},
                ],
                "k_compound": {"$in": [6, 17, 5, 18]},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 101, queryRank: 17.02 */ [
        {
            "$match": {
                "$nor": [
                    {"z_noidx": {"$in": [3, 18, 17]}},
                    {"d_idx": {"$gt": 5}},
                    {"a_compound": {"$exists": False}},
                    {"a_compound": {"$all": [13, 20, 5]}},
                    {"d_compound": {"$exists": False}},
                ],
                "k_compound": {"$nin": [8, 13]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "d_noidx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 101, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [8, 3, 1]}}, {"d_noidx": {"$exists": False}}],
                "d_noidx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "h_idx": 1, "k_idx": -1}},
        {"$skip": 55},
    ],
    /* clusterSize: 101, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {"a_compound": {"$all": [6, 13, 8]}},
                            {"a_compound": {"$all": [7, 16, 19]}},
                            {"i_idx": {"$nin": [1, 2]}},
                        ],
                    },
                    {"h_idx": {"$gte": 10}},
                    {"a_idx": {"$exists": False}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$nin": [16, 9, 10, 17]}},
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1, "z_idx": 1}},
    ],
    /* clusterSize: 101, queryRank: 6.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$elemMatch": {"$exists": False, "$nin": [19, 18, 5]}}},
                    {"a_idx": {"$elemMatch": {"$exists": False}}},
                    {"d_noidx": {"$gte": 8}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$nin": [4, 10]}},
                "a_idx": {"$elemMatch": {"$exists": True}},
                "k_compound": {"$ne": 15},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 100, queryRank: 7.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"$and": [{"k_idx": {"$in": [8, 20]}}, {"c_compound": {"$nin": [16, 16, 15]}}]},
                    {"c_compound": {"$nin": [14, 14, 9, 18]}},
                ],
                "a_compound": {"$ne": 17},
                "c_noidx": {"$exists": True},
                "d_idx": {"$nin": [9, 1, 3]},
            },
        },
    ],
    /* clusterSize: 100, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$elemMatch": {"$gt": 13}}},
                    {"$and": [{"c_compound": {"$lte": 19}}, {"a_compound": {"$elemMatch": {"$nin": [2, 9]}}}]},
                    {"$and": [{"z_noidx": {"$nin": [7, 9]}}, {"k_compound": {"$gte": 1}}]},
                ],
                "a_compound": {"$elemMatch": {"$gte": 16}},
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "k_idx": 1}},
    ],
    /* clusterSize: 100, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_idx": {"$nin": [2, 14]}},
                    {
                        "$or": [
                            {"h_idx": {"$in": [19, 11, 8, 10, 5]}},
                            {"a_compound": {"$all": [3, 18]}},
                            {"a_compound": {"$all": [16, 1]}},
                        ],
                    },
                ],
                "i_compound": {"$ne": 20},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 89},
    ],
    /* clusterSize: 100, queryRank: 9.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$all": [20, 1, 3]}},
                            {"c_idx": {"$nin": [3, 16]}},
                            {"k_idx": {"$nin": [17, 8]}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$gt": 16}}},
                ],
            },
        },
    ],
    /* clusterSize: 100, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"c_compound": {"$nin": [3, 3, 12]}},
                    {"d_compound": {"$ne": 7}},
                    {"a_idx": {"$all": [5, 16]}},
                ],
                "a_idx": {"$nin": [11, 11, 20]},
                "k_compound": {"$ne": 17},
            },
        },
    ],
    /* clusterSize: 99, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [{"a_noidx": {"$nin": [8, 8, 4]}}, {"a_compound": {"$all": [20, 6, 11]}}],
                "a_compound": {"$in": [5, 8, 11]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 99, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [7, 7]}}, {"a_compound": {"$all": [5, 11, 20, 3]}}],
                "a_compound": {"$elemMatch": {"$exists": True, "$gt": 1}},
            },
        },
        {"$project": {"c_compound": 1}},
    ],
    /* clusterSize: 99, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"k_idx": {"$exists": False}}, {"a_compound": {"$all": [12, 19, 16, 4]}}],
                "a_compound": {"$exists": True},
            },
        },
        {"$limit": 39},
        {"$project": {"_id": 0, "a_compound": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 98, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$in": [15, 9]}},
                    {"k_compound": {"$nin": [8, 4]}},
                    {"a_compound": {"$elemMatch": {"$eq": 14, "$exists": True}}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "d_idx": 1}},
    ],
    /* clusterSize: 98, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$nin": [15, 5, 20, 11, 6]}}},
                    {"a_idx": {"$all": [18, 11]}},
                    {"i_compound": {"$exists": True}},
                ],
                "a_compound": {"$exists": True},
                "k_compound": {"$gte": 18},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 92},
        {"$project": {"_id": 0, "a_noidx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 98, queryRank: 4.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$eq": 9, "$exists": True}},
                "a_idx": {"$elemMatch": {"$gte": 20}},
                "k_idx": {"$gt": 12},
            },
        },
    ],
    /* clusterSize: 97, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$ne": 16}}},
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$nin": [15, 1, 9]}}},
                    {"z_compound": {"$exists": True}},
                ],
            },
        },
    ],
    /* clusterSize: 97, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [3, 11, 5, 10]}}, {"c_compound": {"$eq": 9}}],
                "$or": [
                    {"a_compound": {"$lt": 10}},
                    {"a_compound": {"$all": [5, 12]}},
                    {"a_idx": {"$all": [1, 14, 15]}},
                ],
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 236},
    ],
    /* clusterSize: 97, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$eq": 15}},
                    {"a_compound": {"$nin": [1, 14, 18]}},
                    {"a_compound": {"$all": [18, 15, 10]}},
                ],
                "a_noidx": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 97, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$elemMatch": {"$exists": False}}}, {"a_noidx": {"$nin": [11, 20, 17]}}],
                "$or": [
                    {"c_idx": {"$lte": 1}},
                    {
                        "$or": [
                            {"k_idx": {"$exists": True}},
                            {"k_idx": {"$nin": [5, 2]}},
                            {"a_compound": {"$all": [18, 17, 15, 8, 1, 4]}},
                        ],
                    },
                ],
                "h_compound": {"$nin": [18, 20]},
                "k_noidx": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 96, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$nin": [4, 18]}}},
                    {"a_idx": {"$all": [14, 4]}},
                    {"a_idx": {"$all": [6, 9]}},
                ],
                "a_compound": {"$in": [16, 2, 3, 15]},
                "a_idx": {"$in": [11, 11]},
            },
        },
        {"$project": {"c_compound": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 96, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"c_compound": {"$lte": 10}}, {"a_noidx": {"$exists": True}}]},
                    {"a_compound": {"$all": [11, 5, 1, 6]}},
                ],
                "a_idx": {"$elemMatch": {"$lt": 15}},
                "d_idx": {"$lte": 2},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 95, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$elemMatch": {"$in": [9, 3, 12, 14]}}}, {"a_idx": {"$all": [15, 1, 7]}}],
                "a_compound": {"$in": [11, 16]},
            },
        },
    ],
    /* clusterSize: 95, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {
                        "$and": [
                            {"a_compound": {"$all": [4, 14, 20]}},
                            {"a_idx": {"$all": [18, 17]}},
                            {"a_compound": {"$nin": [13, 17, 12]}},
                        ],
                    },
                ],
                "z_compound": {"$exists": True},
            },
        },
        {"$limit": 95},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 95, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$nin": [15, 16, 14, 2]}},
                    {"k_compound": {"$in": [14, 16, 19]}},
                    {"a_compound": {"$all": [6, 12, 8]}},
                ],
                "a_compound": {"$gt": 12},
            },
        },
        {"$sort": {"c_idx": 1, "k_idx": -1}},
        {"$limit": 82},
        {"$skip": 20},
    ],
    /* clusterSize: 94, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [5, 19, 2]}},
                    {"$or": [{"c_compound": {"$exists": False}}, {"d_noidx": {"$eq": 17}}]},
                ],
                "a_compound": {"$nin": [5, 13, 5]},
            },
        },
        {"$sort": {"a_idx": 1, "z_idx": 1}},
        {"$limit": 27},
        {"$skip": 11},
    ],
    /* clusterSize: 94, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$gte": 8}}},
                    {"a_noidx": {"$in": [12, 8]}},
                    {
                        "$and": [
                            {"a_compound": {"$elemMatch": {"$exists": True, "$lte": 6}}},
                            {"a_idx": {"$nin": [10, 3, 1]}},
                            {"i_noidx": {"$nin": [1, 6, 12]}},
                        ],
                    },
                ],
                "a_idx": {"$lte": 14},
            },
        },
        {"$limit": 126},
    ],
    /* clusterSize: 94, queryRank: 7.03 */ [
        {"$match": {"$or": [{"a_compound": {"$ne": 13}}, {"a_idx": {"$all": [12, 6]}}]}},
        {"$sort": {"h_idx": 1}},
        {"$project": {"a_idx": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 94, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [1, 9]}}, {"a_idx": {"$all": [18, 15]}}, {"a_idx": {"$gte": 12}}],
                "d_idx": {"$in": [15, 9]},
            },
        },
        {"$sort": {"a_idx": -1, "i_idx": -1}},
        {"$limit": 195},
    ],
    /* clusterSize: 94, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [4, 1, 15, 5]}},
                    {"$or": [{"a_noidx": {"$in": [18, 13]}}, {"a_idx": {"$elemMatch": {"$gt": 18}}}]},
                ],
                "h_noidx": {"$exists": True},
            },
        },
        {"$limit": 49},
        {"$project": {"h_noidx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 94, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [1, 6]}},
                    {"c_idx": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "h_compound": {"$lte": 7},
            },
        },
    ],
    /* clusterSize: 93, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"a_compound": {"$exists": True}}, {"i_idx": {"$exists": True}}]},
                    {"$and": [{"a_compound": {"$exists": False}}, {"z_idx": {"$eq": 16}}]},
                    {"k_compound": {"$lte": 4}},
                ],
                "h_noidx": {"$gte": 13},
                "k_compound": {"$nin": [17, 7, 7]},
            },
        },
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 93, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [{"i_compound": {"$lte": 7}}, {"a_compound": {"$in": [20, 12]}}, {"a_idx": {"$exists": False}}],
                "a_compound": {"$in": [5, 20]},
                "k_compound": {"$nin": [10, 7, 20]},
            },
        },
        {"$sort": {"c_idx": -1, "z_idx": 1}},
        {"$limit": 126},
    ],
    /* clusterSize: 93, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$exists": True}},
                    {"a_idx": {"$all": [7, 9, 13, 10]}},
                    {"c_compound": {"$nin": [17, 13]}},
                ],
                "a_compound": {"$lte": 14},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$skip": 52},
    ],
    /* clusterSize: 93, queryRank: 12.03 */ [
        {"$match": {"$or": [{"a_idx": {"$all": [5, 2]}}, {"a_compound": {"$all": [9, 10]}}], "a_compound": {"$gt": 3}}},
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 93, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [9, 15, 13]}}, {"i_compound": {"$lte": 12}}],
                "h_compound": {"$nin": [15, 20, 14]},
            },
        },
    ],
    /* clusterSize: 92, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"z_compound": {"$exists": False}}, {"a_compound": {"$all": [10, 16, 15]}}],
                "a_compound": {"$all": [3, 3]},
                "a_idx": {"$nin": [4, 18, 17]},
            },
        },
    ],
    /* clusterSize: 92, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$in": [8, 1, 17, 15, 3]}},
                    {"k_idx": {"$lt": 1}},
                    {"a_idx": {"$elemMatch": {"$in": [16, 8, 3]}}},
                ],
                "$or": [
                    {"$and": [{"k_idx": {"$nin": [7, 5, 19, 20]}}, {"a_idx": {"$all": [16, 15, 10]}}]},
                    {"z_idx": {"$exists": True}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [14, 11, 4, 12]}},
                            {"$nor": [{"a_compound": {"$all": [2, 16]}}, {"a_idx": {"$all": [3, 3]}}]},
                            {"a_compound": {"$elemMatch": {"$exists": False}}},
                            {"a_compound": {"$nin": [20, 3]}},
                        ],
                    },
                    {"a_idx": {"$all": [9, 16]}},
                ],
            },
        },
        {"$sort": {"c_idx": 1, "h_idx": -1, "z_idx": 1}},
        {"$limit": 200},
        {"$project": {"_id": 0, "a_noidx": 1, "h_noidx": 1, "z_compound": 1}},
    ],
    /* clusterSize: 92, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [{"d_compound": {"$gte": 20}}, {"a_compound": {"$all": [4, 8]}}],
                "a_compound": {"$lte": 11},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$project": {"a_noidx": 1, "i_compound": 1, "z_compound": 1, "z_idx": 1}},
    ],
    /* clusterSize: 92, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {
                                "$or": [
                                    {"a_compound": {"$in": [6, 19, 15]}},
                                    {"a_compound": {"$exists": True}},
                                    {"c_compound": {"$gte": 19}},
                                ],
                            },
                            {"z_noidx": {"$exists": False}},
                            {"$and": [{"a_idx": {"$lt": 6}}, {"a_compound": {"$elemMatch": {"$lte": 20}}}]},
                            {"a_idx": {"$all": [8, 20]}},
                        ],
                    },
                    {"a_idx": {"$exists": True}},
                ],
                "h_idx": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 114},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 92, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"i_compound": {"$nin": [1, 18]}}, {"a_idx": {"$all": [19, 13, 6]}}],
                "i_compound": {"$nin": [6, 17]},
                "i_idx": {"$lte": 12},
            },
        },
    ],
    /* clusterSize: 92, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"c_noidx": {"$in": [7, 17, 9, 4]}}, {"a_noidx": {"$exists": False}}],
                "$or": [
                    {
                        "$nor": [
                            {"z_compound": {"$in": [3, 12]}},
                            {"a_idx": {"$elemMatch": {"$exists": True, "$gt": 20}}},
                        ],
                    },
                    {"$nor": [{"i_noidx": {"$gt": 9}}, {"c_idx": {"$exists": False}}]},
                    {"a_compound": {"$gte": 16}},
                    {"$nor": [{"h_idx": {"$in": [20, 7]}}, {"a_compound": {"$exists": True}}]},
                    {
                        "$and": [
                            {"a_idx": {"$all": [7, 4, 16, 13, 3]}},
                            {
                                "$nor": [
                                    {"a_idx": {"$elemMatch": {"$exists": False}}},
                                    {"a_noidx": {"$elemMatch": {"$gt": 3, "$in": [9, 2]}}},
                                ],
                            },
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 44},
    ],
    /* clusterSize: 91, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"z_compound": {"$nin": [20, 5, 13, 2]}},
                            {"a_compound": {"$in": [2, 1]}},
                            {"i_compound": {"$eq": 3}},
                        ],
                    },
                    {"i_idx": {"$gt": 1}},
                ],
                "k_compound": {"$in": [18, 11, 1, 11]},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 91, queryRank: 7.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$nin": [5, 13, 14]}},
                    {
                        "$and": [
                            {"h_compound": {"$in": [19, 13]}},
                            {"$nor": [{"i_noidx": {"$in": [6, 9]}}, {"i_compound": {"$ne": 9}}]},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 91, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"k_compound": {"$gt": 17}},
                            {"a_idx": {"$exists": True}},
                            {"a_idx": {"$all": [12, 15, 2]}},
                        ],
                    },
                    {"a_compound": {"$all": [9, 1]}},
                    {"h_idx": {"$nin": [16, 11, 20]}},
                ],
                "a_noidx": {"$nin": [16, 7, 13, 10]},
            },
        },
    ],
    /* clusterSize: 91, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$in": [2, 1, 18]}}},
                    {"a_idx": {"$all": [6, 3, 14, 16]}},
                    {"a_idx": {"$all": [12, 3]}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1, "k_idx": 1}},
        {"$project": {"a_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 90, queryRank: 7.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$eq": 12}}, {"d_compound": {"$lte": 14}}],
                "h_compound": {"$lte": 5},
                "h_idx": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 89, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"h_idx": {"$nin": [19, 10, 4, 8]}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_idx": {"$all": [17, 4, 1, 6, 18]}},
                    {"a_idx": {"$exists": True}},
                ],
                "a_idx": {"$in": [14, 3, 6]},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 89, queryRank: 15.03 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$exists": False}},
                    {"k_compound": {"$gt": 13}},
                    {"a_compound": {"$all": [13, 1, 18, 6, 14]}},
                    {"a_idx": {"$elemMatch": {"$exists": False, "$in": [4, 14]}}},
                ],
                "a_idx": {"$all": [2, 8]},
                "a_noidx": {"$exists": True},
            },
        },
        {"$limit": 248},
    ],
    /* clusterSize: 88, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$elemMatch": {"$in": [5, 14]}}}, {"a_compound": {"$all": [12, 17, 13]}}],
                "a_compound": {"$lt": 4},
                "z_noidx": {"$in": [1, 11, 16]},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 56},
    ],
    /* clusterSize: 88, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$ne": 1}},
                    {"i_idx": {"$nin": [10, 6, 12]}},
                    {
                        "$nor": [
                            {
                                "a_compound": {
                                    "$elemMatch": {
                                        "$exists": True,
                                        "$in": [12, 18, 3, 11, 18],
                                        "$ne": 4,
                                        "$nin": [17, 17, 9, 20],
                                    },
                                },
                            },
                            {"c_compound": {"$ne": 6}},
                        ],
                    },
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$project": {"a_compound": 1, "h_idx": 1}},
    ],
    /* clusterSize: 88, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"i_compound": {"$in": [4, 18]}},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"a_compound": {"$all": [18, 15, 10]}},
                    {"a_compound": {"$exists": False}},
                ],
                "a_noidx": {"$lt": 3},
                "d_idx": {"$gt": 4},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 3},
    ],
    /* clusterSize: 88, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$gte": 3}, "k_compound": {"$nin": [6, 9, 8, 11]}}},
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 87, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$ne": 7, "$nin": [17, 17, 1]}}},
                    {
                        "$and": [
                            {"a_idx": {"$all": [7, 13, 19, 10, 5]}},
                            {"z_compound": {"$exists": True}},
                            {"$nor": [{"a_idx": {"$nin": [12, 20, 15, 20]}}, {"a_compound": {"$gte": 19}}]},
                            {"a_compound": {"$elemMatch": {"$in": [4, 13]}}},
                        ],
                    },
                ],
                "a_compound": {"$gte": 20},
                "z_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 211},
        {"$project": {"z_compound": 1}},
    ],
    /* clusterSize: 87, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_idx": {"$lte": 3}},
                    {"a_compound": {"$all": [18, 17, 9, 6, 5]}},
                    {"a_compound": {"$lte": 10}},
                ],
                "a_noidx": {"$lte": 4},
                "i_noidx": {"$nin": [6, 17]},
                "z_noidx": {"$lte": 17},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 231},
    ],
    /* clusterSize: 86, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$exists": True}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$gte": 20, "$lt": 11, "$lte": 6}}},
                            {"z_idx": {"$nin": [14, 16]}},
                        ],
                    },
                    {"a_compound": {"$gt": 20}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"c_compound": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 86, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"h_idx": {"$in": [4, 12]}}, {"a_idx": {"$all": [10, 13, 17]}}],
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"d_compound": {"$exists": False}},
                    {"a_idx": {"$in": [13, 14, 5]}},
                    {"c_compound": {"$ne": 13}},
                    {"$or": [{"i_compound": {"$in": [10, 7, 7, 4]}}, {"c_compound": {"$eq": 7}}]},
                ],
                "i_compound": {"$nin": [4, 8]},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 81},
    ],
    /* clusterSize: 85, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$all": [8, 1]}},
                            {"a_compound": {"$exists": True}},
                            {"h_idx": {"$exists": False}},
                            {"c_compound": {"$exists": False}},
                            {"a_idx": {"$in": [14, 6, 4]}},
                        ],
                    },
                    {"a_noidx": {"$nin": [15, 12]}},
                ],
                "$or": [
                    {"$and": [{"i_noidx": {"$nin": [7, 10, 8, 15]}}, {"a_idx": {"$elemMatch": {"$gte": 9}}}]},
                    {"c_compound": {"$nin": [15, 5]}},
                ],
                "a_compound": {"$in": [5, 18]},
            },
        },
        {"$sort": {"h_idx": 1, "z_idx": -1}},
        {"$limit": 211},
        {"$skip": 7},
    ],
    /* clusterSize: 84, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"i_compound": {"$nin": [10, 10]}}, {"a_noidx": {"$all": [15, 15, 2]}}]},
                    {"a_compound": {"$lt": 5}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$skip": 99},
    ],
    /* clusterSize: 84, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$in": [19, 10, 8]}}, {"a_noidx": {"$ne": 18}}],
                "$or": [
                    {"h_idx": {"$exists": False}},
                    {"$nor": [{"i_compound": {"$eq": 12}}, {"z_idx": {"$exists": True}}]},
                    {"a_compound": {"$all": [16, 16]}},
                    {"a_idx": {"$exists": True}},
                ],
                "a_compound": {"$in": [1, 4, 12, 14]},
            },
        },
    ],
    /* clusterSize: 84, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$lte": 10}}, {"a_idx": {"$lte": 2}}, {"z_noidx": {"$exists": True}}],
                "$nor": [
                    {"a_idx": {"$elemMatch": {"$exists": False, "$lte": 4}}},
                    {"$or": [{"a_idx": {"$elemMatch": {"$exists": False}}}, {"a_noidx": {"$gte": 17}}]},
                    {"a_compound": {"$all": [11, 1]}},
                    {"k_compound": {"$lt": 15}},
                ],
            },
        },
        {"$limit": 50},
    ],
    /* clusterSize: 84, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$ne": 15}}, {"a_compound": {"$all": [16, 9]}}],
                "a_compound": {"$elemMatch": {"$in": [16, 19, 5]}},
            },
        },
        {"$skip": 9},
    ],
    /* clusterSize: 83, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"i_idx": {"$gte": 10}}, {"a_compound": {"$all": [7, 11, 19, 18, 12]}}],
                "a_compound": {"$elemMatch": {"$lt": 2, "$lte": 3}},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 147},
    ],
    /* clusterSize: 83, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [5, 8, 6]}},
                    {"a_idx": {"$elemMatch": {"$exists": False, "$lt": 5}}},
                    {"a_compound": {"$lt": 18}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 152},
    ],
    /* clusterSize: 83, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [
                    {"i_compound": {"$exists": False}},
                    {"c_noidx": {"$in": [20, 6]}},
                    {"a_compound": {"$all": [2, 6, 20]}},
                ],
                "a_idx": {"$elemMatch": {"$gte": 2, "$in": [5, 13, 7]}},
            },
        },
        {"$sort": {"i_idx": 1, "k_idx": 1}},
        {"$limit": 64},
        {"$project": {"a_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 82, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$elemMatch": {"$exists": True}}}, {"a_compound": {"$all": [11, 15, 14]}}],
                "a_compound": {"$in": [2, 1, 19]},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 122},
    ],
    /* clusterSize: 82, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$exists": False}},
                    {"a_compound": {"$exists": False}},
                    {"d_noidx": {"$nin": [11, 6, 1]}},
                ],
                "$or": [
                    {"z_compound": {"$ne": 13}},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$in": [18, 12], "$lt": 6}}},
                    {"a_compound": {"$nin": [15, 19, 12]}},
                    {"$and": [{"h_compound": {"$exists": False}}, {"a_compound": {"$all": [10, 1]}}]},
                ],
                "a_compound": {"$gte": 10},
            },
        },
        {"$limit": 238},
    ],
    /* clusterSize: 82, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [{"k_compound": {"$gte": 9}}, {"z_idx": {"$lte": 7}}, {"z_noidx": {"$in": [2, 20, 2]}}],
                "$or": [
                    {"$and": [{"a_compound": {"$nin": [18, 10, 3, 17]}}, {"a_noidx": {"$nin": [5, 15, 15]}}]},
                    {"d_compound": {"$in": [6, 5]}},
                    {"h_compound": {"$exists": False}},
                    {"k_compound": {"$in": [18, 4, 16]}},
                ],
                "z_noidx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 176},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 81, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"i_compound": {"$nin": [1, 8]}},
                    {"k_noidx": {"$ne": 11}},
                    {"z_compound": {"$gt": 4}},
                    {
                        "$or": [
                            {
                                "$or": [
                                    {"a_compound": {"$elemMatch": {"$nin": [17, 1, 11]}}},
                                    {"a_compound": {"$elemMatch": {"$eq": 3, "$exists": True}}},
                                    {"a_compound": {"$all": [7, 20, 17, 9]}},
                                ],
                            },
                            {"z_idx": {"$nin": [12, 3]}},
                        ],
                    },
                ],
            },
        },
        {"$limit": 118},
    ],
    /* clusterSize: 81, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"i_compound": {"$exists": False}},
                    {
                        "$or": [
                            {"a_idx": {"$gte": 12}},
                            {"z_noidx": {"$gte": 14}},
                            {"c_compound": {"$eq": 19}},
                            {"a_compound": {"$in": [9, 17, 13]}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$lte": 5}},
                "k_compound": {"$lte": 12},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 80, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$gt": 17}},
                    {"a_idx": {"$gte": 8}},
                    {
                        "$and": [
                            {"z_idx": {"$lt": 3}},
                            {"$or": [{"a_compound": {"$elemMatch": {"$eq": 4}}}, {"a_idx": {"$all": [16, 12, 6]}}]},
                            {"$nor": [{"c_idx": {"$in": [16, 10, 11, 14]}}, {"i_compound": {"$in": [14, 7]}}]},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 22},
        {"$skip": 5},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 80, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$elemMatch": {"$exists": True}}}, {"a_compound": {"$exists": True}}],
                "c_compound": {"$ne": 3},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 78},
    ],
    /* clusterSize: 80, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$exists": True}}, {"a_compound": {"$lte": 2}}],
                "$nor": [{"k_noidx": {"$in": [17, 1]}}, {"a_idx": {"$exists": False}}],
                "$or": [{"i_compound": {"$nin": [20, 3]}}, {"a_compound": {"$elemMatch": {"$exists": False}}}],
                "a_compound": {"$gt": 17},
                "h_compound": {"$ne": 16},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 186},
        {"$skip": 36},
    ],
    /* clusterSize: 80, queryRank: 17.03 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$nin": [5, 19]}}, {"a_compound": {"$all": [8, 2, 8]}}],
                "$or": [{"a_compound": {"$ne": 9}}, {"a_idx": {"$all": [5, 4, 7, 18, 12]}}],
                "d_idx": {"$ne": 19},
            },
        },
        {"$limit": 241},
    ],
    /* clusterSize: 80, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"c_compound": {"$exists": True}}, {"a_compound": {"$all": [13, 14, 15, 18, 4]}}]},
                    {"c_compound": {"$lt": 13}},
                ],
                "a_compound": {"$exists": True},
                "a_idx": {"$elemMatch": {"$nin": [9, 1]}},
            },
        },
        {"$sort": {"c_idx": 1, "k_idx": 1}},
        {"$limit": 130},
        {"$skip": 60},
    ],
    /* clusterSize: 80, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$in": [13, 16]}}, {"a_compound": {"$elemMatch": {"$exists": True}}}],
                "i_compound": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 79, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$exists": False}}, {"a_idx": {"$ne": 6}}, {"z_idx": {"$in": [11, 18]}}]},
                    {"k_idx": {"$exists": True}},
                ],
                "k_compound": {"$lte": 12},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1, "c_compound": 1}},
    ],
    /* clusterSize: 79, queryRank: 11.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$nin": [5, 5]}},
                    {
                        "$and": [
                            {"a_idx": {"$lt": 3}},
                            {"c_compound": {"$ne": 20}},
                            {"a_compound": {"$in": [4, 12, 2, 16]}},
                            {"k_idx": {"$lte": 16}},
                        ],
                    },
                ],
                "$nor": [
                    {"d_compound": {"$exists": False}},
                    {"a_noidx": {"$nin": [12, 12, 14]}},
                    {"a_idx": {"$all": [20, 6, 11]}},
                ],
                "i_idx": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": -1, "z_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1, "c_idx": 1, "d_noidx": 1}},
    ],
    /* clusterSize: 77, queryRank: 21.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$all": [4, 2]}},
                    {"a_compound": {"$exists": True}},
                    {"k_idx": {"$nin": [1, 1]}},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$eq": 14}}},
                            {"a_compound": {"$all": [4, 3, 2, 13]}},
                            {"k_compound": {"$exists": False}},
                        ],
                    },
                ],
                "$or": [{"a_idx": {"$in": [13, 11]}}, {"i_compound": {"$nin": [19, 12]}}],
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "i_idx": 1}},
    ],
    /* clusterSize: 77, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$in": [17, 10]}},
                    {
                        "$and": [
                            {"d_compound": {"$exists": True}},
                            {
                                "$or": [
                                    {"a_idx": {"$eq": 7}},
                                    {"c_idx": {"$nin": [7, 3]}},
                                    {"k_idx": {"$exists": False}},
                                ],
                            },
                            {"k_idx": {"$exists": True}},
                        ],
                    },
                ],
                "$or": [
                    {"$or": [{"i_compound": {"$gte": 5}}, {"a_idx": {"$all": [19, 6, 8]}}]},
                    {"z_compound": {"$in": [6, 10, 16]}},
                    {"a_compound": {"$all": [8, 19, 3]}},
                ],
                "a_noidx": {"$gt": 13},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 77, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"i_idx": {"$exists": False}},
                    {"$nor": [{"a_compound": {"$all": [20, 3, 20, 16]}}, {"k_compound": {"$gte": 6}}]},
                    {"k_idx": {"$exists": False}},
                    {"z_noidx": {"$gt": 9}},
                ],
                "a_noidx": {"$exists": True},
                "z_compound": {"$exists": True},
            },
        },
        {"$project": {"i_noidx": 1}},
    ],
    /* clusterSize: 77, queryRank: 4.03 */ [
        {"$match": {"a_idx": {"$gt": 9}, "z_compound": {"$lte": 12}}},
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 77, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$elemMatch": {"$in": [12, 16, 4], "$ne": 3}}},
                    {"a_compound": {"$all": [4, 8, 10]}},
                ],
                "a_compound": {"$lte": 14},
                "a_idx": {"$lt": 4},
            },
        },
        {"$sort": {"i_idx": -1, "k_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "d_noidx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 77, queryRank: 11.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$all": [17, 9]}},
                            {"a_compound": {"$elemMatch": {"$nin": [18, 5]}}},
                            {"i_compound": {"$nin": [13, 1]}},
                            {"c_idx": {"$ne": 14}},
                        ],
                    },
                    {"k_compound": {"$lt": 3}},
                ],
            },
        },
    ],
    /* clusterSize: 76, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [15, 5, 3, 8]}},
                    {"d_idx": {"$eq": 7}},
                    {"k_idx": {"$in": [14, 7, 18, 12]}},
                ],
                "a_compound": {"$elemMatch": {"$lte": 17}},
                "a_noidx": {"$gt": 19},
            },
        },
        {"$sort": {"d_idx": 1, "i_idx": -1}},
    ],
    /* clusterSize: 76, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"k_noidx": {"$in": [14, 9]}}, {"a_compound": {"$all": [13, 18, 14, 4]}}],
                "c_compound": {"$exists": True},
            },
        },
        {"$limit": 36},
    ],
    /* clusterSize: 76, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$ne": 1}}, {"a_compound": {"$all": [9, 15, 16]}}],
                "a_noidx": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 75, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [12, 13]}},
                    {"z_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$nin": [1, 16]}}},
                    {"a_compound": {"$nin": [6, 7]}},
                ],
                "a_compound": {"$lt": 2},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 64},
        {"$skip": 39},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 75, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"z_compound": {"$nin": [15, 10]}},
                            {"a_compound": {"$in": [1, 15, 19]}},
                            {"a_idx": {"$exists": True}},
                        ],
                    },
                    {"a_idx": {"$all": [9, 16]}},
                    {"h_idx": {"$nin": [14, 18, 11]}},
                ],
                "a_compound": {"$gt": 4},
                "a_idx": {"$in": [4, 5, 15]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 75, queryRank: 15.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$in": [19, 12, 15]}},
                    {"a_compound": {"$all": [6, 20]}},
                    {"a_compound": {"$exists": False}},
                ],
                "a_compound": {"$all": [2, 12]},
                "z_idx": {"$nin": [4, 6, 13]},
            },
        },
    ],
    /* clusterSize: 75, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [10, 12]}},
                    {"a_compound": {"$elemMatch": {"$lte": 2}}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_idx": {"$gt": 8}},
                ],
                "i_compound": {"$in": [2, 7, 11, 14]},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 115},
    ],
    /* clusterSize: 74, queryRank: 4.02 */ [
        {"$match": {"a_compound": {"$gte": 2}, "a_idx": {"$lte": 7}}},
        {"$sort": {"z_idx": -1}},
        {"$limit": 126},
        {"$project": {"k_noidx": 1}},
    ],
    /* clusterSize: 74, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [3, 5, 15]}},
                    {"a_idx": {"$elemMatch": {"$eq": 5, "$lt": 4, "$lte": 20}}},
                ],
                "a_noidx": {"$lte": 3},
                "h_idx": {"$ne": 5},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$project": {"c_idx": 1}},
    ],
    /* clusterSize: 74, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [10, 9, 5]}},
                    {"h_idx": {"$ne": 4}},
                    {"k_compound": {"$exists": False}},
                ],
                "a_noidx": {"$nin": [16, 7, 2, 9]},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 168},
        {"$project": {"a_compound": 1, "c_compound": 1}},
    ],
    /* clusterSize: 73, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"i_noidx": {"$nin": [2, 20]}},
                    {"k_compound": {"$nin": [7, 18]}},
                    {"$and": [{"c_compound": {"$nin": [15, 5, 8]}}, {"k_compound": {"$in": [18, 2, 6, 7]}}]},
                    {"$and": [{"a_noidx": {"$in": [15, 1, 1]}}, {"a_compound": {"$lte": 16}}]},
                ],
                "a_noidx": {"$elemMatch": {"$lte": 10}},
                "c_idx": {"$nin": [15, 20, 11, 11, 14]},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 73, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [2, 18, 15]}}, {"a_noidx": {"$exists": False}}],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 209},
    ],
    /* clusterSize: 73, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [
                    {"i_idx": {"$nin": [16, 9, 7, 3, 8]}},
                    {"$or": [{"a_compound": {"$all": [14, 5, 1, 10]}}, {"h_compound": {"$gte": 6}}]},
                ],
                "k_compound": {"$gte": 1},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 73, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$lt": 19}, "c_compound": {"$nin": [8, 12, 2, 9]}}},
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 73, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"i_compound": {"$exists": True}}, {"i_compound": {"$nin": [8, 5, 6]}}]},
                    {
                        "$or": [
                            {"a_compound": {"$all": [5, 6, 7, 14]}},
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                            {"a_idx": {"$all": [15, 17]}},
                        ],
                    },
                ],
                "a_noidx": {"$elemMatch": {"$exists": True, "$in": [9, 12]}},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$project": {"_id": 0, "a_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 72, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$nin": [3, 9]}},
                    {"d_compound": {"$gt": 18}},
                    {"a_compound": {"$all": [19, 6, 6]}},
                ],
                "a_noidx": {"$elemMatch": {"$gte": 6, "$lt": 19}},
            },
        },
        {"$limit": 55},
    ],
    /* clusterSize: 71, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"k_compound": {"$nin": [5, 15]}},
                    {
                        "$nor": [
                            {"a_idx": {"$elemMatch": {"$exists": False}}},
                            {"a_compound": {"$all": [20, 4, 16, 10]}},
                            {
                                "$or": [
                                    {"a_noidx": {"$eq": 7}},
                                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                                    {"a_idx": {"$all": [16, 19, 14]}},
                                ],
                            },
                        ],
                    },
                ],
                "$or": [{"a_idx": {"$all": [17, 3, 17]}}, {"a_compound": {"$elemMatch": {"$exists": True, "$lt": 15}}}],
            },
        },
        {"$sort": {"d_idx": -1, "i_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 71, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$exists": True}},
                    {"a_compound": {"$exists": True}},
                    {
                        "$or": [
                            {"a_compound": {"$in": [15, 2]}},
                            {"h_idx": {"$exists": False}},
                            {"a_idx": {"$all": [7, 14]}},
                        ],
                    },
                ],
                "a_idx": {"$elemMatch": {"$exists": True, "$in": [9, 10]}},
            },
        },
        {"$skip": 3},
        {"$project": {"d_compound": 1}},
    ],
    /* clusterSize: 71, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$eq": 11}},
                    {"a_compound": {"$all": [9, 18]}},
                    {"a_compound": {"$nin": [10, 5]}},
                ],
                "a_compound": {"$gte": 20},
            },
        },
        {"$sort": {"a_idx": 1, "i_idx": -1}},
        {"$project": {"_id": 0, "c_compound": 1}},
    ],
    /* clusterSize: 70, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [6, 9, 11]}}, {"k_compound": {"$eq": 4}}, {"k_noidx": {"$eq": 6}}],
                "a_compound": {"$exists": True},
            },
        },
        {"$limit": 211},
    ],
    /* clusterSize: 70, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$nin": [1, 6, 16]}}, {"a_compound": {"$all": [3, 9, 12]}}],
                "a_compound": {"$elemMatch": {"$eq": 6}},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 57},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 70, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_idx": {"$gte": 11}},
                    {
                        "$and": [
                            {"a_idx": {"$all": [3, 9, 16, 17, 18, 16]}},
                            {
                                "$or": [
                                    {"i_noidx": {"$ne": 7}},
                                    {"c_idx": {"$exists": False}},
                                    {"a_noidx": {"$nin": [18, 18, 17]}},
                                ],
                            },
                        ],
                    },
                    {"k_idx": {"$eq": 7}},
                    {"a_idx": {"$nin": [14, 4]}},
                ],
                "c_compound": {"$exists": True},
                "i_compound": {"$nin": [17, 17]},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 213},
        {"$project": {"_id": 0, "z_compound": 1}},
    ],
    /* clusterSize: 70, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [{"c_idx": {"$nin": [11, 6]}}, {"a_compound": {"$all": [5, 19, 4, 2]}}],
                "c_compound": {"$lte": 1},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 174},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 69, queryRank: 21.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$nin": [5, 11, 15]}},
                            {"a_compound": {"$all": [5, 18, 7]}},
                            {"a_idx": {"$elemMatch": {"$in": [20, 15, 19]}}},
                        ],
                    },
                    {
                        "$or": [
                            {"z_compound": {"$in": [8, 5, 8]}},
                            {
                                "$or": [
                                    {"d_compound": {"$gte": 9}},
                                    {"a_idx": {"$all": [16, 12, 6]}},
                                    {"a_idx": {"$elemMatch": {"$gte": 14, "$nin": [12, 9]}}},
                                    {"i_idx": {"$exists": True}},
                                    {"i_idx": {"$exists": False}},
                                ],
                            },
                        ],
                    },
                    {"$or": [{"k_compound": {"$in": [2, 16, 8, 7]}}, {"k_compound": {"$lte": 18}}]},
                ],
            },
        },
    ],
    /* clusterSize: 69, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_compound": {"$ne": 6}},
                    {"z_compound": {"$in": [19, 6]}},
                    {"a_idx": {"$nin": [11, 20, 3]}},
                    {
                        "$nor": [
                            {"a_compound": {"$exists": True}},
                            {"k_idx": {"$exists": False}},
                            {"a_compound": {"$all": [13, 13]}},
                            {"d_compound": {"$exists": True}},
                        ],
                    },
                ],
                "a_compound": {"$nin": [14, 5, 18, 14]},
            },
        },
        {"$limit": 219},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 69, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$lt": 19}}},
                    {"i_idx": {"$nin": [18, 16, 18]}},
                    {"a_compound": {"$lt": 7}},
                ],
                "a_compound": {"$elemMatch": {"$eq": 17, "$ne": 12}},
                "z_noidx": {"$nin": [7, 7, 14, 2]},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 69, queryRank: 3.03 */ [
        {"$match": {"k_compound": {"$gt": 1}}},
        {"$sort": {"k_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1, "c_compound": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 69, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$all": [14, 8, 8]}}, {"a_compound": {"$nin": [9, 1, 17, 13]}}]},
                    {
                        "$and": [
                            {"z_compound": {"$lte": 20}},
                            {"c_idx": {"$nin": [15, 4, 13]}},
                            {"h_idx": {"$exists": True}},
                        ],
                    },
                    {"k_idx": {"$gt": 2}},
                ],
                "a_noidx": {"$gte": 12},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 69},
    ],
    /* clusterSize: 69, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$gt": 2}, "z_compound": {"$ne": 2}}},
        {"$sort": {"c_idx": -1}},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 69, queryRank: 17.02 */ [
        {
            "$match": {
                "$nor": [{"z_compound": {"$in": [4, 12]}}, {"c_compound": {"$in": [9, 9, 3]}}],
                "$or": [
                    {"a_compound": {"$gt": 1}},
                    {"$and": [{"a_compound": {"$all": [17, 16]}}, {"k_compound": {"$in": [7, 15, 17, 3]}}]},
                    {"z_idx": {"$ne": 18}},
                    {"a_idx": {"$all": [7, 20, 7]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
                "z_idx": {"$lt": 11},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$limit": 190},
        {"$project": {"_id": 0, "a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 69, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_idx": {"$in": [3, 5, 15, 7]}},
                    {"i_compound": {"$exists": True}},
                    {"a_idx": {"$all": [14, 15, 8]}},
                ],
                "a_compound": {"$in": [13, 13, 16]},
                "a_idx": {"$in": [10, 3]},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 146},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 69, queryRank: 4.03 */ [
        {"$match": {"k_idx": {"$ne": 5}, "z_compound": {"$ne": 15}}},
        {"$sort": {"c_idx": 1}},
        {"$limit": 77},
    ],
    /* clusterSize: 69, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [4, 18, 16]}},
                    {"a_noidx": {"$nin": [5, 2]}},
                    {"c_idx": {"$exists": False}},
                    {"d_idx": {"$in": [16, 15]}},
                ],
                "a_idx": {"$in": [1, 15]},
                "a_noidx": {"$elemMatch": {"$nin": [13, 19, 11]}},
            },
        },
    ],
    /* clusterSize: 69, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$exists": True}},
                            {"k_compound": {"$nin": [14, 15, 14, 13]}},
                            {"a_compound": {"$all": [12, 3]}},
                        ],
                    },
                    {"k_idx": {"$lte": 11}},
                ],
                "$or": [{"a_idx": {"$lt": 9}}, {"i_noidx": {"$exists": True}}],
            },
        },
        {"$sort": {"c_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 68, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$lt": 4}},
                    {"$or": [{"c_noidx": {"$in": [9, 11]}}, {"a_compound": {"$all": [13, 7, 2, 3]}}]},
                    {"$or": [{"i_compound": {"$lt": 4}}, {"a_idx": {"$eq": 7}}, {"a_compound": {"$exists": False}}]},
                ],
                "h_compound": {"$nin": [19, 13, 13]},
            },
        },
        {"$sort": {"i_idx": -1, "z_idx": 1}},
        {"$limit": 148},
    ],
    /* clusterSize: 67, queryRank: 11.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_idx": {"$exists": True}},
                    {
                        "$nor": [
                            {"$nor": [{"d_compound": {"$eq": 5}}, {"h_idx": {"$nin": [3, 20]}}]},
                            {"a_compound": {"$exists": False}},
                        ],
                    },
                    {"$and": [{"a_compound": {"$elemMatch": {"$nin": [13, 18]}}}, {"a_noidx": {"$lt": 3}}]},
                    {"i_compound": {"$in": [5, 10, 8, 9]}},
                ],
                "a_idx": {"$elemMatch": {"$nin": [10, 11, 8]}},
                "a_noidx": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 55},
    ],
    /* clusterSize: 67, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$exists": True, "$gte": 7}}, "c_compound": {"$exists": True}}},
        {"$sort": {"c_idx": -1}},
        {"$limit": 48},
    ],
    /* clusterSize: 67, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [{"c_noidx": {"$lte": 16}}, {"k_compound": {"$nin": [15, 11, 15, 3]}}],
                "$or": [
                    {"i_compound": {"$lte": 15}},
                    {"a_idx": {"$all": [3, 2, 18, 12]}},
                    {"a_compound": {"$lte": 10}},
                ],
                "k_idx": {"$lte": 3},
            },
        },
        {"$limit": 249},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 67, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"a_noidx": {"$exists": True}}, {"a_compound": {"$elemMatch": {"$nin": [7, 6]}}}]},
                    {"a_compound": {"$nin": [18, 19]}},
                ],
                "k_compound": {"$gte": 6},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 67, queryRank: 17.03 */ [
        {
            "$match": {
                "$and": [
                    {"z_compound": {"$nin": [17, 17, 14]}},
                    {"a_compound": {"$nin": [9, 5, 13]}},
                    {"$or": [{"k_noidx": {"$in": [10, 2, 7]}}, {"a_idx": {"$in": [7, 3, 17]}}]},
                ],
                "$or": [{"c_compound": {"$nin": [2, 6]}}, {"a_idx": {"$all": [3, 16, 7]}}],
                "a_noidx": {"$elemMatch": {"$lt": 3}},
                "c_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 67, queryRank: 7.02 */ [
        {
            "$match": {
                "$nor": [
                    {"z_compound": {"$exists": False}},
                    {"c_compound": {"$gte": 16}},
                    {"i_noidx": {"$exists": False}},
                    {
                        "$and": [
                            {"d_idx": {"$exists": True}},
                            {"i_idx": {"$lt": 10}},
                            {"a_compound": {"$exists": False}},
                        ],
                    },
                ],
                "a_noidx": {"$exists": True},
                "h_compound": {"$exists": True},
            },
        },
        {"$skip": 34},
    ],
    /* clusterSize: 66, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$nin": [8, 18, 13]}},
                    {"$and": [{"k_compound": {"$ne": 9}}, {"a_compound": {"$gt": 6}}]},
                ],
                "a_compound": {"$elemMatch": {"$nin": [13, 14, 13, 12]}},
                "a_idx": {"$elemMatch": {"$in": [11, 4, 13], "$nin": [20, 14]}},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 66, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [18, 15]}},
                    {"a_compound": {"$elemMatch": {"$gte": 15, "$nin": [11, 16, 18, 7]}}},
                    {"k_compound": {"$lt": 2}},
                ],
                "i_idx": {"$in": [19, 20, 5]},
            },
        },
        {"$sort": {"h_idx": -1, "k_idx": -1}},
        {"$limit": 175},
        {"$project": {"_id": 0, "z_noidx": 1}},
    ],
    /* clusterSize: 66, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$lte": 7}},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"a_idx": {"$all": [13, 14, 10, 10]}},
                    {"h_compound": {"$in": [13, 14, 14]}},
                    {"a_compound": {"$in": [6, 20]}},
                ],
                "a_compound": {"$lt": 12},
            },
        },
        {"$sort": {"c_idx": 1, "k_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 66, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [{"c_idx": {"$lte": 19}}, {"a_compound": {"$nin": [11, 11, 5]}}, {"a_idx": {"$all": [18, 14]}}],
                "h_compound": {"$nin": [7, 6, 8]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 237},
    ],
    /* clusterSize: 65, queryRank: 6.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "a_idx": {"$nin": [5, 12, 4]}, "i_compound": {"$gte": 20}}},
        {"$sort": {"h_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 65, queryRank: 7.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$nin": [18, 18, 11]}}},
                    {"z_compound": {"$lte": 7}},
                    {"d_compound": {"$gte": 5}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 64, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"$and": [{"a_compound": {"$elemMatch": {"$exists": True}}}, {"h_compound": {"$exists": True}}]},
                    {
                        "$or": [
                            {"c_idx": {"$eq": 7}},
                            {"a_idx": {"$all": [19, 8, 13, 3]}},
                            {"a_idx": {"$all": [3, 7]}},
                            {"a_compound": {"$ne": 7}},
                        ],
                    },
                ],
                "a_idx": {"$nin": [20, 3]},
            },
        },
        {"$project": {"_id": 0, "h_compound": 1, "k_idx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 64, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"i_compound": {"$exists": True}},
                            {
                                "$and": [
                                    {"a_noidx": {"$elemMatch": {"$lte": 18, "$nin": [17, 13, 16, 19, 1]}}},
                                    {"a_noidx": {"$lte": 15}},
                                    {"a_idx": {"$elemMatch": {"$gte": 20}}},
                                ],
                            },
                        ],
                    },
                    {"h_idx": {"$in": [1, 19, 20]}},
                    {"a_idx": {"$lt": 12}},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                ],
                "a_compound": {"$gte": 10},
                "k_compound": {"$exists": True},
            },
        },
        {"$limit": 70},
    ],
    /* clusterSize: 64, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_idx": {"$exists": False}},
                    {"h_noidx": {"$gte": 3}},
                    {"a_compound": {"$elemMatch": {"$nin": [16, 2, 1]}}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [1, 10, 5, 20, 2]}},
                            {"a_compound": {"$lte": 3}},
                            {"z_idx": {"$exists": True}},
                        ],
                    },
                ],
                "a_compound": {"$nin": [3, 10]},
            },
        },
        {"$limit": 234},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 63, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$nin": [5, 11]}},
                    {"a_compound": {"$all": [7, 11, 1]}},
                    {"z_noidx": {"$gt": 3}},
                ],
                "k_compound": {"$gt": 8},
            },
        },
        {"$sort": {"c_idx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 63, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$nin": [15, 14]}}, {"a_idx": {"$all": [11, 9, 6]}}],
                "a_idx": {"$elemMatch": {"$eq": 3}},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 63, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"d_idx": {"$exists": False}}, {"a_compound": {"$all": [8, 9, 2]}}],
                "a_idx": {"$eq": 15},
                "k_idx": {"$gte": 1},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 63, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_noidx": {"$elemMatch": {"$exists": False}}},
                            {"a_compound": {"$all": [6, 14, 7]}},
                            {"a_idx": {"$all": [12, 17]}},
                            {"k_compound": {"$exists": True}},
                        ],
                    },
                    {"k_compound": {"$gt": 2}},
                ],
                "z_compound": {"$in": [6, 6, 18]},
            },
        },
        {"$sort": {"c_idx": -1, "d_idx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 62, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$exists": True}},
                    {"a_idx": {"$in": [10, 13, 17]}},
                    {"a_idx": {"$all": [17, 16, 11, 1]}},
                ],
                "a_compound": {"$elemMatch": {"$eq": 17, "$lte": 17}},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 62, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$elemMatch": {"$nin": [17, 12, 15, 5]}}}, {"a_compound": {"$lte": 12}}],
                "$or": [
                    {"a_compound": {"$all": [5, 1, 19, 11, 12]}},
                    {"a_idx": {"$nin": [8, 4]}},
                    {"i_idx": {"$lte": 19}},
                    {"a_compound": {"$elemMatch": {"$gte": 11, "$in": [6, 12]}}},
                ],
            },
        },
        {"$project": {"a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 62, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$gt": 12}},
                    {
                        "$or": [
                            {"a_idx": {"$in": [13, 15]}},
                            {"d_idx": {"$nin": [18, 2]}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$lt": 12}}},
                            {"a_compound": {"$exists": False}},
                        ],
                    },
                    {"i_compound": {"$nin": [4, 11]}},
                ],
                "a_noidx": {"$elemMatch": {"$in": [5, 11], "$lt": 8}},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "z_idx": 1}},
    ],
    /* clusterSize: 62, queryRank: 5.02 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$lt": 8}}, "d_compound": {"$nin": [4, 10, 5]}}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 168},
        {"$project": {"a_noidx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 62, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$eq": 15, "$nin": [19, 20, 2, 14]}}},
                    {"i_compound": {"$eq": 20}},
                    {"a_compound": {"$all": [11, 15]}},
                ],
                "k_idx": {"$lte": 20},
            },
        },
        {"$sort": {"c_idx": 1, "d_idx": 1, "h_idx": 1}},
        {"$limit": 187},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 62, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$nin": [16, 3, 16]}, "d_compound": {"$exists": True}}},
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 62, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$nin": [8, 10]}}, {"a_idx": {"$nin": [20, 9]}}],
                "$nor": [
                    {"i_idx": {"$lt": 19}},
                    {"a_compound": {"$in": [10, 15, 2]}},
                    {"a_idx": {"$in": [4, 10, 9, 1]}},
                ],
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 199},
    ],
    /* clusterSize: 62, queryRank: 9.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$nin": [3, 12]}},
                    {"a_idx": {"$gt": 14}},
                    {"z_compound": {"$in": [6, 8, 17, 7, 2]}},
                    {"$or": [{"a_compound": {"$elemMatch": {"$in": [8, 18, 4]}}}, {"a_compound": {"$gte": 15}}]},
                    {"k_idx": {"$nin": [20, 4]}},
                ],
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 178},
    ],
    /* clusterSize: 61, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$in": [1, 14, 7]}}},
                    {"i_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$in": [2, 12, 2, 11], "$nin": [13, 6]}}},
                    {"c_idx": {"$in": [20, 15, 13]}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$limit": 128},
    ],
    /* clusterSize: 61, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [{"k_idx": {"$lt": 8}}, {"i_compound": {"$lte": 5}}, {"a_compound": {"$all": [17, 8]}}],
                "a_noidx": {"$elemMatch": {"$lte": 8}},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 152},
        {"$project": {"c_compound": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 61, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"h_idx": {"$exists": False}},
                    {"a_compound": {"$lte": 13}},
                    {"$or": [{"z_compound": {"$exists": False}}, {"h_idx": {"$gte": 14}}]},
                    {"i_compound": {"$nin": [7, 4]}},
                ],
                "a_compound": {"$gte": 14},
                "a_idx": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 61, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [13, 12, 17]}}, {"a_idx": {"$nin": [17, 10, 9]}}],
                "c_compound": {"$in": [2, 15, 1, 19]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 61, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [18, 8]}},
                    {"k_compound": {"$gte": 7}},
                    {"$or": [{"a_compound": {"$elemMatch": {"$exists": True}}}, {"d_compound": {"$ne": 6}}]},
                ],
                "z_compound": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 152},
        {"$project": {"h_compound": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 61, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$nin": [3, 20]}}, {"i_compound": {"$exists": True}}],
                "a_compound": {"$elemMatch": {"$lte": 1}},
            },
        },
        {"$sort": {"h_idx": -1, "z_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "k_compound": 1}},
    ],
    /* clusterSize: 61, queryRank: 6.03 */ [
        {"$match": {"$and": [{"a_compound": {"$gte": 11}}, {"k_compound": {"$ne": 3}}, {"k_idx": {"$exists": True}}]}},
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1, "c_noidx": 1}},
    ],
    /* clusterSize: 61, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True, "$ne": 7}}},
                    {"a_compound": {"$exists": True}},
                    {"c_compound": {"$nin": [6, 4]}},
                ],
                "a_idx": {"$nin": [1, 10, 17]},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 60, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"z_idx": {"$gte": 18}},
                    {"a_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$gte": 11}}},
                ],
                "i_idx": {"$gte": 1},
            },
        },
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 60, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_compound": {"$exists": True}},
                    {"a_compound": {"$in": [13, 2, 2]}},
                    {"h_noidx": {"$in": [11, 9, 18]}},
                ],
                "$or": [
                    {"a_idx": {"$all": [12, 20, 16]}},
                    {"$or": [{"a_idx": {"$lte": 15}}, {"a_idx": {"$elemMatch": {"$in": [10, 7]}}}]},
                    {"a_idx": {"$all": [8, 4, 10, 7]}},
                    {"a_compound": {"$elemMatch": {"$ne": 13}}},
                ],
            },
        },
        {"$limit": 160},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 60, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$exists": False, "$in": [15, 9]}}},
                    {"a_compound": {"$exists": False}},
                    {"a_noidx": {"$all": [6, 19]}},
                    {"d_compound": {"$in": [7, 1, 6]}},
                ],
                "a_compound": {"$in": [11, 7, 12]},
            },
        },
    ],
    /* clusterSize: 60, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [16, 8, 13]}}, {"h_idx": {"$gte": 12}}, {"a_idx": {"$nin": [7, 8]}}],
                "c_compound": {"$in": [8, 1]},
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 239},
    ],
    /* clusterSize: 59, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [3, 20, 14, 8, 3]}},
                    {"$nor": [{"a_idx": {"$elemMatch": {"$nin": [19, 8, 6]}}}, {"k_compound": {"$lt": 10}}]},
                    {"c_idx": {"$in": [6, 3]}},
                    {"d_compound": {"$lt": 11}},
                ],
                "a_compound": {"$gte": 14},
            },
        },
        {"$sort": {"h_idx": 1, "k_idx": 1}},
        {"$limit": 213},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],
    /* clusterSize: 59, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {"a_compound": {"$nin": [7, 12, 18, 4]}},
                            {"a_compound": {"$nin": [7, 14]}},
                            {"z_compound": {"$exists": False}},
                        ],
                    },
                    {"i_compound": {"$nin": [11, 18]}},
                    {"a_compound": {"$exists": False}},
                ],
                "a_idx": {"$ne": 17},
            },
        },
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 59, queryRank: 14.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_idx": {"$nin": [13, 9]}},
                    {"a_idx": {"$lte": 10}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [3, 18, 16]}},
                            {"a_idx": {"$all": [17, 14, 12, 20]}},
                            {"k_idx": {"$exists": True}},
                        ],
                    },
                ],
                "a_compound": {"$all": [6, 1]},
            },
        },
        {"$project": {"_id": 0, "h_idx": 1, "h_noidx": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 58, queryRank: 15.03 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$in": [4, 12, 15]}}, {"a_compound": {"$all": [13, 11, 5, 3]}}],
                "a_idx": {"$in": [16, 18]},
                "d_idx": {"$gt": 7},
                "z_idx": {"$in": [4, 8]},
            },
        },
    ],
    /* clusterSize: 58, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$exists": True}},
                    {"k_idx": {"$exists": False}},
                    {"c_compound": {"$nin": [9, 14]}},
                ],
                "a_compound": {"$gte": 4},
                "a_noidx": {"$elemMatch": {"$eq": 19, "$exists": True, "$gte": 10}},
            },
        },
    ],
    /* clusterSize: 58, queryRank: 9.02 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"c_compound": {"$in": [10, 9, 10]}}, {"k_compound": {"$lt": 16}}]},
                    {"a_idx": {"$elemMatch": {"$in": [16, 5, 19]}}},
                    {"a_compound": {"$elemMatch": {"$gte": 8, "$nin": [9, 18]}}},
                ],
                "a_noidx": {"$eq": 18},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 58, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"z_compound": {"$lte": 5}},
                    {"$or": [{"d_compound": {"$ne": 18}}, {"a_idx": {"$elemMatch": {"$in": [3, 18, 12]}}}]},
                    {"i_compound": {"$in": [19, 3, 16]}},
                    {"$and": [{"a_noidx": {"$elemMatch": {"$exists": False}}}, {"d_idx": {"$gte": 16}}]},
                ],
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 58, queryRank: 15.03 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$all": [8, 5, 2]}}, {"a_compound": {"$nin": [5, 14]}}]},
                    {"k_idx": {"$ne": 17}},
                ],
                "$or": [
                    {"d_noidx": {"$gt": 11}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [6, 13]}},
                            {"a_noidx": {"$nin": [7, 6, 1, 6, 6]}},
                            {"d_compound": {"$nin": [7, 18]}},
                        ],
                    },
                    {"a_idx": {"$gte": 11}},
                ],
                "a_compound": {"$all": [13, 13]},
            },
        },
    ],
    /* clusterSize: 57, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$nin": [6, 12, 15]}}},
                    {"a_compound": {"$exists": True}},
                    {"a_noidx": {"$elemMatch": {"$exists": True, "$lt": 6}}},
                    {"a_idx": {"$exists": True}},
                ],
                "$or": [{"z_idx": {"$nin": [5, 8, 9]}}, {"i_compound": {"$nin": [3, 1]}}, {"c_compound": {"$lt": 12}}],
            },
        },
        {"$project": {"a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 57, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"c_idx": {"$in": [11, 1, 14, 17]}},
                    {"$nor": [{"a_compound": {"$all": [13, 5, 10, 9]}}, {"z_idx": {"$exists": False}}]},
                ],
                "c_compound": {"$eq": 1},
            },
        },
        {"$limit": 167},
        {"$skip": 43},
    ],
    /* clusterSize: 57, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$in": [14, 20, 7], "$nin": [1, 2, 3]}}},
                    {"a_compound": {"$elemMatch": {"$gte": 6}}},
                    {
                        "$and": [
                            {"h_noidx": {"$in": [15, 11]}},
                            {"z_compound": {"$ne": 4}},
                            {"a_compound": {"$nin": [9, 17]}},
                        ],
                    },
                ],
                "a_idx": {"$elemMatch": {"$gt": 7}},
                "k_idx": {"$in": [11, 20]},
            },
        },
    ],
    /* clusterSize: 57, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"$or": [{"a_compound": {"$all": [10, 1, 3]}}, {"a_compound": {"$exists": False}}]},
                    {"k_noidx": {"$in": [19, 14]}},
                ],
                "a_compound": {"$nin": [18, 3]},
            },
        },
    ],
    /* clusterSize: 57, queryRank: 6.02 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$all": [3, 18]}}, {"k_compound": {"$in": [7, 8]}}],
                "a_compound": {"$elemMatch": {"$lte": 2}},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$limit": 91},
        {"$skip": 22},
    ],
    /* clusterSize: 57, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"d_compound": {"$exists": True}}, {"a_compound": {"$elemMatch": {"$exists": True}}}],
                "a_idx": {"$elemMatch": {"$lte": 15}},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 57, queryRank: 4.03 */ [
        {"$match": {"a_idx": {"$nin": [19, 6, 5]}, "d_compound": {"$nin": [19, 13, 6]}}},
        {"$sort": {"a_idx": -1}},
        {"$project": {"k_compound": 1}},
    ],
    /* clusterSize: 56, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [3, 20]}},
                    {"i_idx": {"$exists": True}},
                    {"z_compound": {"$eq": 16}},
                    {"k_idx": {"$exists": False}},
                ],
                "i_noidx": {"$in": [9, 4]},
            },
        },
        {"$sort": {"k_idx": 1}},
    ],
    /* clusterSize: 56, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$nin": [8, 7]}},
                    {"a_idx": {"$all": [11, 15, 1]}},
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"i_compound": {"$gte": 9}},
                ],
                "d_compound": {"$lte": 9},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 88},
    ],
    /* clusterSize: 56, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$elemMatch": {"$eq": 8, "$exists": True}}},
                    {"a_compound": {"$all": [7, 1, 16, 10]}},
                ],
                "z_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1, "h_idx": -1}},
        {"$limit": 11},
        {"$project": {"k_compound": 1}},
    ],
    /* clusterSize: 56, queryRank: 9.03 */ [
        {"$match": {"$or": [{"a_idx": {"$all": [8, 9, 1]}}, {"a_compound": {"$exists": True}}]}},
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 56, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [1, 7, 14]}}, {"i_idx": {"$lt": 4}}],
                "a_idx": {"$elemMatch": {"$in": [6, 10, 14, 1]}},
            },
        },
        {"$limit": 221},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 56, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$exists": False}}, {"a_compound": {"$all": [18, 4]}}],
                "a_compound": {"$nin": [9, 5, 1]},
            },
        },
        {"$sort": {"c_idx": -1, "z_idx": 1}},
        {"$limit": 66},
    ],
    /* clusterSize: 56, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [17, 4]}}, {"a_idx": {"$nin": [9, 11, 11, 2]}}],
                "a_compound": {"$exists": True},
            },
        },
        {"$project": {"a_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 56, queryRank: 5.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_idx": {"$exists": True}},
                    {"k_compound": {"$exists": True}},
                    {"$and": [{"a_compound": {"$elemMatch": {"$exists": True}}}, {"a_noidx": {"$nin": [8, 20]}}]},
                ],
            },
        },
        {"$limit": 20},
        {"$project": {"c_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 55, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_idx": {"$exists": True}},
                            {"a_idx": {"$elemMatch": {"$in": [11, 6, 18], "$nin": [9, 6]}}},
                            {"a_idx": {"$all": [16, 14]}},
                            {"a_compound": {"$all": [6, 19]}},
                        ],
                    },
                    {"h_idx": {"$ne": 3}},
                ],
                "a_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 55, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                    {
                        "$nor": [
                            {"$or": [{"i_compound": {"$ne": 18}}, {"a_idx": {"$all": [4, 13]}}]},
                            {"z_compound": {"$exists": False}},
                        ],
                    },
                ],
                "i_idx": {"$exists": True},
                "z_compound": {"$lt": 10},
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 55, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$exists": True}}},
                            {"z_compound": {"$in": [1, 16]}},
                            {"a_compound": {"$all": [17, 8]}},
                        ],
                    },
                    {"a_idx": {"$gt": 17}},
                ],
                "k_compound": {"$nin": [6, 16, 20, 6]},
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 55, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$in": [18, 3, 5]}}, {"i_compound": {"$in": [13, 15]}}],
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$all": [3, 18, 9]}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$in": [8, 14, 8]}}},
                            {
                                "$nor": [
                                    {"k_compound": {"$nin": [4, 2]}},
                                    {"a_noidx": {"$elemMatch": {"$eq": 1, "$exists": True, "$nin": [2, 2, 6]}}},
                                ],
                            },
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"a_compound": {"$nin": [16, 15]}},
                ],
                "a_idx": {"$all": [1, 12]},
            },
        },
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 55, queryRank: 14.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [19, 14]}}, {"i_compound": {"$nin": [15, 14, 5]}}],
                "d_compound": {"$lt": 7},
                "k_compound": {"$eq": 10},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 193},
    ],
    /* clusterSize: 55, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [19, 9, 2]}},
                    {"a_idx": {"$nin": [3, 18]}},
                    {"a_compound": {"$gte": 10}},
                ],
                "h_idx": {"$ne": 3},
            },
        },
        {"$sort": {"i_idx": 1, "k_idx": 1}},
        {"$limit": 248},
        {"$project": {"_id": 0, "z_noidx": 1}},
    ],
    /* clusterSize: 54, queryRank: 6.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$exists": True, "$lt": 7}},
                "i_compound": {"$gte": 15},
                "k_compound": {"$in": [19, 4, 18, 7]},
            },
        },
    ],
    /* clusterSize: 54, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$gte": 6}}},
                    {"k_compound": {"$ne": 3}},
                    {"a_compound": {"$elemMatch": {"$eq": 2}}},
                    {"k_compound": {"$in": [9, 14, 6, 5]}},
                ],
                "$or": [{"a_idx": {"$all": [10, 6]}}, {"a_compound": {"$exists": True}}],
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 54, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$or": [{"k_idx": {"$in": [14, 12]}}, {"a_noidx": {"$nin": [18, 8]}}]},
                    {"a_noidx": {"$in": [19, 1]}},
                    {"a_compound": {"$all": [15, 1, 4]}},
                ],
                "a_noidx": {"$gt": 11},
                "i_compound": {"$nin": [15, 11]},
                "k_noidx": {"$nin": [17, 14]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 118},
    ],
    /* clusterSize: 54, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {"a_compound": {"$in": [19, 11, 11]}},
                            {
                                "$or": [
                                    {"a_noidx": {"$in": [1, 17]}},
                                    {"a_compound": {"$gte": 4}},
                                    {"c_compound": {"$in": [3, 8]}},
                                ],
                            },
                        ],
                    },
                    {"h_idx": {"$eq": 7}},
                    {"a_idx": {"$in": [10, 1]}},
                ],
                "a_compound": {"$elemMatch": {"$in": [3, 4]}},
            },
        },
    ],
    /* clusterSize: 54, queryRank: 6.03 */ [
        {
            "$match": {
                "a_compound": {"$nin": [11, 9, 9, 8]},
                "h_idx": {"$exists": True},
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 54, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [11, 18, 17, 20, 1, 12]}}, {"d_idx": {"$nin": [12, 3]}}],
                "h_compound": {"$lte": 5},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 130},
        {"$project": {"h_compound": 1, "i_compound": 1}},
    ],
    /* clusterSize: 54, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$gt": 1}}},
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "a_idx": {"$elemMatch": {"$gt": 17}},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "h_idx": 1}},
    ],
    /* clusterSize: 53, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [10, 7, 15, 1]}},
                    {"h_compound": {"$lt": 2}},
                    {"c_compound": {"$exists": False}},
                ],
                "a_compound": {"$nin": [5, 14]},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$project": {"a_idx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 53, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"h_compound": {"$nin": [4, 1, 10, 3]}},
                    {"i_idx": {"$eq": 12}},
                    {
                        "$and": [
                            {"$nor": [{"z_compound": {"$exists": True}}, {"a_idx": {"$all": [14, 9, 19, 20]}}]},
                            {"c_compound": {"$exists": True}},
                        ],
                    },
                    {"c_idx": {"$gt": 14}},
                    {"d_compound": {"$nin": [19, 20, 3, 15]}},
                ],
                "z_noidx": {"$lt": 7},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 215},
    ],
    /* clusterSize: 53, queryRank: 11.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$elemMatch": {"$exists": True}}}, {"a_noidx": {"$exists": True}}],
                "$or": [{"a_idx": {"$all": [18, 16, 19]}}, {"d_compound": {"$nin": [20, 18]}}],
                "a_noidx": {"$elemMatch": {"$gt": 15, "$nin": [10, 5, 2]}},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 220},
    ],
    /* clusterSize: 53, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$gte": 13}, "i_compound": {"$gte": 13}}},
        {"$sort": {"d_idx": -1, "h_idx": -1, "z_idx": -1}},
    ],
    /* clusterSize: 53, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$gte": 18}}, {"a_idx": {"$all": [1, 11, 20]}}],
                "$or": [
                    {"$and": [{"a_noidx": {"$all": [8, 9]}}, {"a_idx": {"$elemMatch": {"$nin": [13, 4, 12]}}}]},
                    {"$or": [{"a_idx": {"$all": [4, 14]}}, {"a_compound": {"$all": [20, 5, 17, 14, 20]}}]},
                    {"a_compound": {"$elemMatch": {"$in": [10, 15, 11]}}},
                ],
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 53, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"c_compound": {"$in": [18, 11, 1]}},
                    {"k_idx": {"$nin": [13, 11, 13, 6]}},
                    {
                        "$nor": [
                            {"i_compound": {"$nin": [18, 9, 15]}},
                            {"a_idx": {"$elemMatch": {"$exists": False}}},
                            {"$and": [{"c_idx": {"$nin": [19, 12, 15]}}, {"d_compound": {"$in": [20, 15, 2]}}]},
                        ],
                    },
                    {"a_compound": {"$lte": 6}},
                ],
                "c_compound": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 53, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_idx": {"$exists": True}},
                            {"i_compound": {"$lte": 15}},
                            {"a_compound": {"$all": [2, 6, 9, 16, 10]}},
                        ],
                    },
                    {"a_idx": {"$all": [9, 2]}},
                ],
                "i_compound": {"$gt": 3},
            },
        },
        {"$sort": {"c_idx": 1, "h_idx": -1}},
        {"$skip": 2},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 52, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$exists": True}},
                    {"$nor": [{"k_compound": {"$exists": False}}, {"a_compound": {"$all": [12, 2]}}]},
                    {"k_idx": {"$nin": [4, 19, 14]}},
                ],
                "z_noidx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"a_noidx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 52, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$in": [11, 5]}}},
                    {
                        "$or": [
                            {"z_noidx": {"$in": [5, 2]}},
                            {"a_noidx": {"$all": [19, 2]}},
                            {"a_noidx": {"$elemMatch": {"$in": [5, 7, 19], "$nin": [4, 5, 9]}}},
                            {"a_noidx": {"$all": [2, 3, 14, 11]}},
                        ],
                    },
                ],
                "$or": [
                    {"h_idx": {"$lte": 10}},
                    {"a_idx": {"$elemMatch": {"$in": [19, 9], "$ne": 5}}},
                    {
                        "$or": [
                            {"$and": [{"h_compound": {"$in": [13, 6, 4, 13]}}, {"a_idx": {"$all": [17, 1]}}]},
                            {"d_idx": {"$gt": 13}},
                            {"a_idx": {"$all": [19, 20]}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$in": [12, 3], "$nin": [17, 6, 14, 5]}}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 148},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 52, queryRank: 11.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$exists": True}},
                    {"a_idx": {"$elemMatch": {"$in": [14, 7], "$nin": [14, 6, 15]}}},
                    {"a_compound": {"$elemMatch": {"$eq": 1, "$exists": True}}},
                ],
                "$nor": [{"a_compound": {"$exists": False}}, {"a_compound": {"$all": [17, 13]}}],
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 250},
    ],
    /* clusterSize: 52, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$and": [
                            {
                                "$nor": [
                                    {"a_idx": {"$in": [16, 18]}},
                                    {"a_noidx": {"$all": [14, 11]}},
                                    {"a_idx": {"$exists": False}},
                                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                                ],
                            },
                            {"a_idx": {"$nin": [20, 7, 13]}},
                            {"d_compound": {"$nin": [8, 19, 2]}},
                        ],
                    },
                    {"a_compound": {"$all": [14, 2]}},
                ],
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 9},
    ],
    /* clusterSize: 51, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [{"d_compound": {"$exists": True}}, {"a_compound": {"$elemMatch": {"$exists": True}}}],
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": False, "$in": [9, 2, 15]}}},
                    {"a_compound": {"$elemMatch": {"$ne": 3}}},
                    {"a_idx": {"$eq": 19}},
                    {"a_compound": {"$lte": 2}},
                ],
            },
        },
        {"$project": {"c_noidx": 1}},
    ],
    /* clusterSize: 51, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [
                    {"z_compound": {"$ne": 10}},
                    {"a_compound": {"$elemMatch": {"$in": [4, 1]}}},
                    {"$and": [{"a_idx": {"$nin": [15, 18]}}, {"a_noidx": {"$lte": 16}}]},
                    {"a_compound": {"$ne": 4}},
                ],
                "a_noidx": {"$nin": [5, 15, 5]},
                "i_idx": {"$ne": 4},
            },
        },
    ],
    /* clusterSize: 51, queryRank: 17.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$lt": 15}},
                    {"a_compound": {"$all": [2, 5, 4]}},
                    {"d_noidx": {"$exists": False}},
                    {
                        "$or": [
                            {"z_compound": {"$nin": [19, 2]}},
                            {"i_noidx": {"$exists": False}},
                            {"d_idx": {"$in": [13, 12, 15]}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$project": {"_id": 0, "i_compound": 1}},
    ],
    /* clusterSize: 51, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$or": [
                            {"d_noidx": {"$in": [19, 17, 19]}},
                            {"a_compound": {"$all": [10, 4, 6]}},
                            {"k_compound": {"$eq": 15}},
                        ],
                    },
                    {"i_compound": {"$eq": 5}},
                ],
                "a_compound": {"$elemMatch": {"$lte": 3}},
            },
        },
        {"$skip": 86},
    ],
    /* clusterSize: 51, queryRank: 13.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$exists": False}},
                            {"a_idx": {"$ne": 16}},
                            {"a_compound": {"$all": [8, 12, 7]}},
                        ],
                    },
                    {
                        "$or": [
                            {"c_compound": {"$exists": True}},
                            {"a_idx": {"$elemMatch": {"$gt": 2, "$ne": 10}}},
                            {"k_compound": {"$lt": 17}},
                        ],
                    },
                ],
                "a_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 51, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$elemMatch": {"$nin": [18, 9]}}}, {"a_compound": {"$all": [12, 1]}}],
                "z_compound": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1, "z_idx": 1}},
        {"$limit": 66},
    ],
    /* clusterSize: 50, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$eq": 6, "$exists": True, "$ne": 8}}},
                    {"a_compound": {"$all": [9, 2]}},
                    {"a_compound": {"$nin": [12, 12, 14]}},
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1, "h_idx": 1}},
    ],
    /* clusterSize: 50, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [{"c_compound": {"$gt": 4}}, {"a_idx": {"$all": [3, 18]}}, {"a_compound": {"$gt": 5}}],
                "a_compound": {"$all": [18, 18]},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"a_compound": 1, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 50, queryRank: 6.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "d_compound": {"$lt": 7}, "z_idx": {"$lt": 13}}},
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 50, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_idx": {"$gte": 3}},
                    {
                        "$nor": [
                            {"z_compound": {"$in": [5, 20]}},
                            {"c_compound": {"$in": [17, 14]}},
                            {"a_compound": {"$gt": 15}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"a_compound": 1, "k_compound": 1}},
    ],
    /* clusterSize: 50, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$all": [11, 8]}},
                    {"a_compound": {"$all": [10, 3, 8, 17]}},
                    {"$nor": [{"i_idx": {"$exists": True}}, {"a_idx": {"$ne": 7}}, {"a_compound": {"$exists": True}}]},
                ],
                "$or": [
                    {
                        "$or": [
                            {"z_idx": {"$exists": True}},
                            {"i_compound": {"$eq": 9}},
                            {"a_idx": {"$all": [13, 20, 4]}},
                        ],
                    },
                    {"a_idx": {"$all": [5, 20]}},
                    {"a_compound": {"$exists": False}},
                ],
                "c_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 50, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"h_idx": {"$nin": [13, 16, 2]}},
                    {"a_idx": {"$nin": [16, 14, 7, 13]}},
                    {"a_compound": {"$all": [4, 15]}},
                    {"a_idx": {"$all": [12, 8, 9]}},
                ],
                "a_compound": {"$exists": True},
                "c_idx": {"$ne": 15},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 50, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$ne": 16}},
                    {"c_noidx": {"$exists": True}},
                    {"$nor": [{"a_compound": {"$all": [13, 2, 13, 20, 12]}}, {"i_idx": {"$lte": 6}}]},
                ],
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 102},
    ],
    /* clusterSize: 50, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [{"h_noidx": {"$gt": 4}}, {"h_compound": {"$exists": True}}],
                "$nor": [
                    {
                        "$and": [
                            {"a_compound": {"$exists": False}},
                            {"c_idx": {"$exists": True}},
                            {"a_compound": {"$all": [16, 7, 20]}},
                        ],
                    },
                    {"z_noidx": {"$in": [18, 6, 17, 8, 17]}},
                    {"d_noidx": {"$exists": False}},
                ],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1}},
        {"$limit": 216},
        {"$project": {"z_idx": 1}},
    ],
    /* clusterSize: 49, queryRank: 9.03 */ [
        {
            "$match": {
                "$and": [
                    {"d_idx": {"$nin": [13, 8]}},
                    {"h_compound": {"$nin": [12, 20]}},
                    {"c_noidx": {"$lt": 4}},
                    {"a_compound": {"$in": [4, 9]}},
                    {"a_compound": {"$exists": True}},
                ],
                "a_compound": {"$gt": 1},
                "a_idx": {"$elemMatch": {"$in": [18, 5, 11], "$nin": [10, 7, 4]}},
            },
        },
    ],
    /* clusterSize: 49, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$nor": [
                            {"c_compound": {"$exists": False}},
                            {"i_compound": {"$lte": 19}},
                            {"a_idx": {"$exists": True}},
                        ],
                    },
                    {"a_noidx": {"$elemMatch": {"$in": [15, 5]}}},
                ],
                "a_idx": {"$elemMatch": {"$lte": 12}},
            },
        },
    ],
    /* clusterSize: 49, queryRank: 16.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_noidx": {"$exists": True}},
                    {"a_noidx": {"$in": [10, 20, 19]}},
                    {"i_idx": {"$lt": 16}},
                    {
                        "$or": [
                            {
                                "$or": [
                                    {"a_compound": {"$elemMatch": {"$gt": 5}}},
                                    {"a_compound": {"$elemMatch": {"$eq": 7, "$lte": 17}}},
                                ],
                            },
                            {"a_idx": {"$all": [6, 9]}},
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$exists": True, "$nin": [2, 1, 4]}}},
                ],
                "$or": [
                    {"i_compound": {"$exists": True}},
                    {"i_compound": {"$in": [15, 16, 2]}},
                    {"c_idx": {"$exists": False}},
                ],
            },
        },
        {"$sort": {"d_idx": 1, "h_idx": -1}},
        {"$limit": 222},
    ],
    /* clusterSize: 49, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"c_compound": {"$exists": False}},
                    {"a_noidx": {"$nin": [20, 11, 10, 9]}},
                    {"a_compound": {"$all": [15, 12, 7, 13]}},
                ],
                "k_noidx": {"$gt": 4},
            },
        },
        {"$limit": 146},
    ],
    /* clusterSize: 49, queryRank: 6.02 */ [
        {"$match": {"$or": [{"h_compound": {"$gt": 11}}, {"a_idx": {"$all": [17, 4, 19]}}], "a_idx": {"$gte": 7}}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 119},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 49, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$ne": 20}},
                    {"d_idx": {"$lt": 8}},
                    {"a_idx": {"$all": [18, 18, 12, 6, 1]}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "a_compound": {"$elemMatch": {"$nin": [12, 18, 5]}},
                "a_idx": {"$in": [6, 2]},
            },
        },
        {"$limit": 4},
        {"$project": {"d_compound": 1}},
    ],
    /* clusterSize: 49, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {"k_compound": {"$exists": False}},
                            {"i_compound": {"$eq": 18}},
                            {"d_idx": {"$nin": [9, 19, 8, 14]}},
                            {"a_noidx": {"$all": [13, 9]}},
                        ],
                    },
                    {"k_compound": {"$in": [10, 9, 19, 14]}},
                ],
                "a_compound": {"$lt": 14},
                "d_idx": {"$nin": [14, 6, 9, 20, 8, 18]},
                "i_idx": {"$lt": 20},
            },
        },
    ],
    /* clusterSize: 49, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$all": [1, 7, 18]}},
                            {"a_idx": {"$elemMatch": {"$gte": 20}}},
                            {"a_idx": {"$elemMatch": {"$exists": True, "$nin": [14, 8, 2]}}},
                        ],
                    },
                    {"$or": [{"c_compound": {"$lt": 8}}, {"a_idx": {"$eq": 4}}, {"a_compound": {"$nin": [15, 4]}}]},
                ],
                "h_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "i_idx": -1, "k_idx": 1}},
    ],
    /* clusterSize: 48, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$lte": 14}},
                    {
                        "$or": [
                            {"a_compound": {"$in": [13, 14]}},
                            {"a_compound": {"$exists": True}},
                            {"a_compound": {"$in": [19, 17]}},
                        ],
                    },
                    {"z_compound": {"$ne": 15}},
                ],
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 91},
    ],
    /* clusterSize: 48, queryRank: 5.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$in": [3, 12, 8]}}, {"a_compound": {"$elemMatch": {"$eq": 12}}}],
                "a_compound": {"$elemMatch": {"$exists": True, "$lte": 2}},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 29},
        {"$project": {"_id": 0, "c_compound": 1, "h_compound": 1}},
    ],
    /* clusterSize: 48, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$nin": [13, 15]}, "z_compound": {"$nin": [20, 19]}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 48, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$exists": False, "$in": [9, 7, 14]}}},
                    {"$or": [{"z_idx": {"$exists": True}}, {"a_idx": {"$all": [14, 2]}}]},
                    {"k_compound": {"$exists": False}},
                    {"k_compound": {"$lte": 15}},
                ],
                "z_compound": {"$ne": 12},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"_id": 0, "i_compound": 1}},
    ],
    /* clusterSize: 48, queryRank: 7.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$nin": [12, 13]}}, {"a_compound": {"$all": [9, 4]}}],
                "a_compound": {"$gte": 20},
            },
        },
        {"$sort": {"d_idx": -1, "h_idx": -1}},
        {"$limit": 250},
    ],
    /* clusterSize: 48, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [13, 15, 8]}},
                    {
                        "$and": [
                            {"c_idx": {"$nin": [9, 7]}},
                            {"a_compound": {"$in": [11, 7, 14]}},
                            {"d_compound": {"$lte": 2}},
                        ],
                    },
                    {"a_idx": {"$all": [17, 17]}},
                ],
                "a_compound": {"$elemMatch": {"$in": [11, 18]}},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 47, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"h_idx": {"$gte": 8}},
                    {"$and": [{"i_compound": {"$exists": True}}, {"a_compound": {"$all": [2, 9, 19, 3, 20]}}]},
                ],
                "a_compound": {"$lte": 11},
                "z_idx": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 47, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$in": [13, 5]}},
                    {
                        "$or": [
                            {"d_compound": {"$lt": 15}},
                            {"a_idx": {"$in": [16, 7, 13]}},
                            {
                                "$and": [
                                    {
                                        "$and": [
                                            {"k_compound": {"$eq": 7}},
                                            {"c_compound": {"$exists": False}},
                                            {"a_compound": {"$exists": True}},
                                            {"a_compound": {"$gt": 6}},
                                        ],
                                    },
                                    {"i_compound": {"$nin": [13, 16]}},
                                ],
                            },
                        ],
                    },
                    {"a_noidx": {"$exists": True}},
                ],
                "$or": [{"a_idx": {"$in": [19, 12]}}, {"a_compound": {"$ne": 7}}, {"a_idx": {"$ne": 1}}],
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 47, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$lt": 8}},
                    {"c_compound": {"$lt": 13}},
                    {"a_compound": {"$gte": 3}},
                    {"a_compound": {"$ne": 13}},
                    {"a_compound": {"$exists": False}},
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$project": {"a_noidx": 1, "z_compound": 1}},
    ],
    /* clusterSize: 47, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$in": [11, 1, 14, 13, 18, 15]}},
                    {"$nor": [{"a_idx": {"$eq": 18}}, {"a_compound": {"$all": [13, 11, 9]}}]},
                    {"a_noidx": {"$exists": True}},
                    {"c_noidx": {"$nin": [2, 15]}},
                ],
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 47, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$lte": 11}},
                    {"z_noidx": {"$exists": True}},
                    {"$or": [{"a_idx": {"$exists": True}}, {"a_idx": {"$exists": True}}]},
                    {"h_noidx": {"$gt": 10}},
                ],
                "$or": [{"a_idx": {"$all": [17, 12]}}, {"d_compound": {"$gte": 2}}, {"i_compound": {"$exists": True}}],
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$limit": 235},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 46, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$gt": 6}},
                    {"a_idx": {"$elemMatch": {"$exists": True, "$in": [18, 10, 17, 20]}}},
                    {"a_compound": {"$all": [6, 3, 19, 12]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
                "c_noidx": {"$in": [16, 1]},
                "i_noidx": {"$ne": 10},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$limit": 214},
    ],
    /* clusterSize: 46, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$lt": 14}}, {"a_compound": {"$eq": 1}}],
                "a_compound": {"$elemMatch": {"$gte": 15}},
            },
        },
    ],
    /* clusterSize: 46, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [{"a_noidx": {"$in": [1, 4, 11, 5, 9]}}, {"i_noidx": {"$exists": False}}],
                "$or": [
                    {"a_idx": {"$all": [6, 17, 17, 16, 12]}},
                    {"i_compound": {"$gte": 18}},
                    {"$or": [{"a_idx": {"$all": [8, 15]}}, {"a_idx": {"$elemMatch": {"$lte": 16}}}]},
                    {"c_idx": {"$gte": 18}},
                ],
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 46, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$all": [17, 12]}}, {"a_noidx": {"$all": [10, 10]}}]},
                    {"a_idx": {"$in": [8, 3, 2, 7]}},
                ],
            },
        },
        {"$sort": {"a_idx": -1, "d_idx": -1}},
    ],
    /* clusterSize: 45, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {
                                "$or": [
                                    {
                                        "$and": [
                                            {"z_compound": {"$gt": 3}},
                                            {
                                                "a_noidx": {
                                                    "$elemMatch": {"$in": [11, 17], "$lte": 3, "$nin": [3, 14, 20, 8]},
                                                },
                                            },
                                        ],
                                    },
                                    {"i_idx": {"$eq": 9}},
                                    {"z_compound": {"$nin": [19, 14, 14]}},
                                ],
                            },
                            {"h_compound": {"$exists": False}},
                        ],
                    },
                    {"a_idx": {"$in": [12, 7, 9]}},
                    {"a_compound": {"$elemMatch": {"$nin": [17, 15, 10, 14, 15]}}},
                ],
                "i_compound": {"$nin": [19, 2]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 86},
        {"$project": {"_id": 0, "a_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 45, queryRank: 5.03 */ [
        {"$match": {"$or": [{"a_compound": {"$in": [3, 2, 20]}}, {"z_idx": {"$lt": 18}}], "i_compound": {"$ne": 2}}},
        {"$sort": {"z_idx": 1}},
        {"$project": {"_id": 0, "h_compound": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 44, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"k_compound": {"$in": [6, 16]}},
                            {"a_noidx": {"$exists": False}},
                            {
                                "$nor": [
                                    {"a_noidx": {"$exists": True}},
                                    {
                                        "$or": [
                                            {"c_compound": {"$in": [12, 4]}},
                                            {"a_compound": {"$nin": [18, 12, 5]}},
                                            {"a_idx": {"$exists": True}},
                                            {"i_compound": {"$nin": [2, 13]}},
                                        ],
                                    },
                                ],
                            },
                            {"h_compound": {"$ne": 12}},
                        ],
                    },
                    {"z_compound": {"$lte": 10}},
                ],
                "i_noidx": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 198},
    ],
    /* clusterSize: 44, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"c_compound": {"$eq": 4}},
                    {
                        "$nor": [
                            {
                                "$and": [
                                    {"i_compound": {"$exists": True}},
                                    {"a_compound": {"$in": [11, 3]}},
                                    {"$nor": [{"h_idx": {"$eq": 15}}, {"k_compound": {"$in": [5, 19, 20]}}]},
                                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                                ],
                            },
                            {"c_idx": {"$in": [15, 17, 7]}},
                        ],
                    },
                ],
                "k_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1, "z_idx": 1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 44, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {
                                "$nor": [
                                    {"c_noidx": {"$ne": 16}},
                                    {"a_compound": {"$lt": 12}},
                                    {"c_compound": {"$ne": 1}},
                                    {"h_idx": {"$lt": 1}},
                                    {"a_idx": {"$lte": 3}},
                                ],
                            },
                            {"d_noidx": {"$nin": [11, 4]}},
                        ],
                    },
                    {"i_idx": {"$lte": 14}},
                    {"i_idx": {"$in": [18, 14, 13]}},
                ],
                "d_compound": {"$eq": 8},
            },
        },
    ],
    /* clusterSize: 44, queryRank: 13.03 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$elemMatch": {"$gt": 13}}}, {"a_compound": {"$exists": True}}]},
                    {"k_compound": {"$in": [14, 6]}},
                ],
                "k_idx": {"$nin": [3, 20]},
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 43, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$ne": 17}},
                    {
                        "$or": [
                            {"$and": [{"a_noidx": {"$all": [19, 2, 7, 10]}}, {"a_idx": {"$all": [4, 2]}}]},
                            {"k_compound": {"$eq": 3}},
                            {"a_idx": {"$elemMatch": {"$in": [10, 6]}}},
                            {"a_compound": {"$exists": True}},
                        ],
                    },
                ],
                "i_idx": {"$eq": 19},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 230},
    ],
    /* clusterSize: 43, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [1, 13]}}, {"z_compound": {"$in": [19, 3]}}, {"i_compound": {"$gt": 17}}],
                "h_idx": {"$nin": [6, 1]},
            },
        },
        {"$project": {"k_idx": 1}},
    ],
    /* clusterSize: 43, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$eq": 1}},
                    {"a_compound": {"$lt": 8}},
                    {"$and": [{"a_idx": {"$all": [15, 13]}}, {"i_noidx": {"$exists": False}}]},
                ],
                "z_idx": {"$nin": [6, 6, 11]},
            },
        },
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 43, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"k_compound": {"$gte": 7}}, {"h_compound": {"$exists": True}}],
                "a_compound": {"$nin": [1, 4]},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 110},
    ],
    /* clusterSize: 43, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                    {"a_idx": {"$elemMatch": {"$exists": True, "$gt": 9, "$nin": [2, 16]}}},
                    {"a_compound": {"$all": [2, 16, 9]}},
                ],
                "a_noidx": {"$exists": True},
                "z_compound": {"$lte": 3},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 43, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_compound": {"$exists": True}},
                    {
                        "$or": [
                            {"i_idx": {"$nin": [3, 2]}},
                            {
                                "$or": [
                                    {"a_idx": {"$elemMatch": {"$nin": [12, 2]}}},
                                    {"c_compound": {"$exists": False}},
                                    {"c_idx": {"$exists": True}},
                                    {"a_idx": {"$all": [6, 10]}},
                                    {"a_compound": {"$all": [9, 4]}},
                                ],
                            },
                        ],
                    },
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 43, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$exists": True, "$lt": 8}}, "z_compound": {"$ne": 12}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 43, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$exists": True}},
                    {"c_compound": {"$nin": [13, 17, 2]}},
                    {"a_compound": {"$all": [19, 5, 4]}},
                ],
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 111},
    ],
    /* clusterSize: 43, queryRank: 4.03 */ [
        {
            "$match": {
                "a_compound": {"$nin": [16, 20, 16, 1]},
                "i_compound": {"$exists": True},
                "i_noidx": {"$in": [16, 8, 3]},
            },
        },
        {"$project": {"a_idx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 42, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [10, 19]}}, {"a_compound": {"$exists": True}}],
                "i_compound": {"$gte": 17},
            },
        },
        {"$project": {"_id": 0, "h_idx": 1}},
    ],
    /* clusterSize: 42, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [6, 13, 7, 2]}}, {"d_compound": {"$nin": [13, 2]}}],
                "a_idx": {"$elemMatch": {"$gte": 19, "$ne": 16}},
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 42, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$lt": 20}},
                    {"a_idx": {"$all": [14, 15, 20]}},
                    {"a_compound": {"$nin": [3, 16, 12]}},
                ],
                "a_idx": {"$elemMatch": {"$gte": 14}},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"h_noidx": 1}},
    ],
    /* clusterSize: 42, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$nin": [15, 7]}, "z_compound": {"$lte": 10}}},
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 42, queryRank: 9.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"a_noidx": {"$lt": 8}},
                    {"d_noidx": {"$lte": 15}},
                    {
                        "$or": [
                            {"a_idx": {"$gt": 10}},
                            {"a_compound": {"$exists": False}},
                            {
                                "$and": [
                                    {"$nor": [{"a_compound": {"$ne": 18}}, {"d_noidx": {"$lte": 20}}]},
                                    {"c_compound": {"$exists": False}},
                                ],
                            },
                        ],
                    },
                    {"c_idx": {"$exists": True}},
                ],
            },
        },
        {"$sort": {"d_idx": -1, "i_idx": -1}},
    ],
    /* clusterSize: 41, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [1, 13, 15]}},
                    {"a_compound": {"$exists": False}},
                    {"a_compound": {"$nin": [9, 12]}},
                ],
                "z_idx": {"$ne": 11},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 101},
    ],
    /* clusterSize: 41, queryRank: 6.02 */ [
        {
            "$match": {
                "$or": [
                    {"h_compound": {"$in": [16, 6]}},
                    {"a_compound": {"$nin": [18, 2, 4, 2]}},
                    {"i_compound": {"$lt": 8}},
                ],
                "i_idx": {"$gte": 17},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 176},
        {"$project": {"_id": 0, "h_compound": 1}},
    ],
    /* clusterSize: 41, queryRank: 18.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$in": [11, 3, 2, 3], "$ne": 8}}},
                    {"a_compound": {"$elemMatch": {"$gte": 7}}},
                ],
                "$nor": [
                    {"a_compound": {"$all": [10, 9, 7]}},
                    {"k_compound": {"$exists": False}},
                    {"z_compound": {"$in": [13, 20, 20]}},
                ],
            },
        },
    ],
    /* clusterSize: 41, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$in": [2, 1, 18]}, "d_compound": {"$exists": True}}},
        {"$sort": {"c_idx": -1}},
        {"$limit": 213},
    ],
    /* clusterSize: 41, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$nin": [8, 5, 9, 17, 3]}, "k_compound": {"$ne": 12}}},
        {"$sort": {"c_idx": -1}},
        {"$limit": 13},
    ],
    /* clusterSize: 41, queryRank: 15.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"a_compound": {"$elemMatch": {"$gte": 17, "$ne": 7}}},
                            {"a_compound": {"$all": [19, 16, 4, 1]}},
                            {"a_compound": {"$in": [20, 1, 4, 19]}},
                            {"d_noidx": {"$exists": False}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "k_noidx": {"$nin": [18, 16]},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 112},
    ],
    /* clusterSize: 41, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_idx": {"$all": [15, 16, 7]}}, {"z_compound": {"$gt": 11}}]},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$nin": [9, 8]}}},
                    {"a_idx": {"$gt": 8}},
                ],
                "a_compound": {"$in": [20, 3, 5]},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 40, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [{"c_idx": {"$in": [16, 6]}}, {"a_compound": {"$lte": 20}}, {"c_compound": {"$exists": False}}],
                "a_idx": {"$elemMatch": {"$exists": True, "$in": [16, 10, 6]}},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 40, queryRank: 5.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$nin": [7, 16]}},
                "i_idx": {"$nin": [19, 8]},
                "z_compound": {"$lte": 19},
            },
        },
    ],
    /* clusterSize: 40, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_noidx": {"$gte": 6}},
                    {"k_noidx": {"$exists": True}},
                    {"$and": [{"a_idx": {"$gte": 20}}, {"c_compound": {"$nin": [8, 14]}}]},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 109},
    ],
    /* clusterSize: 40, queryRank: 15.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [4, 5, 14, 4]}}, {"d_compound": {"$exists": True}}, {"h_idx": {"$gt": 12}}],
                "c_compound": {"$lt": 6},
                "i_compound": {"$ne": 10},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 171},
    ],
    /* clusterSize: 40, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$in": [13, 19, 1, 16]}},
                    {"c_compound": {"$lt": 7}},
                    {"a_compound": {"$elemMatch": {"$eq": 7}}},
                ],
                "h_compound": {"$eq": 3},
            },
        },
        {"$sort": {"a_idx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 40, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$gte": 11}},
                    {"a_compound": {"$elemMatch": {"$ne": 4}}},
                    {"a_compound": {"$elemMatch": {"$exists": True, "$in": [8, 4, 6], "$ne": 4}}},
                ],
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "h_compound": 1}},
    ],
    /* clusterSize: 40, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_idx": {"$all": [11, 4, 4]}}, {"a_idx": {"$eq": 15}}]},
                    {
                        "$and": [
                            {"c_compound": {"$in": [13, 1, 20, 15]}},
                            {"$or": [{"c_noidx": {"$exists": True}}, {"a_noidx": {"$all": [8, 19]}}]},
                        ],
                    },
                    {
                        "$nor": [
                            {
                                "$and": [
                                    {"a_compound": {"$elemMatch": {"$in": [12, 12, 20, 1]}}},
                                    {
                                        "$or": [
                                            {"a_idx": {"$exists": True}},
                                            {"a_noidx": {"$elemMatch": {"$gte": 15, "$in": [4, 9]}}},
                                            {"k_noidx": {"$exists": True}},
                                        ],
                                    },
                                ],
                            },
                            {"a_idx": {"$exists": True}},
                            {"a_compound": {"$gt": 6}},
                        ],
                    },
                ],
                "d_compound": {"$lte": 2},
                "d_noidx": {"$ne": 12},
            },
        },
        {"$sort": {"d_idx": 1, "h_idx": -1, "z_idx": 1}},
        {"$limit": 58},
    ],
    /* clusterSize: 40, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [13, 16, 1]}},
                    {"k_compound": {"$gt": 16}},
                    {"a_compound": {"$elemMatch": {"$eq": 19, "$exists": True}}},
                ],
                "a_compound": {"$gt": 7},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$project": {"_id": 0, "k_idx": 1}},
    ],
    /* clusterSize: 40, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$ne": 9}},
                    {"$nor": [{"a_compound": {"$in": [16, 14]}}, {"a_idx": {"$all": [16, 1]}}]},
                ],
                "k_idx": {"$in": [10, 14]},
            },
        },
        {"$sort": {"h_idx": 1, "k_idx": -1, "z_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 40, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [4, 9, 18, 2]}}, {"h_compound": {"$exists": False}}],
                "a_compound": {"$elemMatch": {"$exists": True, "$nin": [1, 6, 6]}},
            },
        },
        {"$sort": {"k_idx": 1}},
    ],
    /* clusterSize: 40, queryRank: 15.03 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$all": [1, 5, 19]}}, {"c_compound": {"$gte": 8}}]},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                ],
                "$nor": [{"i_compound": {"$in": [1, 11]}}, {"a_noidx": {"$elemMatch": {"$in": [1, 12]}}}],
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 39, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"h_compound": {"$in": [3, 3]}},
                    {"h_idx": {"$exists": True}},
                    {
                        "$and": [
                            {"a_compound": {"$all": [3, 8, 3, 4]}},
                            {"a_compound": {"$exists": False}},
                            {"z_compound": {"$nin": [11, 18, 15]}},
                        ],
                    },
                ],
                "a_noidx": {"$all": [1, 10]},
                "d_idx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 56},
    ],
    /* clusterSize: 39, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [{"d_compound": {"$eq": 20}}, {"a_compound": {"$all": [11, 16, 10]}}],
                "k_noidx": {"$ne": 5},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"_id": 0, "a_noidx": 1, "d_noidx": 1}},
    ],
    /* clusterSize: 39, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [11, 20, 8, 15, 7]}},
                    {
                        "$or": [
                            {"i_compound": {"$exists": True}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$gt": 17}}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$eq": 4}}},
                    {"h_idx": {"$in": [12, 19, 1]}},
                    {"d_idx": {"$ne": 3}},
                ],
                "a_compound": {"$elemMatch": {"$gte": 8, "$lte": 13}},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 39, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [6, 13]}},
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"i_compound": {"$gte": 5}},
                    {"a_compound": {"$elemMatch": {"$nin": [11, 9, 19]}}},
                ],
                "a_compound": {"$gte": 10},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "h_compound": 1, "k_compound": 1}},
    ],
    /* clusterSize: 39, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [
                    {"z_idx": {"$exists": False}},
                    {"a_idx": {"$elemMatch": {"$in": [14, 8, 2, 18, 5]}}},
                    {"a_compound": {"$all": [6, 10]}},
                ],
                "a_noidx": {"$elemMatch": {"$gte": 2}},
            },
        },
        {"$sort": {"d_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 39, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"d_compound": {"$exists": True}},
                            {"a_compound": {"$nin": [20, 10, 18]}},
                            {"a_compound": {"$elemMatch": {"$in": [19, 4, 9], "$lte": 5}}},
                        ],
                    },
                    {"i_idx": {"$lte": 10}},
                    {"a_idx": {"$gt": 8}},
                ],
                "c_compound": {"$nin": [18, 9]},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 39, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$eq": 16, "$exists": True}}},
                            {"a_idx": {"$all": [7, 14]}},
                            {
                                "$or": [
                                    {"c_compound": {"$lte": 11}},
                                    {"a_compound": {"$elemMatch": {"$in": [19, 6, 14], "$nin": [5, 12, 11, 16, 15]}}},
                                    {"h_idx": {"$gt": 5}},
                                    {"i_compound": {"$nin": [6, 9]}},
                                ],
                            },
                        ],
                    },
                    {"d_compound": {"$exists": True}},
                ],
                "h_compound": {"$gt": 17},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 38, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$elemMatch": {"$lte": 17}}}, {"z_compound": {"$nin": [9, 9]}}],
                "a_compound": {"$eq": 2},
            },
        },
    ],
    /* clusterSize: 38, queryRank: 8.03 */ [
        {
            "$match": {
                "$nor": [{"k_noidx": {"$in": [15, 4]}}, {"z_idx": {"$gte": 12}}, {"a_compound": {"$all": [15, 10]}}],
                "a_compound": {"$elemMatch": {"$nin": [2, 15]}},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 38, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [3, 18]}}, {"a_idx": {"$elemMatch": {"$in": [8, 2]}}}],
                "h_noidx": {"$in": [5, 11]},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 38, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [{"k_idx": {"$exists": False}}, {"i_noidx": {"$nin": [9, 11, 14, 2]}}],
                "$or": [{"a_idx": {"$all": [20, 7, 15, 7]}}, {"a_idx": {"$gte": 19}}, {"a_compound": {"$all": [2, 4]}}],
            },
        },
        {"$sort": {"c_idx": 1, "k_idx": -1, "z_idx": 1}},
    ],
    /* clusterSize: 38, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$lt": 12}}, {"a_compound": {"$in": [11, 12, 3, 11]}}],
                "a_idx": {"$elemMatch": {"$exists": True, "$lte": 17}},
            },
        },
    ],
    /* clusterSize: 38, queryRank: 8.02 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$ne": 2}}, {"a_idx": {"$all": [5, 16]}}],
                "a_idx": {"$elemMatch": {"$nin": [3, 11, 5, 10]}},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"a_noidx": 1, "h_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 38, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$elemMatch": {"$gt": 12}}}, {"a_idx": {"$all": [14, 5, 19]}}],
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$limit": 177},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 38, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [{"d_compound": {"$exists": True}}, {"k_compound": {"$nin": [19, 18, 20, 19]}}],
                "a_compound": {"$eq": 4},
                "k_compound": {"$nin": [3, 13, 17]},
            },
        },
    ],
    /* clusterSize: 38, queryRank: 15.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_idx": {"$exists": True}},
                    {
                        "$and": [
                            {"a_compound": {"$all": [6, 12, 11]}},
                            {"a_compound": {"$nin": [12, 2, 4]}},
                            {"a_idx": {"$all": [10, 3]}},
                        ],
                    },
                ],
                "a_compound": {"$all": [2, 20]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 38, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [{"c_compound": {"$exists": False}}, {"i_compound": {"$exists": False}}],
                "$or": [
                    {"a_compound": {"$all": [12, 15, 18]}},
                    {"h_compound": {"$gte": 14}},
                    {"i_compound": {"$nin": [12, 12, 11]}},
                    {"i_compound": {"$exists": False}},
                ],
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 38, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"h_compound": {"$in": [4, 14, 13, 16, 7, 9]}}, {"a_compound": {"$all": [1, 7, 16, 14, 15]}}],
                "d_noidx": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 37, queryRank: 6.03 */ [{"$match": {"a_compound": {"$all": [2, 3]}, "a_idx": {"$gte": 6}}}],
    /* clusterSize: 37, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"a_compound": {"$all": [3, 20, 12, 13, 1]}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 37, queryRank: 15.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$exists": False}},
                    {"i_noidx": {"$nin": [10, 9]}},
                    {"a_compound": {"$all": [16, 11, 18, 9]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"_id": 0, "d_compound": 1}},
    ],
    /* clusterSize: 37, queryRank: 5.03 */ [
        {
            "$match": {
                "a_compound": {"$eq": 2},
                "a_idx": {"$elemMatch": {"$nin": [10, 20, 5]}},
                "k_compound": {"$gte": 5},
            },
        },
        {"$project": {"_id": 0, "h_compound": 1, "k_compound": 1, "k_idx": 1}},
    ],
    /* clusterSize: 37, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [{"c_noidx": {"$exists": True}}, {"a_idx": {"$gte": 3}}, {"d_compound": {"$lte": 17}}],
                "a_compound": {"$exists": True},
                "i_compound": {"$gte": 3},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 37, queryRank: 4.03 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$all": [20, 9]}}, {"a_idx": {"$all": [1, 9]}}],
                "a_compound": {"$lte": 10},
                "a_noidx": {"$lt": 13},
            },
        },
    ],
    /* clusterSize: 36, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_idx": {"$lt": 7}},
                    {
                        "$or": [
                            {"a_idx": {"$all": [6, 20, 11, 2]}},
                            {"a_compound": {"$nin": [12, 17]}},
                            {
                                "$and": [
                                    {"a_compound": {"$elemMatch": {"$exists": False, "$ne": 12}}},
                                    {"c_noidx": {"$exists": True}},
                                    {"z_noidx": {"$in": [2, 5, 15]}},
                                ],
                            },
                        ],
                    },
                ],
                "a_idx": {"$elemMatch": {"$exists": True, "$ne": 18}},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$project": {"_id": 0, "d_idx": 1, "d_noidx": 1}},
    ],
    /* clusterSize: 36, queryRank: 4.03 */ [
        {"$match": {"c_compound": {"$nin": [16, 17]}, "c_idx": {"$ne": 12}}},
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 36, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"z_compound": {"$nin": [12, 8, 10]}},
                    {"k_compound": {"$gte": 10}},
                ],
                "a_idx": {"$in": [2, 19]},
            },
        },
    ],
    /* clusterSize: 36, queryRank: 7.02 */ [
        {"$match": {"a_compound": {"$exists": True}, "k_compound": {"$ne": 3}, "z_compound": {"$lte": 10}}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 87},
        {"$project": {"_id": 0, "a_idx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 36, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$nin": [16, 14, 17, 16]}}, {"a_compound": {"$all": [10, 18, 14]}}],
                "h_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 36, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [4, 14, 7, 17]}}, {"a_compound": {"$nin": [7, 16]}}],
                "a_compound": {"$gte": 14},
                "a_noidx": {"$lte": 4},
            },
        },
        {"$limit": 186},
        {"$project": {"h_idx": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 36, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$nin": [17, 2, 14]}}, {"h_compound": {"$lte": 4}}],
                "$or": [
                    {"a_idx": {"$in": [7, 19, 5]}},
                    {"$nor": [{"c_idx": {"$nin": [19, 17]}}, {"k_compound": {"$gte": 15}}]},
                    {"a_compound": {"$all": [2, 18, 9]}},
                ],
                "a_noidx": {"$gt": 14},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 36, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$in": [16, 9]}},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$exists": False, "$lt": 19}}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$in": [14, 9]}}},
                        ],
                    },
                    {"z_idx": {"$lt": 20}},
                    {"a_idx": {"$all": [5, 8]}},
                ],
                "h_idx": {"$gte": 7},
            },
        },
    ],
    /* clusterSize: 36, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [14, 4, 2, 11]}},
                    {"a_compound": {"$exists": False}},
                    {"h_noidx": {"$nin": [13, 2]}},
                ],
                "c_noidx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 35, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"d_idx": {"$in": [3, 10]}}, {"z_compound": {"$nin": [7, 4]}}],
                "a_compound": {"$gt": 1},
                "a_noidx": {"$lt": 12},
            },
        },
        {"$project": {"a_compound": 1, "a_idx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 35, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [16, 8]}},
                    {"a_idx": {"$all": [3, 5, 18]}},
                    {"$or": [{"a_idx": {"$all": [6, 19]}}, {"z_compound": {"$exists": True}}]},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 35, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$exists": False}}},
                    {"h_compound": {"$in": [11, 18, 16]}},
                    {"i_idx": {"$exists": True}},
                    {"a_compound": {"$all": [10, 1]}},
                    {"a_compound": {"$eq": 20}},
                ],
                "a_compound": {"$gte": 18},
            },
        },
        {"$project": {"a_compound": 1, "k_compound": 1}},
    ],
    /* clusterSize: 35, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$elemMatch": {"$lte": 3, "$nin": [14, 7, 20]}}},
                    {"z_compound": {"$lt": 3}},
                    {"a_compound": {"$all": [9, 13, 15]}},
                    {"a_idx": {"$in": [12, 10]}},
                ],
                "z_idx": {"$gt": 1},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 35, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {
                                "$and": [
                                    {"a_idx": {"$all": [18, 6, 2, 17]}},
                                    {"a_idx": {"$gt": 7}},
                                    {"a_compound": {"$eq": 2}},
                                ],
                            },
                            {"a_compound": {"$elemMatch": {"$eq": 10}}},
                            {"a_noidx": {"$exists": True}},
                            {"a_compound": {"$elemMatch": {"$lte": 7}}},
                            {"d_compound": {"$exists": True}},
                        ],
                    },
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$all": [17, 5]}},
                ],
                "h_compound": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "c_idx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 35, queryRank: 7.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"z_idx": {"$ne": 13}},
                            {"k_compound": {"$lt": 5}},
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                        ],
                    },
                    {"z_compound": {"$lte": 15}},
                ],
                "a_idx": {"$lte": 10},
            },
        },
        {"$sort": {"k_idx": 1, "z_idx": -1}},
        {"$skip": 6},
    ],
    /* clusterSize: 35, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_compound": {"$in": [3, 20, 18]}},
                ],
                "a_compound": {"$nin": [10, 5, 17]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 35, queryRank: 11.03 */ [
        {"$match": {"$or": [{"a_idx": {"$all": [17, 10]}}, {"k_compound": {"$eq": 11}}, {"c_compound": {"$lt": 5}}]}},
        {"$sort": {"a_idx": 1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 35, queryRank: 9.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [13, 17, 6]}}, {"a_compound": {"$elemMatch": {"$exists": True, "$lte": 8}}}],
                "a_noidx": {"$eq": 11},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 137},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 34, queryRank: 4.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$nin": [7, 19]}},
                "d_noidx": {"$nin": [12, 2, 19, 2]},
                "z_idx": {"$nin": [15, 2]},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 34, queryRank: 10.03 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$all": [14, 9, 19, 8]}}, {"k_idx": {"$exists": True}}]},
                    {"a_noidx": {"$elemMatch": {"$eq": 4}}},
                ],
                "a_idx": {"$lte": 6},
            },
        },
    ],
    /* clusterSize: 34, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"z_idx": {"$nin": [16, 18, 2]}}, {"k_idx": {"$nin": [7, 6, 2]}}]},
                    {"a_idx": {"$gt": 12}},
                    {"a_compound": {"$nin": [8, 1]}},
                    {"i_idx": {"$gt": 3}},
                ],
                "a_compound": {"$exists": True},
                "a_noidx": {"$lte": 16},
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 225},
    ],
    /* clusterSize: 34, queryRank: 11.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$ne": 6}},
                    {
                        "$or": [
                            {"a_compound": {"$all": [2, 14, 5]}},
                            {"a_noidx": {"$all": [17, 6]}},
                            {"k_compound": {"$exists": True}},
                        ],
                    },
                    {"a_compound": {"$exists": True}},
                ],
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 34, queryRank: 7.03 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$lt": 15}}, {"a_compound": {"$nin": [15, 13, 2]}}, {"a_idx": {"$gte": 15}}],
                "i_compound": {"$nin": [4, 7, 1]},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 34, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$and": [{"i_compound": {"$gt": 6}}, {"h_idx": {"$nin": [10, 19]}}, {"a_compound": {"$lt": 1}}]},
                    {"h_noidx": {"$exists": False}},
                ],
                "h_compound": {"$exists": True},
                "z_compound": {"$lte": 18},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"a_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 34, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$exists": True}},
                            {"a_idx": {"$nin": [2, 9, 5, 11]}},
                            {"a_idx": {"$all": [7, 16, 13]}},
                            {"d_idx": {"$exists": True}},
                            {"a_compound": {"$eq": 7}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$gt": 20, "$gte": 3}}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 34, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [16, 14, 10, 13]}},
                    {"a_idx": {"$all": [20, 13, 8]}},
                    {"$or": [{"i_idx": {"$gt": 11}}, {"z_compound": {"$in": [9, 3, 13, 5]}}]},
                ],
                "h_compound": {"$nin": [7, 6]},
            },
        },
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 34, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"h_compound": {"$exists": False}},
                    {"a_compound": {"$all": [16, 6, 7]}},
                    {"c_compound": {"$ne": 13}},
                ],
                "i_compound": {"$lte": 1},
            },
        },
        {"$sort": {"a_idx": 1, "i_idx": -1, "k_idx": 1}},
        {"$limit": 186},
    ],
    /* clusterSize: 33, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [13, 9, 10]}},
                    {"$and": [{"z_idx": {"$lte": 12}}, {"a_compound": {"$lt": 5}}, {"c_compound": {"$gte": 1}}]},
                ],
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 33, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$exists": True}}, {"a_compound": {"$all": [1, 7]}}],
                "a_compound": {"$gte": 1},
            },
        },
        {"$sort": {"i_idx": 1, "z_idx": 1}},
        {"$limit": 65},
    ],
    /* clusterSize: 33, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$nin": [20, 6]}},
                    {"a_compound": {"$all": [14, 11]}},
                    {"a_idx": {"$all": [20, 7, 4]}},
                ],
                "a_compound": {"$in": [19, 1]},
            },
        },
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 33, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$nin": [4, 20, 3, 2, 12]}}},
                    {"a_compound": {"$all": [4, 12, 17, 20]}},
                ],
                "c_compound": {"$exists": True},
            },
        },
        {"$sort": {"c_idx": -1, "i_idx": -1, "z_idx": -1}},
    ],
    /* clusterSize: 33, queryRank: 5.03 */ [
        {"$match": {"c_compound": {"$exists": True}, "k_compound": {"$ne": 1}}},
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 33, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [19, 8, 10]}},
                    {"a_noidx": {"$in": [17, 8, 17, 19]}},
                    {"i_compound": {"$exists": False}},
                ],
                "c_idx": {"$exists": True},
            },
        },
        {"$limit": 187},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 33, queryRank: 14.03 */ [
        {
            "$match": {
                "$or": [{"k_compound": {"$gt": 11}}, {"a_compound": {"$all": [5, 4, 3, 4, 1, 12]}}],
                "a_compound": {"$in": [16, 20]},
                "d_compound": {"$gt": 3},
            },
        },
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 32, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"k_idx": {"$in": [3, 9]}}, {"i_idx": {"$gt": 12}}, {"a_compound": {"$all": [19, 19, 12, 5]}}],
                "a_compound": {"$in": [9, 3, 2]},
            },
        },
        {"$project": {"_id": 0, "z_noidx": 1}},
    ],
    /* clusterSize: 32, queryRank: 9.03 */ [
        {"$match": {"$and": [{"a_idx": {"$elemMatch": {"$lt": 4, "$lte": 8}}}, {"a_compound": {"$all": [2, 13, 3]}}]}},
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 32, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$gte": 1}}, {"a_compound": {"$exists": True}}],
                "$or": [
                    {"a_compound": {"$ne": 13}},
                    {"a_idx": {"$all": [4, 5, 19]}},
                    {"$and": [{"h_compound": {"$exists": False}}, {"d_noidx": {"$exists": False}}]},
                ],
            },
        },
        {"$limit": 57},
    ],
    /* clusterSize: 32, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"h_idx": {"$gte": 11}}, {"a_idx": {"$elemMatch": {"$exists": True, "$in": [14, 10]}}}]},
                    {"c_compound": {"$gte": 4}},
                    {"a_idx": {"$in": [4, 1, 2]}},
                ],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1, "i_idx": 1, "z_idx": -1}},
    ],
    /* clusterSize: 32, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"a_idx": {"$exists": False}},
                            {
                                "$nor": [
                                    {"d_compound": {"$in": [12, 17]}},
                                    {"a_compound": {"$all": [5, 10, 1]}},
                                    {"c_idx": {"$lte": 2}},
                                ],
                            },
                            {"a_noidx": {"$lt": 2}},
                            {"z_compound": {"$exists": False}},
                        ],
                    },
                    {"a_idx": {"$lt": 12}},
                ],
                "$or": [{"a_idx": {"$exists": True}}, {"d_noidx": {"$in": [20, 2, 8, 16]}}],
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "d_noidx": 1}},
    ],
    /* clusterSize: 32, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [18, 4]}},
                    {"a_noidx": {"$elemMatch": {"$nin": [3, 8]}}},
                    {"a_idx": {"$elemMatch": {"$in": [17, 19, 1], "$lt": 15}}},
                ],
                "a_idx": {"$elemMatch": {"$eq": 9}},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 32, queryRank: 4.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$lt": 20, "$nin": [11, 7, 8]}},
                "i_compound": {"$nin": [13, 1, 13]},
            },
        },
        {"$sort": {"a_idx": 1, "h_idx": -1}},
    ],
    /* clusterSize: 32, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$lte": 8}},
                    {"k_idx": {"$ne": 8}},
                    {"d_compound": {"$gte": 17}},
                    {"a_compound": {"$all": [10, 8]}},
                ],
                "a_noidx": {"$elemMatch": {"$gt": 3, "$lte": 20, "$nin": [15, 1, 9]}},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 32, queryRank: 11.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$exists": True}},
                    {"$or": [{"a_compound": {"$all": [6, 5]}}, {"z_compound": {"$nin": [8, 13]}}]},
                ],
            },
        },
        {"$limit": 220},
    ],
    /* clusterSize: 32, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$exists": True}},
                    {"d_compound": {"$exists": True}},
                    {"a_idx": {"$lt": 15}},
                    {"k_idx": {"$lt": 16}},
                ],
                "a_compound": {"$elemMatch": {"$in": [9, 13, 20]}},
                "a_idx": {"$gt": 1},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 95},
    ],
    /* clusterSize: 31, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [4, 1]}},
                    {
                        "$or": [
                            {"a_idx": {"$all": [7, 17, 18]}},
                            {"a_compound": {"$all": [12, 9, 19]}},
                            {"k_compound": {"$gte": 7}},
                        ],
                    },
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 31, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"$or": [{"a_compound": {"$all": [4, 5, 20]}}, {"a_compound": {"$in": [7, 8, 17]}}]},
                    {"a_compound": {"$elemMatch": {"$eq": 11}}},
                    {"a_compound": {"$exists": False}},
                ],
                "a_compound": {"$elemMatch": {"$in": [20, 19]}},
            },
        },
    ],
    /* clusterSize: 31, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"h_idx": {"$in": [18, 11]}},
                            {
                                "$and": [
                                    {"a_idx": {"$all": [9, 7]}},
                                    {"a_noidx": {"$exists": False}},
                                    {"a_idx": {"$gte": 11}},
                                    {"k_compound": {"$lt": 8}},
                                ],
                            },
                            {"a_idx": {"$in": [18, 14]}},
                        ],
                    },
                    {"k_compound": {"$gt": 7}},
                ],
                "h_compound": {"$lte": 11},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 31, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_idx": {"$gt": 20}},
                    {"z_compound": {"$lte": 5}},
                ],
                "k_compound": {"$gte": 9},
            },
        },
    ],
    /* clusterSize: 31, queryRank: 4.03 */ [{"$match": {"k_compound": {"$lte": 8}, "z_compound": {"$gte": 1}}}],
    /* clusterSize: 31, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": False}},
                    {"h_idx": {"$lt": 6}},
                    {"z_idx": {"$nin": [3, 20, 11]}},
                    {"a_compound": {"$exists": False}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 31, queryRank: 5.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [10, 16, 10]}},
                    {
                        "$nor": [
                            {"k_compound": {"$ne": 5}},
                            {"h_compound": {"$eq": 3}},
                            {"a_noidx": {"$lte": 5}},
                            {"a_noidx": {"$exists": False}},
                            {"z_compound": {"$gte": 6}},
                            {"a_compound": {"$exists": True}},
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$in": [7, 12], "$lte": 2}}},
                ],
                "a_compound": {"$eq": 10},
                "a_noidx": {"$elemMatch": {"$gte": 6}},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 30, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$ne": 7}},
                    {
                        "$and": [
                            {"a_idx": {"$nin": [17, 11, 10]}},
                            {"a_idx": {"$ne": 13}},
                            {"$nor": [{"a_idx": {"$eq": 3}}, {"d_noidx": {"$lt": 10}}]},
                            {"a_compound": {"$all": [15, 9, 17]}},
                            {"a_idx": {"$elemMatch": {"$lte": 20}}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$in": [19, 1]}}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 30, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_noidx": {"$nin": [5, 4]}},
                    {"a_noidx": {"$elemMatch": {"$gt": 3}}},
                    {"k_noidx": {"$nin": [9, 4]}},
                    {
                        "$or": [
                            {"d_idx": {"$gte": 13}},
                            {"a_compound": {"$all": [19, 9]}},
                            {"h_idx": {"$exists": True}},
                            {"i_compound": {"$ne": 4}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 207},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 30, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$nin": [2, 17, 14, 16, 20]}},
                    {"a_compound": {"$nin": [9, 17, 11, 20, 16, 11, 6, 19]}},
                ],
                "h_idx": {"$exists": True},
            },
        },
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 30, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$nin": [6, 18, 11, 19]}}, {"a_compound": {"$in": [16, 3]}}],
                "a_compound": {"$lte": 3},
            },
        },
    ],
    /* clusterSize: 30, queryRank: 7.02 */ [
        {
            "$match": {
                "$and": [
                    {"z_idx": {"$gte": 5}},
                    {"k_noidx": {"$nin": [6, 16]}},
                    {"c_compound": {"$lte": 9}},
                    {"a_compound": {"$in": [1, 6]}},
                ],
                "a_compound": {"$elemMatch": {"$exists": True, "$nin": [13, 14]}},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 30, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"d_compound": {"$gte": 5}}, {"k_compound": {"$gt": 10}}, {"z_compound": {"$in": [12, 14]}}],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 69},
    ],
    /* clusterSize: 29, queryRank: 8.02 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$elemMatch": {"$eq": 10}}}, {"c_compound": {"$in": [4, 3, 11]}}],
                "a_compound": {"$exists": True},
                "a_idx": {"$gte": 7},
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 29, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"c_compound": {"$exists": False}},
                    {"a_compound": {"$all": [6, 13, 3, 6, 15]}},
                    {"h_noidx": {"$lte": 19}},
                ],
                "a_noidx": {"$elemMatch": {"$exists": True, "$in": [7, 8], "$nin": [5, 18, 18]}},
                "c_noidx": {"$nin": [16, 15]},
            },
        },
    ],
    /* clusterSize: 29, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"i_compound": {"$eq": 3}},
                    {"a_compound": {"$all": [17, 13, 3, 7]}},
                    {"i_noidx": {"$exists": False}},
                ],
                "c_noidx": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$limit": 29},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 29, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [{"i_noidx": {"$gte": 20}}, {"a_idx": {"$exists": True}}],
                "$or": [
                    {"a_compound": {"$all": [12, 5, 4]}},
                    {"a_idx": {"$elemMatch": {"$exists": True, "$gte": 10}}},
                    {"a_idx": {"$all": [4, 6]}},
                ],
            },
        },
        {"$sort": {"a_idx": 1, "h_idx": 1, "i_idx": 1}},
        {"$project": {"a_idx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 29, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "a_idx": {"$exists": True}, "h_noidx": {"$in": [19, 8, 20]}}},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 29, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$nin": [6, 10]}},
                    {"a_compound": {"$nin": [20, 5]}},
                    {"a_compound": {"$all": [3, 13]}},
                ],
                "a_idx": {"$elemMatch": {"$lt": 7}},
                "a_noidx": {"$nin": [11, 2, 15]},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 29, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [{"k_noidx": {"$gt": 4}}, {"i_compound": {"$gt": 14}}],
                "$nor": [{"a_compound": {"$all": [2, 10]}}, {"a_compound": {"$nin": [17, 16]}}],
                "z_noidx": {"$ne": 3},
            },
        },
        {"$project": {"a_noidx": 1, "d_compound": 1}},
    ],
    /* clusterSize: 29, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$nin": [5, 14, 8]}}},
                    {"a_idx": {"$all": [2, 6]}},
                    {"a_compound": {"$elemMatch": {"$nin": [7, 5]}}},
                    {
                        "$and": [
                            {
                                "$nor": [
                                    {"h_compound": {"$ne": 19}},
                                    {"d_idx": {"$nin": [11, 18, 12, 1]}},
                                    {"i_noidx": {"$ne": 3}},
                                ],
                            },
                            {"k_compound": {"$gt": 7}},
                        ],
                    },
                ],
                "a_noidx": {"$nin": [6, 9]},
                "c_compound": {"$ne": 12},
                "k_idx": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 29, queryRank: 4.03 */ [{"$match": {"i_compound": {"$in": [3, 16]}, "k_compound": {"$lte": 13}}}],
    /* clusterSize: 29, queryRank: 14.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [17, 20]}},
                    {"i_idx": {"$exists": True}},
                    {"c_compound": {"$in": [19, 9]}},
                ],
                "a_compound": {"$all": [1, 9]},
            },
        },
    ],
    /* clusterSize: 28, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$exists": False}}, {"a_noidx": {"$exists": False}}],
                "$or": [
                    {
                        "$and": [
                            {"i_idx": {"$nin": [17, 3]}},
                            {"a_idx": {"$exists": False}},
                            {"i_compound": {"$nin": [2, 20, 6, 18]}},
                            {"h_idx": {"$lte": 1}},
                            {"z_compound": {"$lt": 15}},
                            {"d_compound": {"$exists": False}},
                            {"i_compound": {"$exists": False}},
                        ],
                    },
                    {"a_idx": {"$ne": 14}},
                ],
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 28, queryRank: 9.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [8, 14]}},
                    {"c_idx": {"$exists": True}},
                    {"d_idx": {"$exists": True}},
                    {"z_idx": {"$nin": [10, 15]}},
                    {"a_idx": {"$lt": 15}},
                ],
                "$nor": [{"a_noidx": {"$eq": 12}}, {"c_compound": {"$gte": 6}}],
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 28, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$nin": [11, 17]}}, {"a_compound": {"$in": [18, 15, 10]}}],
                "k_compound": {"$lte": 8},
            },
        },
        {"$limit": 171},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 28, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "i_compound": {"$gte": 20}}},
        {"$sort": {"i_idx": 1, "z_idx": -1}},
    ],
    /* clusterSize: 28, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {"i_idx": {"$lt": 13}},
                    {
                        "$or": [
                            {"a_idx": {"$lte": 8}},
                            {"h_idx": {"$exists": False}},
                            {"a_compound": {"$all": [16, 5, 20]}},
                        ],
                    },
                    {"k_idx": {"$nin": [9, 3, 7]}},
                ],
                "d_noidx": {"$lt": 13},
                "k_idx": {"$in": [20, 3]},
            },
        },
    ],
    /* clusterSize: 28, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$gte": 12}}, "a_idx": {"$gt": 6}}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 40},
    ],
    /* clusterSize: 28, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$nor": [
                            {
                                "$and": [
                                    {"a_noidx": {"$elemMatch": {"$ne": 7}}},
                                    {"a_idx": {"$all": [2, 5, 3]}},
                                    {"k_noidx": {"$nin": [13, 3, 11, 19]}},
                                    {"a_noidx": {"$all": [20, 6]}},
                                ],
                            },
                            {"z_idx": {"$eq": 2}},
                            {
                                "$and": [
                                    {"a_idx": {"$gt": 2}},
                                    {"h_compound": {"$nin": [7, 18]}},
                                    {"d_compound": {"$lt": 13}},
                                ],
                            },
                            {"a_idx": {"$all": [5, 1]}},
                        ],
                    },
                    {"z_idx": {"$nin": [1, 4]}},
                ],
                "i_compound": {"$exists": True},
            },
        },
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 28, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$lt": 20}}, "k_compound": {"$nin": [6, 20]}}},
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 28, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [{"z_compound": {"$exists": True}}, {"a_compound": {"$elemMatch": {"$lt": 18}}}],
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1, "h_idx": 1}},
        {"$limit": 1},
    ],
    /* clusterSize: 28, queryRank: 14.02 */ [
        {
            "$match": {
                "$or": [
                    {"d_idx": {"$exists": True}},
                    {
                        "$or": [
                            {"a_compound": {"$ne": 3}},
                            {"a_compound": {"$elemMatch": {"$in": [11, 17]}}},
                            {"a_idx": {"$all": [19, 13, 16]}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$gte": 14}},
                "a_noidx": {"$elemMatch": {"$gte": 12}},
                "k_compound": {"$nin": [10, 2]},
            },
        },
        {"$sort": {"c_idx": 1, "h_idx": -1}},
        {"$limit": 142},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 28, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$lte": 2}}, "z_compound": {"$exists": True}}},
        {"$project": {"k_idx": 1}},
    ],
    /* clusterSize: 28, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [7, 16]}}, {"d_compound": {"$exists": True}}],
                "a_compound": {"$elemMatch": {"$eq": 11, "$exists": True}},
            },
        },
    ],
    /* clusterSize: 28, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "a_idx": {"$exists": True}, "h_noidx": {"$in": [11, 10]}}},
        {"$sort": {"i_idx": -1, "k_idx": 1}},
    ],
    /* clusterSize: 28, queryRank: 9.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$exists": False}},
                    {
                        "$nor": [
                            {"a_compound": {"$elemMatch": {"$lt": 7, "$nin": [14, 3]}}},
                            {"k_compound": {"$nin": [11, 17, 13]}},
                        ],
                    },
                    {"z_compound": {"$ne": 3}},
                ],
                "a_idx": {"$elemMatch": {"$lte": 12}},
                "i_noidx": {"$gte": 15},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 42},
    ],
    /* clusterSize: 28, queryRank: 7.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$gte": 15}},
                    {
                        "$and": [
                            {"a_compound": {"$elemMatch": {"$gte": 5, "$nin": [2, 12, 17, 13, 15, 14]}}},
                            {"z_idx": {"$nin": [20, 4]}},
                            {"i_compound": {"$ne": 12}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 28, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [{"h_compound": {"$gte": 5}}, {"a_compound": {"$elemMatch": {"$lt": 8, "$nin": [10, 19]}}}],
                "$or": [
                    {"a_compound": {"$nin": [18, 15]}},
                    {"a_idx": {"$exists": False}},
                    {"a_compound": {"$exists": True}},
                    {"a_idx": {"$all": [4, 17]}},
                ],
            },
        },
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 27, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$exists": True}},
                    {"a_compound": {"$all": [1, 6, 13]}},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$in": [11, 6]}}},
                ],
                "i_idx": {"$eq": 6},
            },
        },
    ],
    /* clusterSize: 27, queryRank: 9.03 */ [
        {
            "$match": {
                "$and": [{"d_compound": {"$lte": 5}}, {"a_compound": {"$nin": [14, 1]}}],
                "$or": [{"k_compound": {"$exists": True}}, {"a_idx": {"$exists": False}}],
                "k_idx": {"$nin": [9, 20, 8]},
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 27, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$nin": [8, 5, 16, 3, 7]}, "a_idx": {"$elemMatch": {"$lte": 19}}}},
        {"$sort": {"k_idx": 1}},
        {"$project": {"_id": 0, "a_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 27, queryRank: 7.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$and": [
                            {"c_idx": {"$lte": 15}},
                            {"a_idx": {"$nin": [8, 3]}},
                            {"$or": [{"h_compound": {"$exists": False}}, {"z_compound": {"$exists": True}}]},
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                ],
                "a_compound": {"$nin": [14, 8, 6, 1]},
            },
        },
        {"$sort": {"d_idx": 1, "k_idx": 1}},
        {"$project": {"_id": 0, "h_compound": 1}},
    ],
    /* clusterSize: 26, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$lt": 18}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$nin": [4, 11, 2]}}},
                            {"a_compound": {"$eq": 19}},
                        ],
                    },
                    {"a_compound": {"$eq": 4}},
                ],
                "i_noidx": {"$gte": 6},
            },
        },
        {"$project": {"a_compound": 1, "a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 26, queryRank: 12.02 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_compound": {"$all": [20, 6, 17, 5]}}, {"z_compound": {"$lte": 14}}]},
                    {"z_idx": {"$lte": 10}},
                ],
                "c_compound": {"$in": [1, 3, 2]},
            },
        },
        {"$sort": {"h_idx": 1, "i_idx": -1}},
        {"$limit": 90},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 26, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"k_idx": {"$nin": [11, 18]}},
                    {"z_compound": {"$nin": [12, 1]}},
                    {"a_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                ],
                "a_compound": {"$elemMatch": {"$gt": 2}},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 26, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [5, 4, 2, 12, 8]}},
                    {"a_noidx": {"$exists": False}},
                    {"k_idx": {"$in": [14, 17, 19, 11]}},
                ],
                "a_compound": {"$gte": 15},
            },
        },
        {"$sort": {"a_idx": 1, "i_idx": -1, "z_idx": -1}},
    ],
    /* clusterSize: 26, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [17, 1, 10]}}, {"a_noidx": {"$nin": [13, 3]}}],
                "a_noidx": {"$elemMatch": {"$exists": True, "$lt": 9}},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 25, queryRank: 3.03 */ [
        {"$match": {"c_compound": {"$in": [16, 1, 5]}, "i_idx": {"$exists": True}}},
    ],
    /* clusterSize: 25, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$exists": False}},
                    {
                        "$nor": [
                            {"a_idx": {"$all": [8, 4, 7]}},
                            {"d_compound": {"$exists": False}},
                            {"a_compound": {"$elemMatch": {"$exists": False, "$nin": [7, 8]}}},
                            {"a_idx": {"$all": [10, 2, 9]}},
                            {"a_compound": {"$all": [9, 11]}},
                            {"a_idx": {"$lte": 20}},
                        ],
                    },
                ],
                "a_idx": {"$gte": 3},
            },
        },
        {"$sort": {"a_idx": 1, "c_idx": 1}},
        {"$limit": 36},
    ],
    /* clusterSize: 25, queryRank: 6.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$in": [7, 6, 14]}}},
                    {"a_idx": {"$nin": [6, 12, 16, 1]}},
                    {"a_idx": {"$elemMatch": {"$in": [5, 3, 4, 13]}}},
                    {"a_idx": {"$all": [14, 8, 8, 19, 18]}},
                ],
                "h_noidx": {"$gte": 9},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 215},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 25, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [{"d_compound": {"$nin": [9, 20]}}, {"a_compound": {"$in": [18, 12]}}],
                "a_compound": {"$ne": 19},
            },
        },
    ],
    /* clusterSize: 25, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$gt": 10}}, {"a_compound": {"$all": [9, 7]}}, {"h_compound": {"$exists": False}}],
                "c_idx": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 25, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [12, 16]}},
                    {"a_idx": {"$exists": True}},
                    {"k_compound": {"$in": [13, 17, 2]}},
                ],
                "k_compound": {"$in": [15, 3, 7, 19]},
            },
        },
    ],
    /* clusterSize: 25, queryRank: 4.03 */ [
        {"$match": {"d_compound": {"$nin": [7, 19, 10]}, "h_idx": {"$gte": 8}}},
        {"$sort": {"a_idx": 1}},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 25, queryRank: 3.03 */ [{"$match": {"a_idx": {"$in": [15, 15]}, "i_compound": {"$ne": 14}}}],
    /* clusterSize: 25, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {"$or": [{"a_idx": {"$all": [16, 3, 13, 6]}}, {"a_compound": {"$lt": 16}}]},
                    {"h_compound": {"$ne": 5}},
                    {"k_idx": {"$in": [17, 8]}},
                ],
                "$nor": [
                    {
                        "$nor": [
                            {
                                "$nor": [
                                    {"a_idx": {"$elemMatch": {"$nin": [20, 5, 1, 18]}}},
                                    {"i_compound": {"$in": [2, 15, 18]}},
                                ],
                            },
                            {"i_idx": {"$in": [7, 17]}},
                            {"h_compound": {"$exists": False}},
                        ],
                    },
                    {"z_noidx": {"$in": [7, 6]}},
                ],
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 95},
    ],
    /* clusterSize: 25, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$lt": 12}, "a_noidx": {"$lt": 2}, "k_idx": {"$exists": True}}},
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 25, queryRank: 14.02 */ [
        {
            "$match": {
                "$and": [
                    {"h_noidx": {"$gte": 15}},
                    {
                        "$nor": [
                            {"a_compound": {"$all": [17, 4]}},
                            {"i_compound": {"$eq": 4}},
                            {"k_compound": {"$in": [20, 5, 10]}},
                        ],
                    },
                ],
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$skip": 57},
    ],
    /* clusterSize: 25, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_idx": {"$elemMatch": {"$eq": 10}}},
                            {"z_idx": {"$exists": True}},
                            {
                                "$and": [
                                    {"i_idx": {"$in": [11, 12, 4]}},
                                    {"a_idx": {"$all": [4, 20, 1, 20, 5]}},
                                    {
                                        "$and": [
                                            {"a_idx": {"$gte": 16}},
                                            {"i_noidx": {"$nin": [8, 16]}},
                                            {"i_noidx": {"$in": [12, 14]}},
                                            {"h_noidx": {"$ne": 17}},
                                        ],
                                    },
                                ],
                            },
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$in": [1, 14]}}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 24, queryRank: 5.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$gte": 14, "$in": [3, 16], "$nin": [18, 9]}},
                "a_idx": {"$elemMatch": {"$in": [8, 20, 16], "$nin": [1, 19]}},
                "z_compound": {"$nin": [4, 5, 18]},
            },
        },
    ],
    /* clusterSize: 24, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$all": [19, 15]}}, {"i_compound": {"$in": [14, 19, 10]}}],
                "a_idx": {"$elemMatch": {"$nin": [5, 5, 1, 5]}},
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$limit": 155},
    ],
    /* clusterSize: 24, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"h_idx": {"$gt": 16}}, {"i_compound": {"$gt": 18}}],
                "a_compound": {"$nin": [13, 11, 10]},
                "c_compound": {"$in": [1, 2, 19, 2, 9]},
                "k_idx": {"$gt": 3},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$project": {"_id": 0, "c_noidx": 1}},
    ],
    /* clusterSize: 24, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"i_noidx": {"$exists": True}}, {"a_noidx": {"$exists": True}}, {"a_compound": {"$lte": 3}}],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 24, queryRank: 8.03 */ [
        {
            "$match": {
                "$nor": [{"i_compound": {"$lt": 1}}, {"a_compound": {"$all": [9, 6]}}, {"a_compound": {"$gte": 5}}],
                "a_noidx": {"$nin": [8, 13, 19, 11]},
            },
        },
    ],
    /* clusterSize: 24, queryRank: 7.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [16, 13]}}, {"a_compound": {"$elemMatch": {"$lte": 20}}}],
                "h_noidx": {"$lt": 1},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 24, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$exists": True, "$gte": 3}}, "h_compound": {"$exists": True}}},
        {"$project": {"_id": 0, "h_idx": 1}},
    ],
    /* clusterSize: 23, queryRank: 6.03 */ [
        {"$match": {"a_compound": {"$gt": 17}, "h_compound": {"$nin": [17, 19]}, "i_compound": {"$ne": 13}}},
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 23, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$lt": 8}}, {"a_compound": {"$all": [11, 12, 11, 20]}}],
                "a_compound": {"$in": [4, 4, 14]},
                "a_noidx": {"$gte": 18},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 23, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$elemMatch": {"$eq": 20, "$in": [8, 17, 14, 15, 2], "$lte": 19}}},
                    {"a_compound": {"$all": [8, 17, 18]}},
                ],
                "a_compound": {"$elemMatch": {"$lte": 8}},
            },
        },
        {"$limit": 32},
    ],
    /* clusterSize: 23, queryRank: 4.03 */ [{"$match": {"a_compound": {"$in": [17, 15]}, "z_compound": {"$ne": 5}}}],
    /* clusterSize: 23, queryRank: 8.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [18, 1, 8]}}, {"k_noidx": {"$eq": 5}}],
                "a_noidx": {"$exists": True},
            },
        },
        {"$project": {"_id": 0, "z_compound": 1}},
    ],
    /* clusterSize: 23, queryRank: 6.03 */ [
        {"$match": {"$or": [{"i_compound": {"$ne": 14}}, {"k_compound": {"$exists": True}}], "a_idx": {"$ne": 10}}},
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1, "d_noidx": 1}},
    ],
    /* clusterSize: 23, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$exists": False}},
                    {
                        "$or": [
                            {"a_noidx": {"$ne": 19}},
                            {"a_noidx": {"$elemMatch": {"$in": [1, 14]}}},
                            {"a_noidx": {"$eq": 16}},
                            {"a_compound": {"$all": [6, 15, 19]}},
                        ],
                    },
                ],
                "c_compound": {"$exists": True},
                "i_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 23, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$gte": 20}}, {"c_compound": {"$gte": 6}}],
                "a_compound": {"$elemMatch": {"$gt": 8}},
                "a_idx": {"$gte": 14},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 23, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"d_noidx": {"$gte": 12}},
                    {
                        "$nor": [
                            {"a_idx": {"$eq": 3}},
                            {"a_compound": {"$elemMatch": {"$nin": [20, 13, 9]}}},
                            {"a_idx": {"$all": [18, 11, 11]}},
                            {
                                "$nor": [
                                    {"a_idx": {"$elemMatch": {"$eq": 5, "$in": [5, 14, 12]}}},
                                    {"k_compound": {"$exists": False}},
                                ],
                            },
                        ],
                    },
                ],
                "c_compound": {"$nin": [8, 6, 17]},
                "c_noidx": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 23, queryRank: 6.02 */ [
        {
            "$match": {
                "$or": [{"d_idx": {"$lte": 17}}, {"a_compound": {"$all": [7, 11]}}],
                "k_noidx": {"$nin": [3, 1, 11, 7]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 252},
        {"$project": {"c_compound": 1}},
    ],
    /* clusterSize: 22, queryRank: 9.03 */ [
        {
            "$match": {
                "$and": [
                    {"c_idx": {"$exists": True}},
                    {
                        "$or": [
                            {"a_compound": {"$gt": 14}},
                            {"z_compound": {"$lte": 5}},
                            {"a_idx": {"$eq": 12}},
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                            {"a_idx": {"$lte": 16}},
                            {"d_compound": {"$exists": True}},
                        ],
                    },
                ],
                "z_noidx": {"$lt": 12},
            },
        },
        {"$sort": {"a_idx": -1, "i_idx": 1}},
    ],
    /* clusterSize: 22, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [19, 3, 9]}}, {"k_noidx": {"$in": [6, 11, 2, 6]}}],
                "a_idx": {"$lte": 8},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 200},
    ],
    /* clusterSize: 22, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$eq": 16, "$exists": True, "$in": [1, 4, 18]}}},
                    {"a_compound": {"$all": [14, 13, 8]}},
                ],
                "a_compound": {"$gt": 19},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 22, queryRank: 6.03 */ [
        {
            "$match": {
                "a_compound": {"$nin": [18, 1]},
                "a_noidx": {"$elemMatch": {"$nin": [1, 16]}},
                "c_compound": {"$exists": True},
                "z_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 22, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_idx": {"$nin": [12, 4, 9]}},
                            {"a_compound": {"$in": [9, 15]}},
                            {"a_compound": {"$all": [5, 14]}},
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$exists": True, "$in": [14, 19]}}},
                ],
                "c_idx": {"$ne": 5},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 22, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$elemMatch": {"$exists": False}}},
                    {"a_compound": {"$all": [17, 5, 16, 3]}},
                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                    {"a_compound": {"$exists": False}},
                    {"d_noidx": {"$in": [17, 11, 18]}},
                ],
                "d_noidx": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 22, queryRank: 3.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$exists": False}}, {"a_idx": {"$exists": False}}],
                "a_noidx": {"$in": [9, 8, 18, 9]},
            },
        },
    ],
    /* clusterSize: 22, queryRank: 6.02 */ [
        {
            "$match": {
                "$or": [
                    {"z_idx": {"$lte": 20}},
                    {
                        "$nor": [
                            {"a_idx": {"$exists": False}},
                            {"a_compound": {"$elemMatch": {"$nin": [11, 17, 8]}}},
                            {"$or": [{"i_idx": {"$exists": True}}, {"z_idx": {"$exists": False}}]},
                            {"a_idx": {"$lte": 18}},
                        ],
                    },
                ],
                "a_compound": {"$elemMatch": {"$lte": 2, "$nin": [8, 6, 2]}},
            },
        },
        {"$sort": {"i_idx": -1, "z_idx": -1}},
    ],
    /* clusterSize: 22, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [{"k_noidx": {"$lt": 9}}, {"a_compound": {"$all": [6, 2, 7]}}, {"h_noidx": {"$lte": 2}}],
                "a_idx": {"$nin": [13, 7]},
                "c_noidx": {"$nin": [11, 18, 7]},
            },
        },
    ],
    /* clusterSize: 22, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"z_idx": {"$in": [7, 11, 12, 18, 5]}}, {"a_compound": {"$all": [4, 3, 1]}}],
                "i_compound": {"$nin": [20, 8, 1]},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$project": {"d_noidx": 1, "z_compound": 1}},
    ],
    /* clusterSize: 22, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [2, 2, 5, 14]}},
                    {"a_noidx": {"$in": [20, 9, 16]}},
                    {"a_idx": {"$ne": 4}},
                ],
                "d_compound": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 234},
    ],
    /* clusterSize: 22, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"d_compound": {"$in": [2, 4, 8, 18, 8]}}, {"d_idx": {"$in": [14, 4, 5]}}]},
                    {"c_idx": {"$in": [7, 1, 10, 11]}},
                ],
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 22, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_compound": {"$all": [16, 11, 7]}},
                            {"a_compound": {"$in": [1, 12, 2, 13]}},
                            {"a_idx": {"$all": [4, 6, 2, 13, 7, 19]}},
                            {"i_compound": {"$ne": 7}},
                        ],
                    },
                    {"d_idx": {"$exists": True}},
                ],
                "a_compound": {"$gt": 6},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"a_compound": 1, "a_idx": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 21, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {"a_compound": {"$exists": False}},
                            {"a_compound": {"$lt": 10}},
                            {"h_compound": {"$exists": True}},
                            {"h_idx": {"$gt": 13}},
                        ],
                    },
                    {"a_idx": {"$in": [12, 15]}},
                ],
                "k_compound": {"$lte": 13},
            },
        },
        {"$project": {"a_compound": 1, "h_compound": 1}},
    ],
    /* clusterSize: 21, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$lte": 4}},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$gt": 11}}},
                    {
                        "$or": [
                            {"a_idx": {"$all": [8, 17, 14]}},
                            {"c_compound": {"$exists": False}},
                            {"a_idx": {"$exists": True}},
                            {"a_idx": {"$in": [5, 2, 7]}},
                        ],
                    },
                ],
                "i_compound": {"$in": [15, 12, 9, 2]},
            },
        },
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 21, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$nin": [6, 4]}}, "d_compound": {"$gt": 2}}},
        {"$sort": {"c_idx": -1}},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 21, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [{"d_idx": {"$lte": 13}}, {"i_compound": {"$nin": [17, 19, 1]}}, {"d_compound": {"$lte": 6}}],
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"d_idx": 1}},
    ],
    /* clusterSize: 21, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$all": [9, 18, 3]}}, {"a_idx": {"$eq": 9}}]},
                    {"z_noidx": {"$nin": [17, 17]}},
                    {"z_idx": {"$gte": 6}},
                ],
                "k_noidx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1, "z_idx": -1}},
        {"$limit": 18},
    ],
    /* clusterSize: 21, queryRank: 5.03 */ [
        {"$match": {"$or": [{"c_idx": {"$exists": True}}, {"k_compound": {"$in": [1, 11]}}], "d_compound": {"$eq": 7}}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 21, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$in": [6, 14, 3, 2]}, "k_compound": {"$exists": True}}},
    ],
    /* clusterSize: 21, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [10, 1]}},
                    {"a_noidx": {"$in": [12, 10]}},
                    {"a_compound": {"$all": [20, 10]}},
                ],
                "c_idx": {"$nin": [5, 19, 16]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 21, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$nin": [7, 11]}}},
                    {"a_compound": {"$elemMatch": {"$eq": 13, "$exists": True, "$gt": 3, "$gte": 17}}},
                    {"a_compound": {"$exists": False}},
                ],
                "i_idx": {"$lte": 10},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 21, queryRank: 10.03 */ [
        {"$match": {"$or": [{"a_compound": {"$all": [10, 8]}}, {"k_compound": {"$gt": 12}}], "i_noidx": {"$ne": 5}}},
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 21, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [17, 14]}},
                    {"c_compound": {"$nin": [4, 5, 13]}},
                    {"c_idx": {"$nin": [19, 6]}},
                ],
                "a_compound": {"$all": [7, 2, 7]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 131},
        {"$project": {"_id": 0, "i_noidx": 1}},
    ],
    /* clusterSize: 21, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"i_compound": {"$in": [7, 9]}},
                    {"a_compound": {"$all": [7, 14, 16, 9]}},
                    {"d_compound": {"$nin": [15, 3, 17]}},
                ],
                "a_noidx": {"$elemMatch": {"$exists": True}},
            },
        },
    ],
    /* clusterSize: 21, queryRank: 4.03 */ [
        {
            "$match": {
                "$nor": [{"i_compound": {"$lt": 10}}, {"i_compound": {"$in": [11, 5, 6]}}],
                "a_idx": {"$exists": True},
                "a_noidx": {"$exists": True},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 21, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$in": [13, 3, 10]}, "a_idx": {"$lt": 9}, "d_compound": {"$in": [7, 10]}}},
    ],
    /* clusterSize: 21, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [{"c_compound": {"$nin": [7, 14]}}, {"c_compound": {"$lt": 2}}, {"a_idx": {"$all": [1, 9]}}],
                "k_compound": {"$gte": 8},
            },
        },
        {"$skip": 80},
    ],
    /* clusterSize: 20, queryRank: 16.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_idx": {"$exists": False}},
                    {"a_compound": {"$gt": 17}},
                    {
                        "$nor": [
                            {
                                "$and": [
                                    {"c_compound": {"$exists": True}},
                                    {"a_compound": {"$all": [15, 14, 11]}},
                                    {"a_idx": {"$nin": [18, 13]}},
                                    {"a_idx": {"$nin": [14, 9]}},
                                ],
                            },
                            {"a_idx": {"$lt": 3}},
                            {"d_compound": {"$nin": [12, 19, 16, 19]}},
                        ],
                    },
                ],
                "a_compound": {"$eq": 3},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 20, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$in": [11, 11, 12]}},
                    {"a_idx": {"$in": [9, 12]}},
                    {"a_compound": {"$all": [2, 14, 16, 10]}},
                    {"k_compound": {"$nin": [1, 20]}},
                ],
                "a_noidx": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 20, queryRank: 3.02 */ [
        {"$match": {"a_compound": {"$in": [15, 5, 1, 2]}}},
        {"$sort": {"d_idx": -1}},
        {"$limit": 243},
        {"$project": {"a_idx": 1, "h_noidx": 1}},
    ],
    /* clusterSize: 20, queryRank: 3.03 */ [{"$match": {"a_compound": {"$nin": [8, 2]}, "a_idx": {"$exists": True}}}],
    /* clusterSize: 20, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"c_compound": {"$nin": [16, 7, 2]}}, {"i_idx": {"$lte": 9}}],
                "a_compound": {"$elemMatch": {"$nin": [6, 17]}},
            },
        },
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 20, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$in": [8, 15]}},
                    {"a_compound": {"$elemMatch": {"$nin": [13, 16]}}},
                    {"h_idx": {"$nin": [9, 1]}},
                ],
                "a_noidx": {"$elemMatch": {"$in": [16, 8, 4]}},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 20, queryRank: 4.03 */ [
        {"$match": {"a_noidx": {"$all": [1, 8]}, "h_compound": {"$exists": True}, "i_compound": {"$exists": True}}},
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1, "z_idx": 1}},
    ],
    /* clusterSize: 20, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$lte": 14}},
                    {"a_compound": {"$lte": 9}},
                    {
                        "$nor": [
                            {
                                "$nor": [
                                    {"a_compound": {"$elemMatch": {"$in": [10, 19, 17], "$nin": [8, 8, 2, 5]}}},
                                    {
                                        "$nor": [
                                            {"i_compound": {"$eq": 4}},
                                            {"k_noidx": {"$exists": False}},
                                            {"a_noidx": {"$exists": False}},
                                            {"c_noidx": {"$lt": 5}},
                                            {"a_noidx": {"$exists": False}},
                                        ],
                                    },
                                ],
                            },
                            {"k_noidx": {"$nin": [5, 8, 11]}},
                        ],
                    },
                ],
                "a_noidx": {"$in": [13, 13, 15, 2]},
                "h_noidx": {"$nin": [14, 10, 3]},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 20, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"z_compound": {"$lt": 14}},
                    {
                        "$and": [
                            {"a_noidx": {"$all": [13, 5]}},
                            {
                                "$or": [
                                    {"a_compound": {"$ne": 2}},
                                    {"a_compound": {"$gt": 11}},
                                    {"a_compound": {"$elemMatch": {"$ne": 19}}},
                                ],
                            },
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$gt": 4}}},
                ],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": 1, "h_idx": 1}},
        {"$limit": 16},
        {"$project": {"a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 19, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"a_noidx": {"$all": [11, 2]}}, {"h_idx": {"$lte": 17}}],
                "a_compound": {"$elemMatch": {"$lt": 16}},
                "a_idx": {"$elemMatch": {"$gt": 18}},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$skip": 22},
    ],
    /* clusterSize: 19, queryRank: 13.03 */ [
        {
            "$match": {
                "$and": [{"c_compound": {"$lte": 7}}, {"a_idx": {"$ne": 5}}, {"a_noidx": {"$elemMatch": {"$lt": 20}}}],
                "$or": [
                    {"c_compound": {"$nin": [8, 9]}},
                    {"a_compound": {"$gte": 18}},
                    {"z_compound": {"$in": [10, 15]}},
                ],
                "a_idx": {"$lt": 8},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 19, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$elemMatch": {"$lte": 1, "$nin": [11, 8, 9, 14, 8]}}},
                    {"a_compound": {"$exists": True}},
                    {"a_noidx": {"$in": [2, 17, 16]}},
                    {"k_compound": {"$in": [20, 16, 5]}},
                ],
                "a_idx": {"$lt": 2},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 19, queryRank: 5.02 */ [
        {
            "$match": {
                "$nor": [{"i_compound": {"$eq": 8}}, {"z_idx": {"$gte": 7}}, {"k_idx": {"$lt": 20}}],
                "a_noidx": {"$elemMatch": {"$gte": 19}},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 19, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$ne": 14}}, {"a_compound": {"$exists": True}}, {"c_noidx": {"$in": [1, 3]}}],
            },
        },
        {"$sort": {"c_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 19, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$nor": [
                            {"a_compound": {"$elemMatch": {"$exists": False, "$in": [10, 11]}}},
                            {"a_idx": {"$lte": 20}},
                            {"a_idx": {"$all": [12, 3]}},
                        ],
                    },
                    {"k_compound": {"$ne": 20}},
                    {"h_noidx": {"$lt": 9}},
                ],
                "i_compound": {"$ne": 8},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 19, queryRank: 4.04 */ [
        {"$match": {"a_compound": {"$in": [20, 1]}, "z_compound": {"$in": [1, 13, 14]}}},
    ],
    /* clusterSize: 19, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_idx": {"$lt": 16}},
                    {"a_compound": {"$ne": 8}},
                    {"$or": [{"a_compound": {"$lte": 18}}, {"a_idx": {"$all": [4, 20]}}]},
                ],
                "a_compound": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$sort": {"a_idx": 1, "c_idx": 1, "h_idx": -1}},
    ],
    /* clusterSize: 19, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$eq": 14}},
                    {"d_compound": {"$exists": False}},
                ],
                "a_idx": {"$exists": True},
                "i_idx": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$skip": 67},
    ],
    /* clusterSize: 19, queryRank: 14.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [10, 9, 5, 4]}},
                    {"i_noidx": {"$exists": False}},
                    {"d_noidx": {"$in": [11, 8, 8]}},
                ],
                "k_idx": {"$nin": [11, 4]},
                "z_compound": {"$nin": [13, 5, 7, 20]},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$limit": 48},
    ],
    /* clusterSize: 18, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_compound": {"$elemMatch": {"$exists": True}}},
                            {"i_noidx": {"$ne": 1}},
                            {"a_noidx": {"$gte": 4}},
                            {
                                "$and": [
                                    {"$and": [{"z_noidx": {"$exists": False}}, {"h_compound": {"$nin": [6, 3]}}]},
                                    {"c_compound": {"$eq": 1}},
                                    {"a_idx": {"$all": [12, 5, 20]}},
                                ],
                            },
                            {"a_idx": {"$all": [14, 19]}},
                        ],
                    },
                    {"c_idx": {"$exists": True}},
                ],
                "a_idx": {"$lte": 16},
                "a_noidx": {"$elemMatch": {"$nin": [13, 8]}},
            },
        },
        {"$limit": 37},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 18, queryRank: 5.03 */ [
        {"$match": {"$or": [{"d_compound": {"$exists": True}}, {"d_compound": {"$lte": 14}}]}},
        {"$sort": {"z_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 18, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$all": [7, 6, 19]}},
                    {"a_compound": {"$elemMatch": {"$eq": 8, "$gt": 18}}},
                    {"d_compound": {"$lt": 11}},
                ],
                "a_compound": {"$lte": 9},
                "a_idx": {"$elemMatch": {"$eq": 14, "$gt": 11}},
            },
        },
    ],
    /* clusterSize: 18, queryRank: 7.03 */ [
        {"$match": {"$nor": [{"a_compound": {"$lt": 1}}, {"a_compound": {"$all": [19, 8]}}], "h_idx": {"$gte": 19}}},
    ],
    /* clusterSize: 18, queryRank: 10.02 */ [
        {
            "$match": {
                "$nor": [{"a_noidx": {"$nin": [5, 3, 7]}}, {"h_idx": {"$eq": 17}}],
                "$or": [
                    {"$or": [{"a_compound": {"$all": [4, 17]}}, {"a_idx": {"$gt": 1}}]},
                    {"$and": [{"a_idx": {"$eq": 9}}, {"a_idx": {"$all": [14, 5]}}]},
                    {"c_idx": {"$exists": False}},
                ],
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_idx": 1, "c_noidx": 1}},
    ],
    /* clusterSize: 18, queryRank: 5.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$in": [14, 20]}},
                    {
                        "$and": [
                            {"c_compound": {"$exists": True}},
                            {"a_compound": {"$elemMatch": {"$nin": [7, 13, 14, 18]}}},
                            {"z_noidx": {"$in": [2, 5, 7]}},
                        ],
                    },
                ],
                "d_compound": {"$nin": [10, 8]},
            },
        },
        {"$sort": {"h_idx": 1}},
        {"$project": {"h_idx": 1}},
    ],
    /* clusterSize: 18, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"c_idx": {"$in": [13, 3]}},
                    {"c_compound": {"$in": [12, 18]}},
                    {"a_compound": {"$all": [10, 12, 1]}},
                ],
                "a_idx": {"$lt": 19},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$limit": 234},
    ],
    /* clusterSize: 18, queryRank: 5.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$in": [10, 14, 1]}}},
                    {"z_idx": {"$lte": 4}},
                    {"a_idx": {"$all": [11, 13, 17]}},
                ],
                "h_idx": {"$nin": [2, 18]},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 18, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$all": [20, 17, 2]}}, {"i_idx": {"$gte": 9}}],
                "i_compound": {"$in": [14, 1, 11]},
            },
        },
    ],
    /* clusterSize: 18, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$ne": 15}}, "z_compound": {"$in": [4, 1]}}},
        {"$project": {"a_compound": 1, "h_compound": 1, "i_idx": 1}},
    ],
    /* clusterSize: 18, queryRank: 4.03 */ [
        {
            "$match": {
                "$and": [
                    {"i_noidx": {"$exists": True}},
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$nin": [4, 1, 17]}},
                ],
            },
        },
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 18, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"i_compound": {"$nin": [17, 11]}},
                    {"a_idx": {"$all": [4, 17]}},
                    {"a_compound": {"$lte": 11}},
                    {"k_idx": {"$lte": 5}},
                ],
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 17, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"a_noidx": {"$lte": 17}}, {"a_compound": {"$all": [13, 16, 18]}}]},
                    {"a_compound": {"$exists": True}},
                    {"$and": [{"d_idx": {"$in": [9, 16, 3, 3]}}, {"c_noidx": {"$exists": False}}]},
                ],
                "d_noidx": {"$lte": 6},
            },
        },
        {"$sort": {"i_idx": 1}},
    ],
    /* clusterSize: 17, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [{"k_compound": {"$in": [4, 4]}}, {"k_compound": {"$exists": True}}],
                "$or": [
                    {"a_compound": {"$in": [3, 16, 9]}},
                    {"d_compound": {"$in": [4, 15, 9]}},
                    {"h_idx": {"$exists": False}},
                ],
            },
        },
    ],
    /* clusterSize: 17, queryRank: 9.02 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"c_compound": {"$exists": False}}, {"a_compound": {"$in": [16, 19]}}]},
                    {"a_compound": {"$nin": [11, 12, 7]}},
                    {"i_idx": {"$exists": True}},
                    {"a_idx": {"$lte": 11}},
                    {"h_noidx": {"$gte": 8}},
                ],
                "c_compound": {"$nin": [17, 5, 4]},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"k_compound": 1}},
    ],
    /* clusterSize: 17, queryRank: 7.02 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$all": [9, 19]}}, {"d_idx": {"$lte": 1}}, {"a_compound": {"$exists": True}}],
                "c_noidx": {"$lt": 8},
                "k_noidx": {"$nin": [11, 17, 15]},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 244},
        {"$project": {"a_compound": 1, "a_idx": 1}},
    ],
    /* clusterSize: 17, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_idx": {"$elemMatch": {"$exists": True}}},
                    {"a_compound": {"$elemMatch": {"$gte": 9, "$nin": [9, 18, 3]}}},
                    {"k_idx": {"$gte": 1}},
                ],
                "i_idx": {"$ne": 1},
            },
        },
    ],
    /* clusterSize: 17, queryRank: 10.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$in": [12, 20, 14]}}},
                            {"a_compound": {"$all": [13, 20]}},
                            {"a_compound": {"$all": [5, 6]}},
                        ],
                    },
                    {"a_compound": {"$elemMatch": {"$lt": 5}}},
                ],
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$limit": 233},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 17, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$nin": [7, 11, 11]}, "i_idx": {"$exists": True}}},
        {"$sort": {"a_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 17, queryRank: 13.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"a_idx": {"$eq": 7}},
                            {"a_idx": {"$all": [12, 4, 19, 20]}},
                            {"c_compound": {"$lte": 20}},
                        ],
                    },
                    {"i_compound": {"$nin": [14, 16]}},
                    {"c_idx": {"$nin": [5, 19]}},
                ],
                "d_compound": {"$ne": 7},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 17, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"d_noidx": {"$gte": 14}}, {"a_compound": {"$in": [19, 4]}}],
                "a_compound": {"$nin": [15, 4]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"k_idx": 1}},
    ],
    /* clusterSize: 17, queryRank: 14.03 */ [
        {
            "$match": {
                "$and": [{"a_noidx": {"$elemMatch": {"$gt": 14}}}, {"d_compound": {"$nin": [13, 7, 1]}}],
                "$nor": [
                    {"a_compound": {"$elemMatch": {"$in": [13, 3], "$nin": [1, 1]}}},
                    {"a_compound": {"$all": [8, 1, 20, 3]}},
                ],
                "a_idx": {"$lt": 9},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 17, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"d_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                    {"a_idx": {"$exists": False}},
                    {"i_compound": {"$exists": False}},
                ],
                "a_idx": {"$elemMatch": {"$lte": 9, "$nin": [11, 10]}},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 17, queryRank: 4.03 */ [
        {"$match": {"$or": [{"a_idx": {"$gt": 4}}, {"a_compound": {"$ne": 3}}], "d_compound": {"$exists": True}}},
        {"$project": {"h_idx": 1}},
    ],
    /* clusterSize: 17, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$lte": 12}}, {"a_idx": {"$all": [11, 5, 2, 7]}}],
                "k_noidx": {"$in": [1, 9]},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$project": {"_id": 0, "i_idx": 1, "z_noidx": 1}},
    ],
    /* clusterSize: 17, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$in": [2, 3]}}, {"a_compound": {"$exists": True}}],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 17, queryRank: 6.03 */ [
        {
            "$match": {
                "a_compound": {"$elemMatch": {"$exists": True}},
                "a_idx": {"$gt": 20},
                "c_noidx": {"$ne": 2},
                "k_compound": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"_id": 0, "k_noidx": 1}},
    ],
    /* clusterSize: 16, queryRank: 9.02 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"a_idx": {"$gte": 15}}, {"a_compound": {"$nin": [4, 14, 12, 20]}}]},
                    {"a_compound": {"$exists": False}},
                ],
                "d_compound": {"$lt": 15},
            },
        },
        {"$sort": {"h_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1, "d_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 16, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$lte": 9}},
                    {"a_idx": {"$elemMatch": {"$lt": 14}}},
                    {"k_compound": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$exists": True, "$ne": 4}}},
                ],
                "k_compound": {"$gte": 8},
            },
        },
    ],
    /* clusterSize: 16, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [12, 8]}}, {"d_noidx": {"$in": [12, 2]}}],
                "a_idx": {"$elemMatch": {"$gte": 16}},
                "a_noidx": {"$lt": 15},
            },
        },
        {"$skip": 28},
    ],
    /* clusterSize: 16, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$nin": [1, 19]}, "a_idx": {"$elemMatch": {"$exists": True}}}},
    ],
    /* clusterSize: 16, queryRank: 13.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_compound": {"$nin": [7, 7, 9]}},
                    {"c_idx": {"$nin": [8, 7, 2]}},
                    {"k_compound": {"$eq": 20}},
                    {"a_idx": {"$all": [10, 8]}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$limit": 82},
        {"$project": {"i_compound": 1, "i_idx": 1}},
    ],
    /* clusterSize: 16, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$in": [7, 12, 17]}},
                    {"a_idx": {"$nin": [14, 3]}},
                    {"a_compound": {"$all": [1, 7]}},
                ],
                "i_noidx": {"$nin": [10, 2]},
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "c_noidx": 1, "h_compound": 1, "k_noidx": 1}},
    ],
    /* clusterSize: 16, queryRank: 14.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$gt": 1}},
                    {"a_compound": {"$all": [14, 9, 5, 20]}},
                    {"d_noidx": {"$exists": False}},
                ],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 16, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"i_compound": {"$exists": False}}, {"a_idx": {"$all": [11, 4]}}]},
                    {"a_compound": {"$exists": True}},
                    {"a_compound": {"$lte": 18}},
                ],
                "a_idx": {"$lt": 6},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 16, queryRank: 13.02 */ [
        {
            "$match": {
                "$and": [{"z_compound": {"$lt": 12}}, {"a_compound": {"$lte": 11}}],
                "$or": [
                    {"a_idx": {"$nin": [19, 1, 16, 19]}},
                    {
                        "$or": [
                            {
                                "$or": [
                                    {"a_idx": {"$elemMatch": {"$gt": 16, "$in": [1, 1], "$nin": [17, 16, 15]}}},
                                    {"c_compound": {"$in": [9, 8, 18]}},
                                    {"a_compound": {"$all": [5, 15, 4]}},
                                    {"a_compound": {"$gt": 18}},
                                ],
                            },
                            {"a_idx": {"$nin": [2, 2, 17]}},
                        ],
                    },
                ],
            },
        },
        {"$limit": 89},
        {"$project": {"a_noidx": 1, "d_noidx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 16, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$gt": 20}, "i_idx": {"$exists": True}}},
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 15, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$gte": 8}}, "i_compound": {"$ne": 3}}},
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 15, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"z_idx": {"$nin": [15, 17]}},
                    {"a_idx": {"$elemMatch": {"$lt": 8, "$nin": [19, 17, 5]}}},
                    {"a_idx": {"$in": [9, 12]}},
                ],
                "k_compound": {"$nin": [20, 2]},
            },
        },
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 15, queryRank: 9.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$nin": [17, 1, 20, 1]}},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$nin": [5, 8]}}},
                            {"c_idx": {"$exists": False}},
                            {"i_idx": {"$exists": False}},
                            {
                                "$nor": [
                                    {"$or": [{"h_idx": {"$exists": False}}, {"h_compound": {"$gt": 10}}]},
                                    {"k_noidx": {"$nin": [4, 2, 1]}},
                                    {"c_idx": {"$in": [13, 18]}},
                                ],
                            },
                        ],
                    },
                ],
                "i_idx": {"$ne": 9},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 15, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "c_idx": {"$nin": [7, 15]}, "d_idx": {"$nin": [2, 4, 7, 19]}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 15, queryRank: 11.03 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$lt": 12}}, {"a_idx": {"$all": [7, 3, 5]}}],
                "a_compound": {"$nin": [5, 20, 1]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 15, queryRank: 5.02 */ [
        {"$match": {"i_compound": {"$nin": [19, 19]}, "k_compound": {"$exists": True}}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 228},
        {"$project": {"a_idx": 1, "c_idx": 1}},
    ],
    /* clusterSize: 15, queryRank: 3.03 */ [{"$match": {"a_compound": {"$in": [1, 8]}}}, {"$sort": {"i_idx": -1}}],
    /* clusterSize: 15, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [1, 17, 6, 16]}}, {"a_idx": {"$in": [9, 10, 16]}}],
                "a_noidx": {"$elemMatch": {"$eq": 4}},
                "z_idx": {"$gt": 1},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"_id": 0, "c_compound": 1}},
    ],
    /* clusterSize: 15, queryRank: 9.02 */ [
        {
            "$match": {
                "$or": [{"z_compound": {"$in": [20, 8, 8]}}, {"a_compound": {"$exists": True}}],
                "a_compound": {"$gt": 2},
                "h_compound": {"$exists": True},
                "k_idx": {"$nin": [14, 11]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"h_compound": 1}},
    ],
    /* clusterSize: 14, queryRank: 13.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {"a_idx": {"$all": [2, 9]}},
                            {"k_compound": {"$nin": [13, 20]}},
                            {"a_idx": {"$all": [4, 18]}},
                            {"z_idx": {"$nin": [13, 20]}},
                        ],
                    },
                    {"z_compound": {"$nin": [12, 17]}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 14, queryRank: 3.02 */ [
        {
            "$match": {
                "$or": [
                    {"k_idx": {"$gt": 17}},
                    {"$and": [{"a_noidx": {"$in": [11, 8, 9]}}, {"h_compound": {"$nin": [20, 18, 1, 4]}}]},
                ],
                "i_compound": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": 1, "i_idx": -1}},
        {"$limit": 46},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 14, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [
                    {"a_compound": {"$exists": True}},
                    {"$or": [{"a_compound": {"$lt": 6}}, {"a_noidx": {"$all": [7, 14, 6]}}]},
                ],
                "$or": [{"a_compound": {"$lt": 4}}, {"a_compound": {"$elemMatch": {"$eq": 8}}}],
            },
        },
        {"$sort": {"a_idx": -1, "d_idx": 1}},
        {"$limit": 158},
        {"$project": {"_id": 0, "a_idx": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 14, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [
                    {"c_compound": {"$in": [1, 16]}},
                    {
                        "$and": [
                            {"$or": [{"h_compound": {"$exists": True}}, {"d_noidx": {"$lt": 8}}]},
                            {"k_noidx": {"$exists": False}},
                            {"a_compound": {"$all": [20, 9]}},
                            {"c_idx": {"$ne": 10}},
                            {"a_noidx": {"$exists": True}},
                        ],
                    },
                ],
                "d_idx": {"$nin": [3, 16]},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"d_noidx": 1}},
    ],
    /* clusterSize: 14, queryRank: 10.03 */ [
        {"$match": {"$nor": [{"a_compound": {"$all": [15, 17, 4]}}, {"c_idx": {"$in": [4, 2]}}], "h_idx": {"$gt": 10}}},
        {"$sort": {"h_idx": 1}},
        {"$limit": 137},
    ],
    /* clusterSize: 14, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$elemMatch": {"$nin": [4, 20, 9, 5, 5]}}}, {"i_compound": {"$gte": 8}}],
                "a_noidx": {"$elemMatch": {"$lte": 16}},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 14, queryRank: 4.03 */ [
        {
            "$match": {
                "$or": [{"a_idx": {"$elemMatch": {"$eq": 19}}}, {"c_compound": {"$lte": 8}}],
                "a_compound": {"$gte": 10},
            },
        },
        {"$project": {"d_compound": 1}},
    ],
    /* clusterSize: 14, queryRank: 11.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [12, 6, 20, 14]}},
                    {"i_idx": {"$exists": False}},
                    {"a_noidx": {"$ne": 4}},
                ],
                "i_idx": {"$nin": [8, 9, 16]},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 153},
        {"$project": {"_id": 0, "a_compound": 1, "i_idx": 1}},
    ],
    /* clusterSize: 14, queryRank: 4.03 */ [
        {"$match": {"$nor": [{"i_idx": {"$in": [18, 18]}}, {"a_compound": {"$eq": 12}}], "d_noidx": {"$lte": 12}}},
        {"$sort": {"a_idx": -1}},
        {"$project": {"c_idx": 1, "d_idx": 1}},
    ],
    /* clusterSize: 13, queryRank: 4.03 */ [
        {"$match": {"a_idx": {"$nin": [11, 5, 17]}, "z_compound": {"$ne": 6}}},
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 13, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$exists": True}, "c_compound": {"$in": [17, 1, 1, 1, 20]}}},
        {"$sort": {"d_idx": 1, "i_idx": 1}},
        {"$limit": 166},
    ],
    /* clusterSize: 13, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$in": [10, 17]}}, {"a_idx": {"$all": [14, 17, 7, 11]}}]},
                    {"k_idx": {"$exists": True}},
                ],
                "d_idx": {"$gte": 9},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$limit": 171},
        {"$skip": 6},
    ],
    /* clusterSize: 13, queryRank: 5.03 */ [
        {"$match": {"i_compound": {"$gte": 2}, "i_idx": {"$nin": [6, 14]}, "z_idx": {"$exists": True}}},
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 13, queryRank: 8.02 */ [
        {
            "$match": {
                "$or": [{"a_compound": {"$nin": [3, 10, 2, 6, 16]}}, {"a_idx": {"$all": [15, 7]}}],
                "a_idx": {"$exists": True},
                "k_idx": {"$gte": 13},
            },
        },
        {"$sort": {"c_idx": -1, "i_idx": -1}},
        {"$limit": 152},
        {"$skip": 15},
    ],
    /* clusterSize: 13, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_idx": {"$elemMatch": {"$exists": True, "$in": [20, 13, 19, 17], "$ne": 14}}},
                            {"d_noidx": {"$lte": 1}},
                            {"h_idx": {"$gte": 7}},
                            {"z_compound": {"$exists": True}},
                        ],
                    },
                    {"i_compound": {"$exists": True}},
                ],
                "i_idx": {"$gt": 11},
                "k_idx": {"$nin": [13, 19]},
            },
        },
    ],
    /* clusterSize: 13, queryRank: 8.03 */ [
        {
            "$match": {
                "$and": [
                    {"$nor": [{"a_compound": {"$exists": False}}, {"z_compound": {"$eq": 15}}]},
                    {"a_idx": {"$lte": 10}},
                    {"$and": [{"i_idx": {"$lte": 10}}, {"d_idx": {"$eq": 3}}]},
                ],
                "a_idx": {"$elemMatch": {"$exists": True}},
            },
        },
        {"$project": {"a_idx": 1, "h_idx": 1}},
    ],
    /* clusterSize: 13, queryRank: 12.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$gt": 16}},
                    {"$and": [{"a_compound": {"$ne": 9}}, {"i_noidx": {"$lte": 1}}]},
                    {"a_compound": {"$exists": True}},
                ],
                "a_noidx": {"$in": [5, 10]},
                "k_compound": {"$lte": 9},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$limit": 160},
        {"$project": {"i_compound": 1}},
    ],
    /* clusterSize: 13, queryRank: 11.02 */ [
        {
            "$match": {
                "$or": [{"c_compound": {"$nin": [14, 12, 5]}}, {"a_compound": {"$gt": 17}}],
                "a_idx": {"$nin": [6, 9]},
                "h_compound": {"$gte": 11},
                "z_compound": {"$ne": 9},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "h_compound": 1}},
    ],
    /* clusterSize: 12, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$gte": 18}}, "c_compound": {"$lte": 5}, "d_idx": {"$lt": 17}}},
        {"$sort": {"d_idx": -1, "z_idx": 1}},
        {"$limit": 127},
    ],
    /* clusterSize: 12, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {"z_compound": {"$lte": 3}},
                    {"$or": [{"a_compound": {"$all": [6, 10, 18, 18]}}, {"z_idx": {"$lt": 6}}]},
                ],
                "a_idx": {"$gt": 12},
            },
        },
    ],
    /* clusterSize: 12, queryRank: 6.03 */ [
        {"$match": {"$nor": [{"c_compound": {"$gt": 12}}, {"a_compound": {"$exists": False}}], "a_idx": {"$lte": 3}}},
        {"$sort": {"d_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 12, queryRank: 9.03 */ [
        {
            "$match": {
                "$nor": [
                    {"h_noidx": {"$nin": [9, 4, 4]}},
                    {"a_compound": {"$all": [5, 17, 6]}},
                    {"a_noidx": {"$elemMatch": {"$gt": 10}}},
                ],
                "i_noidx": {"$gte": 6},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 12, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"$or": [{"i_compound": {"$ne": 1}}, {"k_compound": {"$gt": 3}}, {"d_idx": {"$exists": True}}]},
                    {"a_idx": {"$in": [15, 10, 5, 17]}},
                ],
                "c_idx": {"$nin": [11, 3, 14]},
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 12, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [
                    {"h_compound": {"$nin": [8, 3]}},
                    {
                        "$and": [
                            {"a_noidx": {"$elemMatch": {"$nin": [7, 1, 12]}}},
                            {"i_noidx": {"$nin": [3, 6]}},
                            {"a_idx": {"$elemMatch": {"$exists": True}}},
                            {"i_compound": {"$ne": 12}},
                        ],
                    },
                    {"h_idx": {"$nin": [20, 15]}},
                ],
                "a_noidx": {"$elemMatch": {"$gt": 18}},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 12, queryRank: 3.03 */ [{"$match": {"a_compound": {"$gt": 2}}}, {"$sort": {"a_idx": -1}}],
    /* clusterSize: 12, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_idx": {"$all": [8, 2]}}, {"a_idx": {"$elemMatch": {"$eq": 8}}}]},
                    {"k_idx": {"$nin": [18, 14]}},
                    {"i_idx": {"$in": [11, 15]}},
                ],
                "i_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 12, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$all": [16, 10]}},
                    {"h_idx": {"$exists": False}},
                    {"a_compound": {"$elemMatch": {"$eq": 19, "$lte": 4}}},
                ],
                "a_idx": {"$exists": True},
                "k_compound": {"$gte": 18},
            },
        },
        {"$sort": {"d_idx": -1, "i_idx": 1, "z_idx": -1}},
    ],
    /* clusterSize: 12, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"d_noidx": {"$gt": 6}}, {"a_compound": {"$all": [14, 20, 7, 8]}}],
                "h_idx": {"$nin": [20, 1, 18, 15]},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 12, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"a_idx": {"$gt": 13}},
                    {"a_compound": {"$elemMatch": {"$in": [14, 10]}}},
                ],
                "i_idx": {"$ne": 17},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 12, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$lte": 20}}, "a_idx": {"$exists": True}}},
    ],
    /* clusterSize: 12, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$exists": False, "$nin": [15, 15]}}},
                    {"z_compound": {"$exists": True}},
                    {"i_compound": {"$gte": 4}},
                ],
                "d_noidx": {"$nin": [17, 8, 17, 14]},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 12, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [{"z_compound": {"$nin": [3, 20]}}, {"a_compound": {"$nin": [13, 17]}}],
                "a_noidx": {"$in": [14, 2]},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 11, queryRank: 8.03 */ [
        {
            "$match": {
                "$or": [{"k_idx": {"$eq": 14}}, {"d_compound": {"$in": [13, 14]}}, {"c_compound": {"$ne": 19}}],
                "c_compound": {"$in": [16, 1, 5]},
                "z_noidx": {"$exists": True},
            },
        },
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 11, queryRank: 4.03 */ [{"$match": {"c_compound": {"$gte": 1}, "d_compound": {"$exists": True}}}],
    /* clusterSize: 11, queryRank: 6.02 */ [
        {
            "$match": {
                "$and": [{"c_noidx": {"$exists": True}}, {"a_compound": {"$gt": 12}}],
                "$or": [{"i_compound": {"$eq": 9}}, {"k_idx": {"$gte": 8}}],
                "a_noidx": {"$lt": 1},
            },
        },
        {"$sort": {"i_idx": 1}},
        {"$project": {"a_compound": 1, "a_noidx": 1, "i_idx": 1, "k_idx": 1}},
    ],
    /* clusterSize: 11, queryRank: 8.02 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$or": [
                            {"d_idx": {"$nin": [5, 2]}},
                            {"a_compound": {"$elemMatch": {"$exists": True, "$gte": 20, "$nin": [17, 16, 14]}}},
                        ],
                    },
                    {"c_compound": {"$in": [18, 6, 20]}},
                    {"a_idx": {"$elemMatch": {"$ne": 4}}},
                ],
                "z_compound": {"$gte": 4},
            },
        },
        {"$sort": {"k_idx": -1, "z_idx": 1}},
        {"$project": {"a_compound": 1, "k_compound": 1}},
    ],
    /* clusterSize: 11, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [
                    {"d_noidx": {"$gt": 6}},
                    {"k_idx": {"$exists": True}},
                    {"d_idx": {"$lt": 10}},
                    {
                        "$or": [
                            {"a_idx": {"$nin": [5, 12]}},
                            {"a_idx": {"$exists": False}},
                            {"i_compound": {"$exists": False}},
                        ],
                    },
                ],
                "a_noidx": {"$nin": [17, 3, 6]},
                "c_idx": {"$nin": [8, 18]},
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 10, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$nor": [
                            {"a_compound": {"$all": [2, 14]}},
                            {"a_idx": {"$elemMatch": {"$gte": 15}}},
                            {"a_compound": {"$ne": 19}},
                        ],
                    },
                    {"a_idx": {"$nin": [6, 7, 7]}},
                ],
                "i_compound": {"$nin": [20, 13, 18]},
            },
        },
    ],
    /* clusterSize: 10, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [{"a_idx": {"$all": [18, 1, 19]}}, {"c_compound": {"$in": [5, 19]}}],
                "a_idx": {"$ne": 20},
                "k_compound": {"$eq": 3},
            },
        },
    ],
    /* clusterSize: 10, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [
                    {"$and": [{"a_compound": {"$lte": 7}}, {"a_compound": {"$gte": 8}}, {"k_noidx": {"$nin": [3, 7]}}]},
                    {"c_idx": {"$in": [11, 9, 1, 11, 15]}},
                ],
                "z_noidx": {"$lte": 17},
            },
        },
        {"$limit": 255},
    ],
    /* clusterSize: 10, queryRank: 6.03 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_compound": {"$lte": 18}}, {"a_compound": {"$exists": False}}]},
                    {"c_idx": {"$lte": 16}},
                ],
                "c_compound": {"$in": [1, 19, 17]},
                "d_noidx": {"$in": [9, 11, 16]},
            },
        },
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 10, queryRank: 12.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$in": [13, 11, 19]}}, {"c_noidx": {"$gt": 15}}],
                "$or": [{"i_compound": {"$nin": [5, 14, 12, 6]}}, {"i_compound": {"$exists": True}}],
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 10, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"$and": [{"i_compound": {"$exists": False}}, {"z_compound": {"$ne": 6}}]},
                    {"a_compound": {"$gt": 16}},
                    {"a_compound": {"$gt": 20}},
                ],
                "a_idx": {"$nin": [19, 18, 6, 11]},
            },
        },
        {"$sort": {"z_idx": -1}},
    ],
    /* clusterSize: 10, queryRank: 5.03 */ [
        {"$match": {"c_compound": {"$nin": [5, 16, 17]}, "z_compound": {"$lt": 18}}},
        {"$sort": {"a_idx": 1}},
        {"$project": {"d_compound": 1}},
    ],
    /* clusterSize: 10, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [
                    {"$and": [{"a_idx": {"$gt": 10}}, {"c_compound": {"$in": [5, 1]}}]},
                    {"a_compound": {"$elemMatch": {"$nin": [7, 15, 3, 1, 13]}}},
                    {"a_noidx": {"$lt": 10}},
                ],
                "a_compound": {"$gte": 20},
            },
        },
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 10, queryRank: 12.03 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"z_compound": {"$exists": True}}, {"a_idx": {"$all": [7, 20, 12, 1]}}]},
                    {"a_idx": {"$all": [15, 18]}},
                ],
                "h_noidx": {"$nin": [17, 13, 9, 16]},
                "z_idx": {"$lt": 3},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 10, queryRank: 7.03 */ [
        {"$match": {"a_compound": {"$lte": 8}, "i_compound": {"$exists": True}, "k_compound": {"$in": [4, 12, 17, 4]}}},
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 10, queryRank: 15.03 */ [
        {
            "$match": {
                "$nor": [{"c_idx": {"$gte": 15}}, {"a_compound": {"$all": [13, 15, 9, 10]}}],
                "a_compound": {"$exists": True},
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 10, queryRank: 12.02 */ [
        {
            "$match": {
                "$nor": [
                    {"d_idx": {"$exists": False}},
                    {"$or": [{"a_compound": {"$nin": [17, 14, 16]}}, {"a_compound": {"$all": [11, 9, 10]}}]},
                ],
                "z_idx": {"$nin": [9, 2]},
                "z_noidx": {"$nin": [11, 11, 8, 12, 19]},
            },
        },
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 10, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$exists": True, "$in": [6, 7, 1], "$ne": 8}}, "a_idx": {"$lt": 20}}},
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 10, queryRank: 7.03 */ [
        {
            "$match": {
                "$and": [{"i_compound": {"$gte": 7}}, {"k_compound": {"$gt": 15}}],
                "i_idx": {"$nin": [20, 14]},
                "k_idx": {"$nin": [11, 14]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"_id": 0, "d_compound": 1, "d_idx": 1}},
    ],
    /* clusterSize: 10, queryRank: 13.02 */ [
        {
            "$match": {
                "$nor": [
                    {"z_idx": {"$in": [13, 5]}},
                    {"$nor": [{"a_compound": {"$all": [18, 8, 9]}}, {"k_compound": {"$gte": 18}}]},
                ],
                "a_compound": {"$lt": 13},
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "h_compound": 1, "k_idx": 1}},
    ],
    /* clusterSize: 9, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [{"z_compound": {"$exists": True}}, {"a_compound": {"$nin": [10, 14, 19]}}],
                "k_idx": {"$lte": 13},
            },
        },
    ],
    /* clusterSize: 9, queryRank: 5.02 */ [
        {
            "$match": {
                "$and": [{"a_noidx": {"$exists": True}}, {"a_compound": {"$elemMatch": {"$lt": 5, "$ne": 10}}}],
                "a_compound": {"$in": [9, 1, 2, 6]},
                "a_noidx": {"$in": [6, 2]},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 9, queryRank: 6.03 */ [
        {
            "$match": {
                "$nor": [
                    {"d_idx": {"$in": [10, 11]}},
                    {"k_compound": {"$eq": 2}},
                    {"a_compound": {"$nin": [10, 8]}},
                    {"c_noidx": {"$gt": 17}},
                ],
                "i_idx": {"$gt": 17},
            },
        },
    ],
    /* clusterSize: 9, queryRank: 7.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$all": [7, 7, 10]}},
                    {"a_compound": {"$in": [1, 13]}},
                    {"c_compound": {"$exists": False}},
                ],
                "a_idx": {"$exists": True},
                "k_noidx": {"$nin": [15, 18, 9, 18]},
                "z_idx": {"$in": [1, 9, 12, 5]},
            },
        },
        {"$sort": {"z_idx": 1}},
        {"$project": {"a_idx": 1, "i_noidx": 1}},
    ],
    /* clusterSize: 9, queryRank: 7.02 */ [
        {
            "$match": {
                "$nor": [
                    {"k_compound": {"$nin": [10, 8, 11]}},
                    {
                        "$nor": [
                            {"i_idx": {"$in": [15, 5, 15]}},
                            {"$nor": [{"d_compound": {"$gt": 3}}, {"a_idx": {"$in": [17, 17, 9]}}]},
                            {"a_idx": {"$in": [20, 14, 17, 11, 5]}},
                        ],
                    },
                ],
                "a_compound": {"$lte": 5},
                "d_noidx": {"$nin": [4, 4]},
            },
        },
        {"$project": {"a_noidx": 1, "i_compound": 1}},
    ],
    /* clusterSize: 9, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"z_compound": {"$exists": False}},
                    {"a_idx": {"$exists": True}},
                    {"a_compound": {"$nin": [15, 20]}},
                ],
                "i_noidx": {"$gt": 8},
            },
        },
        {"$sort": {"h_idx": 1}},
    ],
    /* clusterSize: 9, queryRank: 7.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$in": [1, 2, 10]}}, {"a_compound": {"$elemMatch": {"$nin": [17, 2, 11, 6]}}}],
                "$nor": [
                    {"h_noidx": {"$nin": [12, 4, 2, 15]}},
                    {"i_compound": {"$in": [4, 13]}},
                    {"d_idx": {"$in": [2, 7, 19]}},
                ],
            },
        },
        {"$sort": {"d_idx": -1}},
    ],
    /* clusterSize: 9, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$lte": 20}, "a_noidx": {"$elemMatch": {"$in": [13, 16, 20, 5]}}}},
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 9, queryRank: 5.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True, "$gt": 6}}},
                    {"a_compound": {"$in": [4, 8, 11]}},
                ],
                "k_noidx": {"$in": [4, 14]},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 9, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [19, 11]}},
                    {"a_compound": {"$elemMatch": {"$exists": False, "$nin": [11, 12, 4]}}},
                    {"$or": [{"i_compound": {"$exists": True}}, {"a_compound": {"$exists": False}}]},
                    {"a_compound": {"$all": [17, 10]}},
                ],
                "k_noidx": {"$nin": [5, 15]},
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 9, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$gt": 4}}, "a_idx": {"$lt": 16}}},
        {"$sort": {"d_idx": -1}},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 8, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$nin": [3, 15]}, "a_idx": {"$elemMatch": {"$in": [1, 13, 3]}}}},
        {"$project": {"a_compound": 1, "a_noidx": 1, "d_idx": 1}},
    ],
    /* clusterSize: 8, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$nin": [12, 1, 2, 9]}, "a_idx": {"$exists": True}, "k_noidx": {"$ne": 9}}},
    ],
    /* clusterSize: 8, queryRank: 13.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [8, 2, 5, 17]}}, {"d_noidx": {"$in": [7, 5]}}],
                "k_compound": {"$gte": 7},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"i_compound": 1}},
    ],
    /* clusterSize: 8, queryRank: 10.02 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$or": [
                            {
                                "$and": [
                                    {"a_compound": {"$nin": [4, 15, 11]}},
                                    {"a_compound": {"$exists": False}},
                                    {
                                        "$or": [
                                            {"h_noidx": {"$nin": [20, 6, 1]}},
                                            {"i_compound": {"$nin": [10, 14, 15]}},
                                        ],
                                    },
                                    {"i_compound": {"$nin": [7, 20]}},
                                ],
                            },
                            {
                                "$or": [
                                    {"a_compound": {"$lte": 6}},
                                    {"i_idx": {"$ne": 1}},
                                    {"h_compound": {"$exists": False}},
                                ],
                            },
                        ],
                    },
                    {"i_noidx": {"$gt": 6}},
                ],
                "h_compound": {"$exists": True},
            },
        },
        {"$sort": {"k_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 8, queryRank: 12.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$gte": 7}}, {"d_noidx": {"$in": [3, 20]}}],
                "$or": [
                    {"a_idx": {"$all": [20, 11, 4]}},
                    {"a_compound": {"$all": [19, 19, 5, 9]}},
                    {"c_compound": {"$exists": True}},
                ],
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 8, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$exists": True}},
                    {"k_compound": {"$nin": [6, 13]}},
                    {"c_compound": {"$in": [1, 1, 9]}},
                ],
                "d_idx": {"$in": [11, 1, 9]},
            },
        },
        {"$project": {"a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 8, queryRank: 3.03 */ [
        {"$match": {"d_compound": {"$exists": True}, "i_idx": {"$nin": [1, 17, 1]}}},
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 8, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$lte": 3}}},
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 8, queryRank: 4.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_compound": {"$exists": False}},
                    {"i_idx": {"$exists": False}},
                    {"a_idx": {"$exists": False}},
                ],
                "a_noidx": {"$eq": 13},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "z_noidx": 1}},
    ],
    /* clusterSize: 7, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$nin": [1, 5]}}, "c_idx": {"$nin": [12, 19]}, "z_idx": {"$ne": 20}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 7, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$in": [7, 1]}}},
        {"$sort": {"a_idx": 1}},
        {"$project": {"a_idx": 1, "a_noidx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 7, queryRank: 10.03 */ [
        {
            "$match": {
                "$nor": [{"a_compound": {"$all": [6, 18, 11]}}, {"a_noidx": {"$nin": [6, 17, 8, 7]}}],
                "c_idx": {"$in": [3, 1, 19]},
            },
        },
        {"$sort": {"d_idx": 1}},
    ],
    /* clusterSize: 7, queryRank: 4.03 */ [
        {"$match": {"a_idx": {"$lte": 3}, "i_compound": {"$exists": True}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 7, queryRank: 5.03 */ [
        {
            "$match": {
                "a_compound": {"$gt": 16},
                "a_noidx": {"$elemMatch": {"$exists": True}},
                "z_compound": {"$ne": 17},
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 7, queryRank: 8.02 */ [
        {
            "$match": {
                "$and": [{"d_noidx": {"$nin": [12, 14]}}, {"c_idx": {"$exists": True}}, {"c_idx": {"$exists": True}}],
                "d_compound": {"$ne": 10},
                "i_compound": {"$nin": [11, 4, 7]},
                "z_compound": {"$ne": 18},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"a_compound": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 6, queryRank: 8.03 */ [
        {
            "$match": {
                "$nor": [
                    {"k_idx": {"$exists": False}},
                    {"$nor": [{"a_compound": {"$ne": 12}}, {"d_compound": {"$exists": True}}]},
                ],
                "a_compound": {"$nin": [16, 13]},
            },
        },
        {"$sort": {"c_idx": 1}},
    ],
    /* clusterSize: 6, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"a_noidx": {"$in": [6, 14]}}, {"d_compound": {"$exists": True}}, {"a_compound": {"$gte": 5}}],
            },
        },
        {"$sort": {"k_idx": 1}},
    ],
    /* clusterSize: 6, queryRank: 5.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$exists": True, "$gte": 19}}, "z_compound": {"$exists": True}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 6, queryRank: 3.02 */ [
        {"$match": {"a_compound": {"$in": [2, 1, 20]}, "z_idx": {"$nin": [7, 8, 11, 12, 18]}}},
        {"$sort": {"i_idx": 1, "k_idx": 1}},
        {"$limit": 219},
        {"$project": {"a_idx": 1, "i_noidx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 6, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$lte": 6}, "a_idx": {"$elemMatch": {"$lte": 2}}}},
        {"$sort": {"i_idx": -1}},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 6, queryRank: 7.02 */ [
        {
            "$match": {
                "$nor": [{"i_idx": {"$in": [2, 13]}}, {"k_compound": {"$lt": 14}}],
                "a_compound": {"$gt": 7},
                "h_idx": {"$gte": 10},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"a_noidx": 1, "c_idx": 1}},
    ],
    /* clusterSize: 6, queryRank: 5.03 */ [
        {"$match": {"$and": [{"a_compound": {"$nin": [19, 12, 9]}}, {"a_compound": {"$elemMatch": {"$lt": 11}}}]}},
        {"$sort": {"z_idx": 1}},
    ],
    /* clusterSize: 6, queryRank: 9.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"a_compound": {"$all": [5, 3]}},
                            {"h_idx": {"$eq": 13}},
                            {"z_compound": {"$in": [12, 18]}},
                        ],
                    },
                    {"c_compound": {"$exists": False}},
                    {"z_idx": {"$ne": 13}},
                ],
                "i_idx": {"$gte": 16},
                "i_noidx": {"$nin": [12, 11]},
            },
        },
    ],
    /* clusterSize: 6, queryRank: 5.03 */ [
        {
            "$match": {
                "$nor": [{"z_noidx": {"$in": [8, 12, 8]}}, {"a_compound": {"$all": [17, 4]}}],
                "a_idx": {"$exists": True},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 165},
    ],
    /* clusterSize: 5, queryRank: 6.02 */ [
        {
            "$match": {
                "$nor": [
                    {"a_noidx": {"$elemMatch": {"$in": [4, 14, 9]}}},
                    {"i_idx": {"$exists": False}},
                    {"i_compound": {"$lte": 9}},
                ],
                "a_compound": {"$lt": 14},
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1}},
    ],
    /* clusterSize: 5, queryRank: 5.03 */ [
        {"$match": {"$nor": [{"a_compound": {"$all": [14, 2]}}, {"a_noidx": {"$gt": 3}}], "a_noidx": {"$in": [12, 2]}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 5, queryRank: 5.03 */ [
        {"$match": {"k_compound": {"$gt": 10}, "z_compound": {"$in": [13, 1, 2]}}},
        {"$sort": {"i_idx": -1}},
    ],
    /* clusterSize: 5, queryRank: 5.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$lte": 15}}, {"a_idx": {"$nin": [11, 7, 18, 3]}}, {"a_noidx": {"$lte": 19}}],
                "c_idx": {"$exists": True},
            },
        },
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 4, queryRank: 6.03 */ [
        {"$match": {"$and": [{"i_compound": {"$exists": True}}, {"c_compound": {"$ne": 10}}, {"k_idx": {"$gte": 2}}]}},
        {"$sort": {"c_idx": -1}},
    ],
    /* clusterSize: 4, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$lt": 5}, "k_noidx": {"$nin": [10, 1, 10, 9]}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 4, queryRank: 4.03 */ [
        {
            "$match": {
                "$and": [{"a_compound": {"$nin": [5, 17, 6]}}, {"k_noidx": {"$ne": 2}}],
                "h_compound": {"$gte": 11},
            },
        },
        {"$sort": {"k_idx": -1}},
        {"$project": {"a_idx": 1, "z_compound": 1}},
    ],
    /* clusterSize: 4, queryRank: 5.02 */ [
        {
            "$match": {
                "$nor": [
                    {"$nor": [{"h_compound": {"$lt": 10}}, {"c_noidx": {"$exists": True}}]},
                    {"c_compound": {"$exists": False}},
                    {"i_idx": {"$in": [7, 16, 18]}},
                ],
                "d_idx": {"$nin": [2, 17]},
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$limit": 80},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 4, queryRank: 5.02 */ [
        {"$match": {"a_compound": {"$lte": 11}, "i_idx": {"$ne": 4}, "k_compound": {"$gt": 3}}},
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "h_compound": 1}},
    ],
    /* clusterSize: 4, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$nin": [10, 17]}, "h_noidx": {"$nin": [6, 1, 6, 1]}, "i_idx": {"$gt": 17}}},
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 4, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$in": [2, 12]}, "h_idx": {"$lt": 16}}},
        {"$sort": {"h_idx": -1}},
    ],
    /* clusterSize: 4, queryRank: 4.03 */ [
        {
            "$match": {
                "$nor": [{"k_compound": {"$in": [17, 7]}}, {"h_compound": {"$exists": False}}],
                "z_noidx": {"$exists": True},
            },
        },
        {"$sort": {"z_idx": -1}},
        {"$project": {"a_idx": 1, "h_compound": 1}},
    ],
    /* clusterSize: 4, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [
                    {
                        "$nor": [
                            {"$and": [{"a_compound": {"$exists": True}}, {"a_idx": {"$in": [13, 3]}}]},
                            {"c_idx": {"$gt": 9}},
                        ],
                    },
                    {"d_compound": {"$lt": 18}},
                    {"k_idx": {"$gt": 12}},
                ],
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 4, queryRank: 4.03 */ [
        {
            "$match": {
                "$and": [{"a_idx": {"$nin": [3, 15, 13]}}, {"a_compound": {"$elemMatch": {"$nin": [17, 10, 18]}}}],
            },
        },
        {"$sort": {"i_idx": -1}},
        {"$project": {"_id": 0, "a_idx": 1}},
    ],
    /* clusterSize: 3, queryRank: 11.03 */ [
        {
            "$match": {
                "$nor": [
                    {"k_noidx": {"$nin": [13, 14]}},
                    {
                        "$nor": [
                            {"c_compound": {"$nin": [8, 10]}},
                            {"a_idx": {"$all": [1, 14, 18, 6]}},
                            {"d_compound": {"$eq": 17}},
                            {"a_idx": {"$lte": 9}},
                        ],
                    },
                ],
                "h_compound": {"$exists": True},
            },
        },
    ],
    /* clusterSize: 3, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$elemMatch": {"$lt": 18}}, "a_noidx": {"$in": [17, 13, 16, 6, 8, 3, 3]}}},
        {"$sort": {"k_idx": -1}},
        {"$project": {"z_idx": 1}},
    ],
    /* clusterSize: 3, queryRank: 6.03 */ [
        {"$match": {"a_idx": {"$in": [4, 1, 13]}, "d_compound": {"$lt": 10}, "k_compound": {"$gt": 16}}},
        {"$sort": {"k_idx": 1}},
    ],
    /* clusterSize: 3, queryRank: 3.03 */ [
        {"$match": {"a_compound": {"$gt": 4}, "h_noidx": {"$nin": [13, 11, 19]}, "z_idx": {"$in": [11, 1, 2, 3]}}},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 2, queryRank: 6.03 */ [
        {
            "$match": {
                "$and": [
                    {"k_idx": {"$lt": 12}},
                    {
                        "$or": [
                            {"a_compound": {"$elemMatch": {"$in": [16, 7], "$nin": [20, 2]}}},
                            {"z_idx": {"$eq": 16}},
                            {"a_compound": {"$in": [3, 17]}},
                        ],
                    },
                    {"z_idx": {"$in": [14, 3, 16]}},
                ],
            },
        },
        {"$sort": {"k_idx": -1}},
    ],
    /* clusterSize: 2, queryRank: 3.03 */ [
        {
            "$match": {
                "$and": [
                    {"c_compound": {"$exists": True}},
                    {"k_noidx": {"$in": [19, 7, 12, 20]}},
                    {"a_idx": {"$exists": True}},
                ],
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 2, queryRank: 4.03 */ [
        {"$match": {"$nor": [{"i_noidx": {"$exists": False}}, {"z_idx": {"$gt": 2}}], "d_compound": {"$gt": 2}}},
        {"$sort": {"a_idx": -1}},
    ],

    /*
            Starting featuresets: 1443
            Desired clusters: 11

            Features before clustering:

            {ConstantOperator.$all: 23,
 ConstantOperator.$and: 10,
 ConstantOperator.$elemMatch: 19,
 ConstantOperator.$eq: 22,
 ConstantOperator.$exists: 18,
 ConstantOperator.$gt: 160,
 ConstantOperator.$gte: 157,
 ConstantOperator.$in: 144,
 ConstantOperator.$lt: 216,
 ConstantOperator.$lte: 200,
 ConstantOperator.$ne: 141,
 ConstantOperator.$nin: 437,
 ConstantOperator.$nor: 6,
 ConstantOperator.$or: 19,
 IndexCardinalityEstimate.-1: 98,
 IndexCardinalityEstimate.-10: 1,
 IndexCardinalityEstimate.-2: 37,
 IndexCardinalityEstimate.-3: 38,
 IndexCardinalityEstimate.-5: 9,
 IndexCardinalityEstimate.-6: 3,
 IndexCardinalityEstimate.-7: 2,
 IndexCardinalityEstimate.-8: 12,
 IndexCardinalityEstimate.-9: 4,
 IndexCardinalityEstimate.0: 1222,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 65,
 <IndexProperty.Calculated_keyPattern_single: 7>: 1361,
 <IndexProperty.Calculated_multiBoundField: 6>: 701,
 <IndexProperty.Calculated_multiFieldBound: 5>: 65,
 <IndexProperty.isMultiKey: 0>: 525,
 PipelineStage.$limit: 689,
 PipelineStage.$match: 1443,
 PipelineStage.$project: 723,
 PipelineStage.$skip: 1388,
 PipelineStage.$sort: 23,
 PlanStage.FETCH: 1388,
 PlanStage.IXSCAN: 1426,
 PlanStage.LIMIT: 676,
 PlanStage.OR: 17,
 PlanStage.PROJECTION_COVERED: 38,
 PlanStage.PROJECTION_SIMPLE: 685,
 PlanStage.SKIP: 1388,
 PlanStage.SORT: 23,
 PlanStage.SUBPLAN: 17,
 PlanStageRelationship.FETCH->IXSCAN: 17,
 PlanStageRelationship.FETCH->SKIP: 1371,
 PlanStageRelationship.LIMIT->FETCH: 317,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 14,
 PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 345,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 30,
 PlanStageRelationship.PROJECTION_COVERED->SKIP: 8,
 PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 674,
 PlanStageRelationship.PROJECTION_SIMPLE->OR: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 5,
 PlanStageRelationship.PROJECTION_SIMPLE->SORT: 5,
 PlanStageRelationship.SKIP->IXSCAN: 1379,
 PlanStageRelationship.SKIP->SORT: 9,
 PlanStageRelationship.SORT->FETCH: 6,
 PlanStageRelationship.SORT->OR: 16,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 7,
 PlanStageRelationship.SUBPLAN->SKIP: 4,
 PlanStageRelationship.SUBPLAN->SORT: 6,
 <RangeProperty.entire: 6>: 24,
 <RangeProperty.leftClosed: 1>: 1276,
 <RangeProperty.leftLimited: 8>: 868,
 <RangeProperty.leftOpen: 0>: 719,
 <RangeProperty.partial: 5>: 1268,
 <RangeProperty.point: 7>: 165,
 <RangeProperty.rightClosed: 4>: 1219,
 <RangeProperty.rightLimited: 9>: 967,
 <RangeProperty.rightOpen: 3>: 775,
 <ResultsetSizeRelative.IDENTICAL: 2>: 8,
 <ResultsetSizeRelative.SMALLER: 1>: 1435}

            Features after clustering:

            {ConstantOperator.$all: 5,
 ConstantOperator.$and: 1,
 ConstantOperator.$elemMatch: 5,
 ConstantOperator.$exists: 3,
 ConstantOperator.$gte: 3,
 ConstantOperator.$in: 3,
 ConstantOperator.$lt: 2,
 ConstantOperator.$lte: 2,
 ConstantOperator.$nin: 6,
 ConstantOperator.$nor: 1,
 ConstantOperator.$or: 5,
 IndexCardinalityEstimate.-8: 1,
 IndexCardinalityEstimate.0: 5,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 5,
 <IndexProperty.Calculated_keyPattern_single: 7>: 1,
 <IndexProperty.Calculated_multiBoundField: 6>: 4,
 <IndexProperty.Calculated_multiFieldBound: 5>: 5,
 <IndexProperty.isMultiKey: 0>: 1,
 PipelineStage.$limit: 6,
 PipelineStage.$match: 11,
 PipelineStage.$project: 4,
 PipelineStage.$skip: 7,
 PipelineStage.$sort: 5,
 PlanStage.FETCH: 6,
 PlanStage.IXSCAN: 6,
 PlanStage.LIMIT: 4,
 PlanStage.OR: 5,
 PlanStage.PROJECTION_SIMPLE: 4,
 PlanStage.SKIP: 7,
 PlanStage.SORT: 5,
 PlanStage.SUBPLAN: 5,
 PlanStageRelationship.FETCH->IXSCAN: 1,
 PlanStageRelationship.FETCH->SKIP: 5,
 PlanStageRelationship.LIMIT->FETCH: 2,
 PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 2,
 PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 3,
 PlanStageRelationship.PROJECTION_SIMPLE->SORT: 1,
 PlanStageRelationship.SKIP->IXSCAN: 5,
 PlanStageRelationship.SKIP->SORT: 2,
 PlanStageRelationship.SORT->OR: 5,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->SKIP: 2,
 PlanStageRelationship.SUBPLAN->SORT: 2,
 <RangeProperty.leftClosed: 1>: 6,
 <RangeProperty.leftLimited: 8>: 4,
 <RangeProperty.leftOpen: 0>: 4,
 <RangeProperty.partial: 5>: 6,
 <RangeProperty.point: 7>: 1,
 <RangeProperty.rightClosed: 4>: 5,
 <RangeProperty.rightLimited: 9>: 6,
 <RangeProperty.rightOpen: 3>: 5,
 <ResultsetSizeRelative.IDENTICAL: 2>: 1,
 <ResultsetSizeRelative.SMALLER: 1>: 10}

            Features remaining:

            {ConstantOperator.$eq,
 ConstantOperator.$gt,
 ConstantOperator.$ne,
 IndexCardinalityEstimate.-1,
 IndexCardinalityEstimate.-10,
 IndexCardinalityEstimate.-2,
 IndexCardinalityEstimate.-3,
 IndexCardinalityEstimate.-5,
 IndexCardinalityEstimate.-6,
 IndexCardinalityEstimate.-7,
 IndexCardinalityEstimate.-9,
 PlanStage.PROJECTION_COVERED,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN,
 PlanStageRelationship.PROJECTION_COVERED->SKIP,
 PlanStageRelationship.PROJECTION_SIMPLE->OR,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP,
 PlanStageRelationship.SORT->FETCH,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE,
 <RangeProperty.entire: 6>}
            */

    /* clusterSize: 183, queryRank: 4.02 */ [
        {"$match": {"c_compound": {"$nin": [11, 10]}, "d_compound": {"$nin": [2, 14, 17, 11]}}},
        {"$skip": 20},
        {"$project": {"_id": 0, "a_compound": 1}},
    ],
    /* clusterSize: 161, queryRank: 7.02 */ [
        {
            "$match": {
                "$and": [{"i_compound": {"$lte": 16}}, {"a_compound": {"$all": [10, 1]}}],
                "a_noidx": {"$elemMatch": {"$in": [12, 4, 1]}},
            },
        },
        {"$limit": 93},
        {"$project": {"a_idx": 1}},
    ],
    /* clusterSize: 161, queryRank: 4.03 */ [
        {"$match": {"i_compound": {"$nin": [18, 17]}, "z_compound": {"$nin": [2, 2, 11, 17]}}},
        {"$skip": 29},
    ],
    /* clusterSize: 158, queryRank: 4.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$elemMatch": {"$gte": 4}}},
                    {"h_idx": {"$exists": False}},
                    {"a_idx": {"$all": [13, 10]}},
                ],
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$skip": 18},
    ],
    /* clusterSize: 156, queryRank: 4.02 */ [
        {"$match": {"c_compound": {"$nin": [8, 19, 18, 4]}, "z_compound": {"$lte": 10}}},
        {"$limit": 127},
        {"$skip": 29},
    ],
    /* clusterSize: 150, queryRank: 4.02 */ [
        {"$match": {"c_compound": {"$nin": [5, 12, 3]}, "z_compound": {"$nin": [19, 18, 16]}}},
        {"$limit": 178},
        {"$skip": 14},
        {"$project": {"_id": 0, "k_idx": 1}},
    ],
    /* clusterSize: 130, queryRank: 10.04 */ [
        {
            "$match": {
                "$or": [
                    {"a_idx": {"$all": [8, 4, 12, 8]}},
                    {"a_compound": {"$all": [11, 3]}},
                    {"a_compound": {"$exists": False}},
                    {"i_compound": {"$exists": True}},
                ],
            },
        },
        {"$sort": {"a_idx": 1}},
    ],
    /* clusterSize: 100, queryRank: 3.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$nor": [
                            {"h_idx": {"$exists": True}},
                            {"$or": [{"k_noidx": {"$nin": [19, 14, 9]}}, {"i_noidx": {"$exists": False}}]},
                            {"k_idx": {"$exists": False}},
                            {"a_noidx": {"$lt": 13}},
                            {"i_noidx": {"$nin": [12, 10]}},
                        ],
                    },
                    {"a_idx": {"$elemMatch": {"$gte": 12}}},
                ],
            },
        },
        {"$sort": {"a_idx": -1}},
        {"$limit": 210},
        {"$skip": 31},
    ],
    /* clusterSize: 91, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$in": [19, 6]}},
                    {"a_compound": {"$elemMatch": {"$gte": 18, "$nin": [4, 14, 18, 19, 18, 18]}}},
                    {"a_compound": {"$all": [12, 6, 11]}},
                ],
            },
        },
        {"$sort": {"a_idx": -1}},
    ],
    /* clusterSize: 77, queryRank: 2.03 */ [{"$match": {"c_compound": {"$lt": 9}}}, {"$limit": 214}, {"$skip": 64}],
    /* clusterSize: 76, queryRank: 10.03 */ [
        {"$match": {"$or": [{"a_compound": {"$all": [6, 3]}}, {"a_compound": {"$elemMatch": {"$in": [4, 1]}}}]}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 186},
        {"$project": {"c_noidx": 1, "h_noidx": 1, "z_idx": 1}},
    ],

    /*
            Starting featuresets: 515
            Desired clusters: 20

            Features before clustering:

            {ConstantOperator.$all: 15,
 ConstantOperator.$and: 7,
 ConstantOperator.$elemMatch: 12,
 ConstantOperator.$eq: 22,
 ConstantOperator.$exists: 11,
 ConstantOperator.$gt: 160,
 ConstantOperator.$gte: 15,
 ConstantOperator.$in: 113,
 ConstantOperator.$lt: 27,
 ConstantOperator.$lte: 23,
 ConstantOperator.$ne: 141,
 ConstantOperator.$nin: 36,
 ConstantOperator.$nor: 5,
 ConstantOperator.$or: 10,
 IndexCardinalityEstimate.-1: 98,
 IndexCardinalityEstimate.-10: 1,
 IndexCardinalityEstimate.-2: 37,
 IndexCardinalityEstimate.-3: 38,
 IndexCardinalityEstimate.-5: 9,
 IndexCardinalityEstimate.-6: 3,
 IndexCardinalityEstimate.-7: 2,
 IndexCardinalityEstimate.-8: 7,
 IndexCardinalityEstimate.-9: 4,
 IndexCardinalityEstimate.0: 308,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 47,
 <IndexProperty.Calculated_keyPattern_single: 7>: 460,
 <IndexProperty.Calculated_multiBoundField: 6>: 275,
 <IndexProperty.Calculated_multiFieldBound: 5>: 47,
 <IndexProperty.isMultiKey: 0>: 210,
 PipelineStage.$limit: 239,
 PipelineStage.$match: 515,
 PipelineStage.$project: 280,
 PipelineStage.$skip: 472,
 PipelineStage.$sort: 14,
 PlanStage.FETCH: 469,
 PlanStage.IXSCAN: 507,
 PlanStage.LIMIT: 231,
 PlanStage.OR: 8,
 PlanStage.PROJECTION_COVERED: 38,
 PlanStage.PROJECTION_SIMPLE: 242,
 PlanStage.SKIP: 472,
 PlanStage.SORT: 14,
 PlanStage.SUBPLAN: 8,
 PlanStageRelationship.FETCH->IXSCAN: 12,
 PlanStageRelationship.FETCH->SKIP: 457,
 PlanStageRelationship.LIMIT->FETCH: 97,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 14,
 PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 120,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 30,
 PlanStageRelationship.PROJECTION_COVERED->SKIP: 8,
 PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 233,
 PlanStageRelationship.PROJECTION_SIMPLE->OR: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 5,
 PlanStageRelationship.PROJECTION_SIMPLE->SORT: 3,
 PlanStageRelationship.SKIP->IXSCAN: 465,
 PlanStageRelationship.SKIP->SORT: 7,
 PlanStageRelationship.SORT->FETCH: 6,
 PlanStageRelationship.SORT->OR: 7,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 5,
 PlanStageRelationship.SUBPLAN->SKIP: 2,
 PlanStageRelationship.SUBPLAN->SORT: 1,
 <RangeProperty.entire: 6>: 24,
 <RangeProperty.leftClosed: 1>: 357,
 <RangeProperty.leftLimited: 8>: 332,
 <RangeProperty.leftOpen: 0>: 321,
 <RangeProperty.partial: 5>: 377,
 <RangeProperty.point: 7>: 136,
 <RangeProperty.rightClosed: 4>: 486,
 <RangeProperty.rightLimited: 9>: 212,
 <RangeProperty.rightOpen: 3>: 191,
 <ResultsetSizeRelative.IDENTICAL: 2>: 4,
 <ResultsetSizeRelative.SMALLER: 1>: 511}

            Features after clustering:

            {ConstantOperator.$all: 3,
 ConstantOperator.$and: 4,
 ConstantOperator.$elemMatch: 4,
 ConstantOperator.$eq: 2,
 ConstantOperator.$exists: 2,
 ConstantOperator.$gt: 8,
 ConstantOperator.$gte: 2,
 ConstantOperator.$in: 7,
 ConstantOperator.$lt: 3,
 ConstantOperator.$lte: 4,
 ConstantOperator.$ne: 2,
 ConstantOperator.$nin: 6,
 ConstantOperator.$nor: 2,
 ConstantOperator.$or: 1,
 IndexCardinalityEstimate.-1: 2,
 IndexCardinalityEstimate.-10: 1,
 IndexCardinalityEstimate.-2: 2,
 IndexCardinalityEstimate.-5: 1,
 IndexCardinalityEstimate.-8: 3,
 IndexCardinalityEstimate.-9: 2,
 IndexCardinalityEstimate.0: 8,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 12,
 <IndexProperty.Calculated_keyPattern_single: 7>: 7,
 <IndexProperty.Calculated_multiBoundField: 6>: 10,
 <IndexProperty.Calculated_multiFieldBound: 5>: 12,
 <IndexProperty.isMultiKey: 0>: 9,
 PipelineStage.$limit: 10,
 PipelineStage.$match: 20,
 PipelineStage.$project: 9,
 PipelineStage.$skip: 13,
 PipelineStage.$sort: 3,
 PlanStage.FETCH: 18,
 PlanStage.IXSCAN: 19,
 PlanStage.LIMIT: 9,
 PlanStage.OR: 1,
 PlanStage.PROJECTION_COVERED: 1,
 PlanStage.PROJECTION_SIMPLE: 8,
 PlanStage.SKIP: 13,
 PlanStage.SORT: 3,
 PlanStage.SUBPLAN: 1,
 PlanStageRelationship.FETCH->IXSCAN: 6,
 PlanStageRelationship.FETCH->SKIP: 12,
 PlanStageRelationship.LIMIT->FETCH: 4,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 1,
 PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 4,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 7,
 PlanStageRelationship.PROJECTION_SIMPLE->SORT: 1,
 PlanStageRelationship.SKIP->IXSCAN: 12,
 PlanStageRelationship.SKIP->SORT: 1,
 PlanStageRelationship.SORT->FETCH: 2,
 PlanStageRelationship.SORT->OR: 1,
 PlanStageRelationship.SUBPLAN->SKIP: 1,
 <RangeProperty.leftClosed: 1>: 16,
 <RangeProperty.leftLimited: 8>: 10,
 <RangeProperty.leftOpen: 0>: 9,
 <RangeProperty.partial: 5>: 13,
 <RangeProperty.point: 7>: 8,
 <RangeProperty.rightClosed: 4>: 18,
 <RangeProperty.rightLimited: 9>: 8,
 <RangeProperty.rightOpen: 3>: 5,
 <ResultsetSizeRelative.SMALLER: 1>: 20}

            Features remaining:

            {IndexCardinalityEstimate.-3,
 IndexCardinalityEstimate.-6,
 IndexCardinalityEstimate.-7,
 PlanStageRelationship.PROJECTION_COVERED->SKIP,
 PlanStageRelationship.PROJECTION_SIMPLE->OR,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE,
 PlanStageRelationship.SUBPLAN->SORT,
 <RangeProperty.entire: 6>,
 <ResultsetSizeRelative.IDENTICAL: 2>}
            */

    /* clusterSize: 46, queryRank: 4.03 */ [
        {"$match": {"c_compound": {"$ne": 15}, "d_compound": {"$nin": [18, 8, 18, 3, 11]}}},
        {"$skip": 22},
    ],
    /* clusterSize: 43, queryRank: 4.03 */ [
        {"$match": {"a_compound": {"$eq": 10}, "i_compound": {"$eq": 10}}},
        {"$limit": 214},
        {"$project": {"i_idx": 1, "k_compound": 1}},
    ],
    /* clusterSize: 42, queryRank: 2.03 */ [{"$match": {"k_compound": {"$gt": 20}}}, {"$skip": 21}],
    /* clusterSize: 41, queryRank: 4.02 */ [
        {"$match": {"i_compound": {"$ne": 15}, "z_compound": {"$nin": [6, 7]}}},
        {"$skip": 12},
        {"$project": {"_id": 0, "a_compound": 1, "h_idx": 1}},
    ],
    /* clusterSize: 40, queryRank: 4.02 */ [
        {"$match": {"a_compound": {"$gt": 16}, "k_compound": {"$gte": 3}}},
        {"$limit": 33},
        {"$skip": 29},
        {"$project": {"_id": 0, "a_noidx": 1}},
    ],
    /* clusterSize: 39, queryRank: 7.02 */ [
        {
            "$match": {
                "$and": [
                    {"d_idx": {"$lte": 20}},
                    {"k_compound": {"$in": [17, 16, 14]}},
                    {"a_compound": {"$elemMatch": {"$in": [14, 18], "$lt": 15}}},
                ],
                "a_compound": {"$exists": True},
            },
        },
        {"$sort": {"d_idx": 1}},
        {"$project": {"a_noidx": 1}},
    ],
    /* clusterSize: 39, queryRank: 4.02 */ [
        {"$match": {"$and": [{"k_compound": {"$gt": 14}}, {"a_compound": {"$lte": 13}}]}},
        {"$skip": 59},
        {"$project": {"_id": 0, "a_idx": 1, "i_idx": 1}},
    ],
    /* clusterSize: 37, queryRank: 4.02 */ [
        {"$match": {"a_compound": {"$gt": 17}, "i_compound": {"$nin": [6, 17, 14]}}},
        {"$limit": 201},
        {"$skip": 12},
        {"$project": {"c_compound": 1}},
    ],
    /* clusterSize: 32, queryRank: 7.03 */ [
        {"$match": {"a_compound": {"$all": [2, 11]}, "i_compound": {"$lte": 11}}},
        {"$sort": {"a_idx": 1, "i_idx": -1}},
        {"$limit": 46},
    ],
    /* clusterSize: 31, queryRank: 2.02 */ [
        {"$match": {"z_compound": {"$nin": [1, 16]}}},
        {"$limit": 58},
        {"$project": {"_id": 0, "z_compound": 1}},
    ],
    /* clusterSize: 23, queryRank: 2.03 */ [{"$match": {"a_compound": {"$gt": 19}}}, {"$limit": 165}, {"$skip": 6}],
    /* clusterSize: 21, queryRank: 2.03 */ [{"$match": {"d_compound": {"$lt": 6}}}, {"$skip": 4}],
    /* clusterSize: 19, queryRank: 7.03 */ [
        {
            "$match": {
                "$nor": [
                    {"a_idx": {"$gt": 15}},
                    {"d_compound": {"$in": [7, 17]}},
                    {"i_compound": {"$nin": [6, 9, 17, 12, 4]}},
                ],
                "a_compound": {"$in": [12, 13]},
            },
        },
        {"$limit": 248},
    ],
    /* clusterSize: 19, queryRank: 4.03 */ [
        {"$match": {"c_compound": {"$lte": 9}, "z_compound": {"$in": [2, 14, 6]}}},
        {"$skip": 5},
    ],
    /* clusterSize: 12, queryRank: 2.03 */ [{"$match": {"d_compound": {"$gte": 7}}}, {"$skip": 14}],
    /* clusterSize: 10, queryRank: 4.03 */ [
        {
            "$match": {
                "$and": [
                    {"a_noidx": {"$elemMatch": {"$lt": 12}}},
                    {"$and": [{"i_compound": {"$eq": 3}}, {"z_compound": {"$in": [15, 2]}}]},
                ],
                "a_noidx": {"$in": [3, 10, 12]},
            },
        },
        {"$limit": 29},
    ],
    /* clusterSize: 8, queryRank: 2.03 */ [{"$match": {"i_compound": {"$gt": 13}}}, {"$limit": 252}, {"$skip": 45}],
    /* clusterSize: 8, queryRank: 10.03 */ [
        {
            "$match": {
                "$or": [
                    {"$or": [{"a_compound": {"$all": [11, 8, 15]}}, {"a_compound": {"$all": [7, 20, 19]}}]},
                    {"a_idx": {"$elemMatch": {"$gt": 10}}},
                    {"a_idx": {"$in": [7, 9, 12]}},
                ],
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$skip": 28},
    ],
    /* clusterSize: 3, queryRank: 9.02 */ [
        {
            "$match": {
                "$nor": [
                    {
                        "$and": [
                            {
                                "$and": [
                                    {"k_idx": {"$nin": [8, 15, 6]}},
                                    {"a_noidx": {"$elemMatch": {"$exists": False}}},
                                    {"a_noidx": {"$exists": True}},
                                    {"a_noidx": {"$elemMatch": {"$exists": True}}},
                                    {"a_compound": {"$elemMatch": {"$exists": True}}},
                                ],
                            },
                            {"a_idx": {"$all": [11, 6, 8]}},
                        ],
                    },
                    {"k_compound": {"$in": [13, 2, 9]}},
                ],
                "a_compound": {"$all": [20, 2, 20]},
                "i_compound": {"$in": [17, 20]},
            },
        },
        {"$project": {"_id": 0, "i_idx": 1}},
    ],
    /* clusterSize: 2, queryRank: 2.02 */ [
        {"$match": {"i_compound": {"$in": [17, 15, 7]}}},
        {"$limit": 175},
        {"$skip": 1},
        {"$project": {"a_compound": 1}},
    ],

    /*
            Starting featuresets: 75
            Desired clusters: 11

            Features before clustering:

            {ConstantOperator.$all: 5,
 ConstantOperator.$and: 1,
 ConstantOperator.$elemMatch: 2,
 ConstantOperator.$eq: 8,
 ConstantOperator.$exists: 6,
 ConstantOperator.$gt: 3,
 ConstantOperator.$gte: 5,
 ConstantOperator.$in: 26,
 ConstantOperator.$lt: 10,
 ConstantOperator.$lte: 9,
 ConstantOperator.$ne: 2,
 ConstantOperator.$nin: 9,
 ConstantOperator.$nor: 1,
 ConstantOperator.$or: 6,
 IndexCardinalityEstimate.-1: 1,
 IndexCardinalityEstimate.-2: 3,
 IndexCardinalityEstimate.-3: 38,
 IndexCardinalityEstimate.-5: 1,
 IndexCardinalityEstimate.-6: 3,
 IndexCardinalityEstimate.-7: 2,
 IndexCardinalityEstimate.0: 21,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 25,
 <IndexProperty.Calculated_keyPattern_single: 7>: 44,
 <IndexProperty.Calculated_multiBoundField: 6>: 35,
 <IndexProperty.Calculated_multiFieldBound: 5>: 25,
 <IndexProperty.isMultiKey: 0>: 25,
 PipelineStage.$limit: 34,
 PipelineStage.$match: 75,
 PipelineStage.$project: 59,
 PipelineStage.$skip: 49,
 PipelineStage.$sort: 6,
 PlanStage.FETCH: 36,
 PlanStage.IXSCAN: 69,
 PlanStage.LIMIT: 31,
 PlanStage.OR: 6,
 PlanStage.PROJECTION_COVERED: 33,
 PlanStage.PROJECTION_SIMPLE: 26,
 PlanStage.SKIP: 49,
 PlanStage.SORT: 6,
 PlanStage.SUBPLAN: 6,
 PlanStageRelationship.FETCH->SKIP: 36,
 PlanStageRelationship.LIMIT->FETCH: 7,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 11,
 PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 13,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 25,
 PlanStageRelationship.PROJECTION_COVERED->SKIP: 8,
 PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 20,
 PlanStageRelationship.PROJECTION_SIMPLE->OR: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 5,
 PlanStageRelationship.SKIP->IXSCAN: 44,
 PlanStageRelationship.SKIP->SORT: 5,
 PlanStageRelationship.SORT->OR: 5,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 5,
 PlanStageRelationship.SUBPLAN->SORT: 1,
 <RangeProperty.entire: 6>: 24,
 <RangeProperty.leftClosed: 1>: 68,
 <RangeProperty.leftLimited: 8>: 17,
 <RangeProperty.leftOpen: 0>: 13,
 <RangeProperty.partial: 5>: 36,
 <RangeProperty.point: 7>: 34,
 <RangeProperty.rightClosed: 4>: 63,
 <RangeProperty.rightLimited: 9>: 29,
 <RangeProperty.rightOpen: 3>: 20,
 <ResultsetSizeRelative.IDENTICAL: 2>: 4,
 <ResultsetSizeRelative.SMALLER: 1>: 71}

            Features after clustering:

            {ConstantOperator.$all: 1,
 ConstantOperator.$eq: 3,
 ConstantOperator.$exists: 1,
 ConstantOperator.$gte: 2,
 ConstantOperator.$in: 2,
 ConstantOperator.$lt: 1,
 ConstantOperator.$lte: 2,
 ConstantOperator.$nin: 1,
 ConstantOperator.$or: 1,
 IndexCardinalityEstimate.-3: 5,
 IndexCardinalityEstimate.-6: 1,
 IndexCardinalityEstimate.-7: 1,
 IndexCardinalityEstimate.0: 3,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 3,
 <IndexProperty.Calculated_keyPattern_single: 7>: 7,
 <IndexProperty.Calculated_multiBoundField: 6>: 3,
 <IndexProperty.Calculated_multiFieldBound: 5>: 3,
 <IndexProperty.isMultiKey: 0>: 3,
 PipelineStage.$limit: 4,
 PipelineStage.$match: 11,
 PipelineStage.$project: 7,
 PipelineStage.$skip: 7,
 PipelineStage.$sort: 1,
 PlanStage.FETCH: 5,
 PlanStage.IXSCAN: 10,
 PlanStage.LIMIT: 3,
 PlanStage.OR: 1,
 PlanStage.PROJECTION_COVERED: 5,
 PlanStage.PROJECTION_SIMPLE: 2,
 PlanStage.SKIP: 7,
 PlanStage.SORT: 1,
 PlanStage.SUBPLAN: 1,
 PlanStageRelationship.FETCH->SKIP: 5,
 PlanStageRelationship.LIMIT->FETCH: 1,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 1,
 PlanStageRelationship.LIMIT->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 4,
 PlanStageRelationship.PROJECTION_COVERED->SKIP: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->FETCH: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 1,
 PlanStageRelationship.SKIP->IXSCAN: 6,
 PlanStageRelationship.SKIP->SORT: 1,
 PlanStageRelationship.SORT->OR: 1,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 1,
 <RangeProperty.entire: 6>: 2,
 <RangeProperty.leftClosed: 1>: 10,
 <RangeProperty.leftLimited: 8>: 3,
 <RangeProperty.leftOpen: 0>: 1,
 <RangeProperty.partial: 5>: 6,
 <RangeProperty.point: 7>: 5,
 <RangeProperty.rightClosed: 4>: 9,
 <RangeProperty.rightLimited: 9>: 4,
 <RangeProperty.rightOpen: 3>: 2,
 <ResultsetSizeRelative.IDENTICAL: 2>: 1,
 <ResultsetSizeRelative.SMALLER: 1>: 10}

            Features remaining:

            {ConstantOperator.$and,
 ConstantOperator.$elemMatch,
 ConstantOperator.$gt,
 ConstantOperator.$ne,
 ConstantOperator.$nor,
 IndexCardinalityEstimate.-1,
 IndexCardinalityEstimate.-2,
 IndexCardinalityEstimate.-5,
 PlanStageRelationship.PROJECTION_SIMPLE->OR,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE,
 PlanStageRelationship.SUBPLAN->SORT}
            */

    /* clusterSize: 14, queryRank: 4.02 */ [
        {"$match": {"a_compound": {"$in": [20, 15, 20]}, "i_compound": {"$gte": 5}}},
        {"$limit": 237},
        {"$skip": 1},
        {"$project": {"_id": 0, "i_noidx": 1}},
    ],
    /* clusterSize: 11, queryRank: 2.03 */ [
        {"$match": {"c_compound": {"$lte": 9}}},
        {"$project": {"_id": 0, "c_compound": 1}},
    ],
    /* clusterSize: 9, queryRank: 2.03 */ [{"$match": {"a_compound": {"$in": [6, 13]}}}, {"$skip": 10}],
    /* clusterSize: 7, queryRank: 2.03 */ [
        {"$match": {"a_compound": {"$eq": 9}}},
        {"$limit": 123},
        {"$project": {"_id": 0, "i_compound": 1}},
    ],
    /* clusterSize: 7, queryRank: 2.03 */ [
        {"$match": {"d_compound": {"$nin": [2, 1]}}},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],
    /* clusterSize: 6, queryRank: 7.02 */ [
        {"$match": {"$or": [{"k_compound": {"$exists": True}}, {"a_idx": {"$all": [7, 4]}}]}},
        {"$sort": {"a_idx": -1}},
        {"$limit": 196},
        {"$skip": 19},
        {"$project": {"_id": 0, "i_noidx": 1}},
    ],
    /* clusterSize: 5, queryRank: 2.03 */ [{"$match": {"i_compound": {"$lte": 14}}}, {"$limit": 43}, {"$skip": 11}],
    /* clusterSize: 5, queryRank: 2.03 */ [{"$match": {"d_compound": {"$eq": 5}}}, {"$skip": 5}],
    /* clusterSize: 4, queryRank: 2.03 */ [
        {"$match": {"k_compound": {"$gte": 11}}},
        {"$skip": 68},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],
    /* clusterSize: 4, queryRank: 2.03 */ [{"$match": {"i_compound": {"$lt": 16}}}, {"$skip": 12}],
    /* clusterSize: 3, queryRank: 2.03 */ [
        {"$match": {"k_compound": {"$eq": 8}}},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],

    /*
            Starting featuresets: 12
            Desired clusters: 11

            Features before clustering:

            {ConstantOperator.$and: 1,
 ConstantOperator.$elemMatch: 2,
 ConstantOperator.$eq: 1,
 ConstantOperator.$exists: 2,
 ConstantOperator.$gt: 3,
 ConstantOperator.$gte: 1,
 ConstantOperator.$in: 5,
 ConstantOperator.$lte: 1,
 ConstantOperator.$ne: 2,
 ConstantOperator.$nin: 1,
 ConstantOperator.$nor: 1,
 ConstantOperator.$or: 2,
 IndexCardinalityEstimate.-1: 1,
 IndexCardinalityEstimate.-2: 3,
 IndexCardinalityEstimate.-3: 1,
 IndexCardinalityEstimate.-5: 1,
 IndexCardinalityEstimate.0: 4,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 7,
 <IndexProperty.Calculated_keyPattern_single: 7>: 3,
 <IndexProperty.Calculated_multiBoundField: 6>: 6,
 <IndexProperty.Calculated_multiFieldBound: 5>: 7,
 <IndexProperty.isMultiKey: 0>: 4,
 PipelineStage.$limit: 5,
 PipelineStage.$match: 12,
 PipelineStage.$project: 11,
 PipelineStage.$skip: 5,
 PipelineStage.$sort: 2,
 PlanStage.FETCH: 1,
 PlanStage.IXSCAN: 10,
 PlanStage.LIMIT: 5,
 PlanStage.OR: 2,
 PlanStage.PROJECTION_COVERED: 9,
 PlanStage.PROJECTION_SIMPLE: 2,
 PlanStage.SKIP: 5,
 PlanStage.SORT: 2,
 PlanStage.SUBPLAN: 2,
 PlanStageRelationship.FETCH->SKIP: 1,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 5,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 6,
 PlanStageRelationship.PROJECTION_COVERED->SKIP: 3,
 PlanStageRelationship.PROJECTION_SIMPLE->OR: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 1,
 PlanStageRelationship.SKIP->IXSCAN: 4,
 PlanStageRelationship.SKIP->SORT: 1,
 PlanStageRelationship.SORT->OR: 1,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->SORT: 1,
 <RangeProperty.entire: 6>: 7,
 <RangeProperty.leftClosed: 1>: 9,
 <RangeProperty.leftLimited: 8>: 5,
 <RangeProperty.leftOpen: 0>: 5,
 <RangeProperty.partial: 5>: 6,
 <RangeProperty.point: 7>: 4,
 <RangeProperty.rightClosed: 4>: 10,
 <RangeProperty.rightLimited: 9>: 3,
 <RangeProperty.rightOpen: 3>: 2,
 <ResultsetSizeRelative.SMALLER: 1>: 12}

            Features after clustering:

            {ConstantOperator.$and: 1,
 ConstantOperator.$elemMatch: 2,
 ConstantOperator.$eq: 1,
 ConstantOperator.$exists: 2,
 ConstantOperator.$gt: 2,
 ConstantOperator.$gte: 1,
 ConstantOperator.$in: 5,
 ConstantOperator.$lte: 1,
 ConstantOperator.$ne: 2,
 ConstantOperator.$nin: 1,
 ConstantOperator.$nor: 1,
 ConstantOperator.$or: 2,
 IndexCardinalityEstimate.-1: 1,
 IndexCardinalityEstimate.-2: 3,
 IndexCardinalityEstimate.-3: 1,
 IndexCardinalityEstimate.-5: 1,
 IndexCardinalityEstimate.0: 3,
 <IndexProperty.Calculated_keyPattern_compound: 8>: 6,
 <IndexProperty.Calculated_keyPattern_single: 7>: 3,
 <IndexProperty.Calculated_multiBoundField: 6>: 6,
 <IndexProperty.Calculated_multiFieldBound: 5>: 6,
 <IndexProperty.isMultiKey: 0>: 3,
 PipelineStage.$limit: 4,
 PipelineStage.$match: 11,
 PipelineStage.$project: 10,
 PipelineStage.$skip: 5,
 PipelineStage.$sort: 2,
 PlanStage.FETCH: 1,
 PlanStage.IXSCAN: 9,
 PlanStage.LIMIT: 4,
 PlanStage.OR: 2,
 PlanStage.PROJECTION_COVERED: 8,
 PlanStage.PROJECTION_SIMPLE: 2,
 PlanStage.SKIP: 5,
 PlanStage.SORT: 2,
 PlanStage.SUBPLAN: 2,
 PlanStageRelationship.FETCH->SKIP: 1,
 PlanStageRelationship.LIMIT->PROJECTION_COVERED: 4,
 PlanStageRelationship.PROJECTION_COVERED->IXSCAN: 5,
 PlanStageRelationship.PROJECTION_COVERED->SKIP: 3,
 PlanStageRelationship.PROJECTION_SIMPLE->OR: 1,
 PlanStageRelationship.PROJECTION_SIMPLE->SKIP: 1,
 PlanStageRelationship.SKIP->IXSCAN: 4,
 PlanStageRelationship.SKIP->SORT: 1,
 PlanStageRelationship.SORT->OR: 1,
 PlanStageRelationship.SORT->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->PROJECTION_SIMPLE: 1,
 PlanStageRelationship.SUBPLAN->SORT: 1,
 <RangeProperty.entire: 6>: 6,
 <RangeProperty.leftClosed: 1>: 8,
 <RangeProperty.leftLimited: 8>: 4,
 <RangeProperty.leftOpen: 0>: 4,
 <RangeProperty.partial: 5>: 5,
 <RangeProperty.point: 7>: 4,
 <RangeProperty.rightClosed: 4>: 9,
 <RangeProperty.rightLimited: 9>: 3,
 <RangeProperty.rightOpen: 3>: 2,
 <ResultsetSizeRelative.SMALLER: 1>: 11}

            Features remaining:

            set()
            */

    /* clusterSize: 2, queryRank: 2.02 */ [
        {"$match": {"a_compound": {"$gt": 1}}},
        {"$limit": 122},
        {"$project": {"_id": 0, "i_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.02 */ [
        {"$match": {"z_compound": {"$ne": 11}}},
        {"$skip": 73},
        {"$project": {"_id": 0, "z_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.03 */ [
        {"$match": {"a_compound": {"$in": [3, 15]}}},
        {"$limit": 242},
        {"$project": {"_id": 0, "i_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.02 */ [
        {"$match": {"d_compound": {"$in": [6, 16, 8, 6]}}},
        {"$limit": 204},
        {"$skip": 17},
        {"$project": {"_id": 0, "d_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.03 */ [
        {"$match": {"a_compound": {"$in": [10, 12, 18]}}},
        {"$limit": 44},
        {"$project": {"_id": 0, "i_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 3.03 */ [
        {
            "$match": {
                "$or": [
                    {
                        "$and": [
                            {"i_compound": {"$nin": [9, 3, 7, 20, 6]}},
                            {
                                "$nor": [
                                    {"i_compound": {"$exists": False}},
                                    {"a_idx": {"$elemMatch": {"$exists": False}}},
                                ],
                            },
                            {"$or": [{"i_noidx": {"$in": [9, 8]}}, {"d_idx": {"$nin": [10, 8, 17]}}]},
                        ],
                    },
                    {"h_compound": {"$exists": False}},
                    {"k_idx": {"$exists": False}},
                ],
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$project": {"_id": 0, "a_idx": 1, "a_noidx": 1, "k_compound": 1, "z_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.03 */ [{"$match": {"z_compound": {"$gt": 3}}}, {"$skip": 2}],
    /* clusterSize: 1, queryRank: 2.02 */ [
        {"$match": {"d_compound": {"$ne": 1}}},
        {"$skip": 60},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.03 */ [
        {"$match": {"d_compound": {"$lte": 5}}},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 3.02 */ [
        {
            "$match": {
                "$or": [
                    {"a_compound": {"$elemMatch": {"$exists": True, "$gte": 10}}},
                    {"a_idx": {"$elemMatch": {"$eq": 3}}},
                ],
            },
        },
        {"$sort": {"a_idx": 1}},
        {"$skip": 91},
        {"$project": {"a_compound": 1}},
    ],
    /* clusterSize: 1, queryRank: 2.03 */ [
        {"$match": {"d_compound": {"$in": [7, 10, 7, 2]}}},
        {"$project": {"_id": 0, "k_compound": 1}},
    ],
];

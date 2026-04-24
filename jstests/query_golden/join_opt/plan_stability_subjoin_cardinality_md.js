/**
 * Extract individual subjoins out of the winning plans of the TPC-H fuzzed queries,
 * convert them to stand-alone MQL statements and compare their cardinality estimates
 * to the actual cardinality of the subjoin.
 *
 * @tags: [
 * incompatible_aubsan,
 * tsan_incompatible,
 * ]
 *
 */

import {joinPlanToString, newlineBeforeEachStage} from "jstests/query_golden/libs/pretty_printers.js";
import {populateTPCHDataset} from "jstests/libs/query/tpch_dataset.js";
import {commands} from "jstests/query_golden/test_inputs/plan_stability_pipelines_tpch_fuzzed.js";

// Report only subjoins with cardinality estimates that differ from the actual cardinality
// by more than this many orders of magnitude.
const ORDERS_OF_MAGNITUDE_REPORTING_THRESHOLD = 2;

/**
 * Convert a particular plan stage into a MQL pipeline fragment,
 * possibly also identifying the base collection that pipeline would operate on.
 */
function reconstructStage(stage) {
    switch (stage.stage) {
        case "NESTED_LOOP_JOIN_EMBEDDING":
        case "INDEXED_NESTED_LOOP_JOIN_EMBEDDING":
        case "HASH_JOIN_EMBEDDING":
            return {reconstructedBaseCollection: undefined, reconstructedStages: reconstructJoin(stage)};
        case "COLLSCAN":
        case "FETCH":
        case "IXSCAN":
            return {
                reconstructedBaseCollection: getCollectionName(stage),
                reconstructedStages: [{$match: getPredicate(stage)}],
            };
        default:
            throw new Error(`Unsupported stage "${stage.stage}"`);
    }
}

/**
 * Convert a join stage from the plan into a $lookup stage
 */
function reconstructJoin(stage) {
    assert(stage.stage.includes("EMBEDDING"), `Unsupported stage "${stage.stage}"`);
    assert(stage.inputStages.length === 2, `Join stage "${stage.stage}" must have exactly two inputs"`);

    let joinPredicates = stage.joinPredicates;

    // If possible, lock on to a join predicate of form `field1 = field2` rather than `field1 = coll2.field2`.
    const filteredJoinPredicates = joinPredicates.filter((item) => !item.includes("."));
    if (filteredJoinPredicates.length === 0) {
        assert(joinPredicates.length === 1);
    } else if (filteredJoinPredicates.length === 1) {
        joinPredicates = filteredJoinPredicates;
    } else {
        assert(false, "Test does not currently support joins with more than one predicate");
    }

    // Determine the two sides of the join predicate
    const joinPredicate = joinPredicates[0];
    const parts = joinPredicate.split(" = ");
    assert(parts.length === 2, `${joinPredicate} could not be split`);

    let fromTable;
    let localField;
    let foreignField;

    // Determine which input is the base table and which input is potentially a nested join
    const baseTables = stage.inputStages.filter((is) => !is.stage.includes("EMBEDDING"));
    assert(
        baseTables.length > 0,
        `Expected at least one base table, but found ${baseTables.length} in ${JSON.stringify(stage.inputStages)}`,
    );

    if (baseTables.length === 2) {
        // Both inputs to the join are base tables
        fromTable = stage.inputStages[1];
        localField = parts[0].substring(parts[0].indexOf(".") + 1);
        foreignField = parts[1].substring(parts[1].indexOf(".") + 1);
    } else if (stage.inputStages[0].stage.includes("EMBEDDING")) {
        // The first join input is a join itself, so the base table is the second input
        fromTable = stage.inputStages[1];
        localField = parts[0].substring(parts[0].indexOf(".") + 1);
        foreignField = parts[1].substring(parts[1].indexOf(".") + 1);
    } else if (stage.inputStages[1].stage.includes("EMBEDDING")) {
        // The second join input is a join itself, so the base table is the first input
        fromTable = stage.inputStages[0];
        localField = parts[1].substring(parts[1].indexOf(".") + 1);
        foreignField = parts[0].substring(parts[0].indexOf(".") + 1);
    } else {
        assert(false, "Unable to determine base table for join");
    }

    // Make sure we have identified the base table input correctly
    assert(fromTable.stage === "COLLSCAN" || fromTable.stage === "FETCH");

    const fromTableName = getCollectionName(fromTable);
    const {reconstructedStages: lookupSubpipeline} = reconstructStage(fromTable);

    return [
        {
            "$lookup": {
                "from": fromTableName,
                "localField": localField,
                "foreignField": foreignField,
                "as": fromTableName,
                "pipeline": lookupSubpipeline,
            },
        },
        {
            "$unwind": "$" + fromTableName,
        },
        {
            // Flatten the joined document into the base document to enable
            // future stages to refer to those fields without having to prefix
            // them with a collection name.
            $replaceRoot: {
                newRoot: {
                    $mergeObjects: ["$$ROOT", "$" + fromTableName],
                },
            },
        },
    ];
}

function getCollectionName(stage) {
    assert(stage.nss, `Stage ${stage.stage} does not have an nss field`);
    return stage.nss.split(".").pop();
}
/**
 * Extract query predicates from a plan stage.
 */
function getPredicate(stage) {
    let predicate = [];
    if (stage.filter !== undefined) {
        predicate.push(stage.filter);
    }
    switch (stage.stage) {
        case "COLLSCAN":
            break;
        case "FETCH":
            predicate.push(getPredicate(stage.inputStage));
            break;
        case "IXSCAN":
            predicate.push(getPredicateFromIxscan(stage));
            break;
        case "INDEX_PROBE_NODE":
            break;
        default:
            throw new Error(`Unknown stage type ${stage.stage}`);
    }

    if (predicate.length === 0) {
        return {};
    } else if (predicate.length === 1) {
        return predicate[0];
    } else {
        return {$and: predicate};
    }
}

/**
 * Extract query predicates from the IXSCAN range.
 */
function getPredicateFromIxscan(stage) {
    assert(stage.keyPattern, `Stage ${stage.stage} does not have a keyPattern field`);
    assert(stage.indexBounds, `Stage ${stage.stage} does not have an indexBounds field`);
    const keyPattern = stage.keyPattern;
    const indexBounds = stage.indexBounds;

    const filter = {};
    for (const field of Object.keys(keyPattern)) {
        const bounds = indexBounds[field];
        if (!bounds || bounds.length === 0) continue;

        // Parse bound strings like '["value", "value"]' or '(5, Infinity]'
        const parsedBounds = bounds.map(parseIndexBound);

        if (parsedBounds.length === 1) {
            const b = parsedBounds[0];
            if (b.isEquality) {
                filter[field] = b.lower.value;
            } else if (b.isFullRange) {
                // [MinKey, MaxKey] — no constraint on this field
                continue;
            } else {
                const rangeFilter = {};
                if (b.lower.value !== "MinKey" && b.lower.value !== undefined) {
                    rangeFilter[b.lower.inclusive ? "$gte" : "$gt"] = b.lower.value;
                }
                if (b.upper.value !== "MaxKey" && b.upper.value !== undefined) {
                    rangeFilter[b.upper.inclusive ? "$lte" : "$lt"] = b.upper.value;
                }
                if (Object.keys(rangeFilter).length > 0) {
                    filter[field] = rangeFilter;
                }
            }
        } else {
            // Multiple bounds → $in or $or on this field
            const allEquality = parsedBounds.every((b) => b.isEquality);
            if (allEquality) {
                filter[field] = {$in: parsedBounds.map((b) => b.lower.value)};
            } else {
                // Complex multiple range bounds
                const orConditions = parsedBounds.map((b) => {
                    if (b.isEquality) return {[field]: b.lower.value};
                    const cond = {};
                    if (b.lower.value !== "MinKey") {
                        cond[b.lower.inclusive ? "$gte" : "$gt"] = b.lower.value;
                    }
                    if (b.upper.value !== "MaxKey") {
                        cond[b.upper.inclusive ? "$lte" : "$lt"] = b.upper.value;
                    }
                    return {[field]: cond};
                });
                if (!filter.$or) filter.$or = [];
                filter.$or.push(...orConditions);
            }
        }
    }
    return filter;
}

/**
 * Reconstruct the index bound out of an indexBounds string.
 * Since the indexBounds are strings containing serialized representations,
 * we need to do a lot of heavy lifting to reconstruct the original predicate.
 */
function parseIndexBound(boundStr) {
    // Bounds look like this: '["abc", "abc"]', '(5.0, 10.0]', '[MinKey, MaxKey]'
    const match = boundStr.match(/^([\[\(])\s*(.+?)\s*,\s*(.+?)\s*([\]\)])$/);
    if (!match) {
        return {
            isEquality: false,
            isFullRange: false,
            lower: {value: undefined, inclusive: true},
            upper: {value: undefined, inclusive: true},
        };
    }

    const lowerInclusive = match[1] === "[";
    const upperInclusive = match[4] === "]";
    const lowerVal = parseIndexBoundLiteral(match[2].trim());
    const upperVal = parseIndexBoundLiteral(match[3].trim());

    const isEquality =
        lowerInclusive &&
        upperInclusive &&
        JSON.stringify(lowerVal) === JSON.stringify(upperVal) &&
        lowerVal !== "MinKey" &&
        lowerVal !== "MaxKey";

    const isFullRange = lowerInclusive && upperInclusive && lowerVal === "MinKey" && upperVal === "MaxKey";

    return {
        isEquality,
        isFullRange,
        lower: {value: lowerVal, inclusive: lowerInclusive},
        upper: {value: upperVal, inclusive: upperInclusive},
    };
}

/**
 * Parse an individual literal as seen in an index bound
 */
function parseIndexBoundLiteral(str) {
    if (str === "MinKey") return "MinKey";
    if (str === "MaxKey") return "MaxKey";
    if (str === "null") return null;
    if (str === "true") return true;
    if (str === "false") return false;
    if (str === "Infinity" || str === "inf") return Infinity;
    if (str === "-Infinity" || str === "-inf") return -Infinity;

    // Try to parse as number
    const num = Number(str);
    if (!isNaN(num) && str !== '""' && str !== "") return num;

    // Remove surrounding quotes if present
    const strMatch = str.match(/^"(.*)"$/);
    if (strMatch) return strMatch[1];

    // ObjectId
    const oidMatch = str.match(/^ObjectId\('([a-f0-9]+)'\)$/);
    if (oidMatch) return new ObjectId(oidMatch[1]);

    // ISODate
    const isoDateMatch = str.match(/^ISODate\('(.+)'\)$/);
    if (isoDateMatch) return new ISODate(isoDateMatch[1]);

    // Date
    const dateMatch = str.match(/^new Date\((.+)\)$/);
    if (dateMatch) return new Date(parseInt(dateMatch[1]));

    return str;
}

/**
 * Walk the query plan depth first, so that we visit and reconstruct
 * subjoins before their parent stages.
 */
function walkReverseDepth(subplan, callback) {
    const queue = [subplan];
    const levels = [];

    while (queue.length > 0) {
        const levelSize = queue.length;
        const currentLevel = [];

        for (let i = 0; i < levelSize; i++) {
            const node = queue.shift();
            currentLevel.push(node);

            if (node.hasOwnProperty("inputStage")) {
                queue.push(node.inputStage);
            } else if (node.hasOwnProperty("inputStages")) {
                queue.push(...node.inputStages);
            }
        }

        levels.push(currentLevel);
    }

    // Walk deepest level first
    for (let depth = levels.length - 1; depth >= 0; depth--) {
        for (const node of levels[depth]) {
            callback(node, depth);
        }
    }
}

/**
 * Normalize the absolute difference between two values and report it in terms of orders of magnitude.
 */
function orderOfMagnitudeDiff(a, b) {
    assert(a > 0 && b > 0);
    return Math.abs(Math.floor(Math.log10(Math.abs(a))) - Math.floor(Math.log10(Math.abs(b))));
}

/**
 * Check the cardinality estimates for all subjoins of a given command
 */
function checkCommandEstimates(db, command) {
    const originalBaseCollection = command.aggregate;
    const originalPipeline = command.pipeline;

    const explain = db[originalBaseCollection].explain().aggregate(originalPipeline);

    print(`## >>> Command idx ${command.idx}`);
    print("```");
    print(newlineBeforeEachStage(JSON.stringify(command)));
    print("```");

    if (
        !(
            explain.hasOwnProperty("queryPlanner") &&
            explain.queryPlanner.hasOwnProperty("winningPlan") &&
            explain.queryPlanner.winningPlan.hasOwnProperty("queryPlan")
        )
    ) {
        // Query also contains legacy $lookup stages so we are unable to extract its subjoins.
        print(`Query is not eligible, as it does not have an SBE-only plan.`);
        return;
    }

    const originalPlan = explain.queryPlanner.winningPlan.queryPlan;

    assert(
        originalPlan.hasOwnProperty("cardinalityEstimate"),
        "Plan does not have cardinalityEstimate field: " + tojson(originalPlan),
    );
    assert(
        explain.queryPlanner.winningPlan.hasOwnProperty("usedJoinOptimization") &&
            explain.queryPlanner.winningPlan.usedJoinOptimization,
        "Plan does not have usedJoinOptimization field set to true: " + tojson(originalPlan),
    );

    // All our reconstructions use the same base collection
    let globalReconstructionBaseCollection;

    // We build the reconstructed pipelines incrementally
    const globalReconstructedPipeline = [];
    const reconstructions = [];

    // Walk the original plan starting from the simplest sub-plan first.
    walkReverseDepth(originalPlan, (stage) => {
        if (stage.cardinalityEstimate !== undefined) {
            const {reconstructedBaseCollection, reconstructedStages} = reconstructStage(stage);
            // The collection that is returned by the first (simplest) reconstruction
            // is used as a base collection for all reconstructions going forward.
            if (reconstructedBaseCollection !== undefined) {
                if (globalReconstructionBaseCollection === undefined) {
                    globalReconstructionBaseCollection = reconstructedBaseCollection;
                    assert(reconstructedStages.length === 1);
                    assert(reconstructedStages[0].hasOwnProperty("$match"));
                    assert(globalReconstructedPipeline.length === 0);
                    globalReconstructedPipeline.push(...reconstructedStages);
                } else {
                    // We already have a base collection, so we are only interested in joins from this point onwards
                    return;
                }
            } else {
                // The reconstruction is a $lookup, tack it on to the reconstructed pipeline
                assert(reconstructedStages[0].hasOwnProperty("$lookup"));
                globalReconstructedPipeline.push(...reconstructedStages);
            }

            // A subplan is all the reconstructed stages accumulated so far
            reconstructions.push({
                cardinalityEstimate: Math.round(stage.cardinalityEstimate),
                stage: stage,
                reconstructedBaseCollection: globalReconstructionBaseCollection,
                reconstructedPipeline: [...globalReconstructedPipeline],
            });
        }
    });

    assert(reconstructions.length > 0, "No successful subjoin reconstructions were made.");

    const originalRows = db[originalBaseCollection].aggregate(originalPipeline).itcount();

    const globalReconstructionRows = db[globalReconstructionBaseCollection]
        .aggregate(globalReconstructedPipeline)
        .itcount();
    // Make sure our reconstruction matches the semantics of the original query.
    if (originalRows !== globalReconstructionRows) {
        assert(
            false,
            `Unsuccessful reconstruction -- resultset length mismatch: ${globalReconstructionRows} vs. ${originalRows}` +
                "Original plan:" +
                tojson(originalPlan) +
                "Reconstructed base collection:" +
                globalReconstructionBaseCollection +
                "Reconstructed pipeline:" +
                tojson(globalReconstructedPipeline),
        );
    }

    for (const [
        subjoin_id,
        {cardinalityEstimate: subjoinCardinalityEstimate, reconstructedPipeline, stage},
    ] of reconstructions.entries()) {
        const reconstructionActualCardinality = db[globalReconstructionBaseCollection]
            .aggregate(reconstructedPipeline)
            .itcount();

        print(`### >>> Subjoin ${command.idx}-${subjoin_id}`);

        let stageString = joinPlanToString(stage).trimEnd();
        print("```");
        print(`db.${globalReconstructionBaseCollection}.aggregate(EJSON.deserialize(`);
        print(newlineBeforeEachStage(JSON.stringify(reconstructedPipeline)));
        print(`));`);
        print("```");
        print("Subjoin plan:");
        print("```");
        print(stageString);
        print("```");

        // Avoid reporting bogus ordersOfMagnitude if either cardinality is zero
        const reconstructionActualCardinalityNormalized =
            reconstructionActualCardinality !== 0 ? reconstructionActualCardinality : 1;
        const subjoinCardinalityEstimateNormalized = subjoinCardinalityEstimate !== 0 ? subjoinCardinalityEstimate : 1;
        const ordersOfMagnitude = orderOfMagnitudeDiff(
            reconstructionActualCardinalityNormalized,
            subjoinCardinalityEstimateNormalized,
        );

        print(`Estimated cardinality: ${subjoinCardinalityEstimate}  `);
        print(`Actual cardinality: ${reconstructionActualCardinality}  `);
        print(`Orders of magnitude: ${ordersOfMagnitude}`);

        if (ordersOfMagnitude >= ORDERS_OF_MAGNITUDE_REPORTING_THRESHOLD) {
            print("> [!WARNING]");
            print(
                `> Estimate discrepancy is more than ${ORDERS_OF_MAGNITUDE_REPORTING_THRESHOLD} orders of magnitude.`,
            );
        }

        print();
        print("---");
    }
}

const tpch = populateTPCHDataset("0.1");

for (const command of commands) {
    checkCommandEstimates(tpch, command);
}

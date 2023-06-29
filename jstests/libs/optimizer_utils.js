import {getAggPlanStage, isAggregationPlan} from "jstests/libs/analyze_plan.js";

/**
 * Utility for checking if the Cascades optimizer code path is enabled (checks framework control).
 */
export function checkCascadesOptimizerEnabled(theDB) {
    const val = theDB.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1})
                    .internalQueryFrameworkControl;
    return val == "tryBonsai" || val == "tryBonsaiExperimental" || val == "forceBonsai";
}

/**
 * Utility for checking if the Cascades optimizer feature flag is on.
 */
export function checkCascadesFeatureFlagEnabled(theDB) {
    const featureFlag = theDB.adminCommand({getParameter: 1, featureFlagCommonQueryFramework: 1});
    return featureFlag.hasOwnProperty("featureFlagCommonQueryFramework") &&
        featureFlag.featureFlagCommonQueryFramework.value;
}

/**
 * Given the result of an explain command, returns whether the bonsai optimizer was used.
 */
export function usedBonsaiOptimizer(explain) {
    if (!isAggregationPlan(explain)) {
        return explain.queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    }

    const plannerOutput = getAggPlanStage(explain, "$cursor");
    if (plannerOutput != null) {
        return plannerOutput["$cursor"].queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    } else {
        return explain.queryPlanner.winningPlan.hasOwnProperty("optimizerPlan");
    }
}

/**
 * Given a query plan or explain output, follow the leftmost child until
 * we reach a leaf stage, and return it.
 *
 * This is useful for finding the access path part of a plan, typically a PhysicalScan or IndexScan.
 */
export function leftmostLeafStage(node) {
    for (;;) {
        if (node.queryPlanner) {
            node = node.queryPlanner;
        } else if (node.winningPlan) {
            node = node.winningPlan;
        } else if (node.optimizerPlan) {
            node = node.optimizerPlan;
        } else if (node.child) {
            node = node.child;
        } else if (node.leftChild) {
            node = node.leftChild;
        } else if (node.children) {
            node = node.children[0];
        } else {
            break;
        }
    }
    return node;
}

/**
 * Retrieves the cardinality estimate from a node in explain.
 */
export function extractLogicalCEFromNode(node) {
    const ce = node.properties.logicalProperties.cardinalityEstimate[0].ce;
    assert.neq(ce, null, tojson(node));
    return ce;
}

/**
 * Get a very simplified version of a plan, which only includes nodeType and nesting structure.
 */
export function getPlanSkeleton(node, options = {}) {
    const {extraKeepKeys = [], keepKeysDeep = [], printFilter = false, printLogicalCE = false} =
        options;

    const keepKeys = [
        'nodeType',

        'queryPlanner',
        'winningPlan',
        'optimizerPlan',
        'child',
        'children',
        'leftChild',
        'rightChild',
    ].concat(extraKeepKeys);

    if (Array.isArray(node)) {
        return node.map(n => getPlanSkeleton(n, options));
    } else if (node === null || typeof node !== 'object') {
        return node;
    } else {
        return Object.fromEntries(
            Object.keys(node)
                .filter(key => (keepKeys.includes(key) || keepKeysDeep.includes(key)))
                .map(key => {
                    if (key === 'interval') {
                        return [key, prettyInterval(node[key])];
                    } else if (key === 'filter' && printFilter) {
                        return [key, prettyExpression(node[key])];
                    } else if (key === "properties" && printLogicalCE) {
                        return ["logicalCE", extractLogicalCEFromNode(node)];
                    } else if (keepKeysDeep.includes(key)) {
                        return [key, node[key]];
                    } else {
                        return [key, getPlanSkeleton(node[key], options)];
                    }
                }));
    }
}

/**
 * Recur into every object and array; return any subtree that matches 'predicate'.
 * Only calls 'predicate' on objects: not arrays or scalars.
 *
 * This is completely ignorant of the structure of a query: for example if there
 * are literals match the predicate, it will also match those.
 */
export function findSubtrees(tree, predicate) {
    let result = [];
    const visit = subtree => {
        if (typeof subtree === 'object' && subtree != null) {
            if (Array.isArray(subtree)) {
                for (const child of subtree) {
                    visit(child);
                }
            } else {
                if (predicate(subtree)) {
                    result.push(subtree);
                }
                for (const key of Object.keys(subtree)) {
                    visit(subtree[key]);
                }
            }
        }
    };
    visit(tree);
    return result;
}

export function printBound(bound) {
    if (!Array.isArray(bound.bound)) {
        return [false, ""];
    }

    let result = "";
    let first = true;
    for (const element of bound.bound) {
        if (element.nodeType !== "Const") {
            return [false, ""];
        }

        result += tojson(element.value);
        if (first) {
            first = false;
        } else {
            result += " | ";
        }
    }

    return [true, result];
}

export function prettyInterval(compoundInterval) {
    // Takes an array of intervals, each one applying to one component of a compound index key.
    // Try to format it as a string.
    // If either bound is not Constant, return the original JSON unchanged.

    const lowBound = compoundInterval.lowBound;
    const highBound = compoundInterval.highBound;
    const lowInclusive = lowBound.inclusive;
    const highInclusive = highBound.inclusive;
    assert.eq(typeof lowInclusive, 'boolean');
    assert.eq(typeof highInclusive, 'boolean');

    let result = '';
    {
        const res = printBound(lowBound);
        if (!res[0]) {
            return compoundInterval;
        }
        result += lowInclusive ? '[ ' : '( ';
        result += res[1];
    }
    result += ", ";
    {
        const res = printBound(highBound);
        if (!res[0]) {
            return compoundInterval;
        }
        result += res[1];
        result += highInclusive ? ' ]' : ' )';
    }
    return result.trim();
}

export function prettyExpression(expr) {
    switch (expr.nodeType) {
        case 'Variable':
            return expr.name;
        case 'Const':
            return tojson(expr.value);
        case 'FunctionCall':
            return `${expr.name}(${expr.arguments.map(a => prettyExpression(a)).join(', ')})`;
        case 'If': {
            const if_ = prettyExpression(expr.condition);
            const then_ = prettyExpression(expr.then);
            const else_ = prettyExpression(expr.else);
            return `if ${if_} then ${then_} else ${else_}`;
        }
        case 'Let': {
            const x = expr.variable;
            const b = prettyExpression(expr.bind);
            const e = prettyExpression(expr.expression);
            return `let ${x} = ${b} in ${e}`;
        }
        case 'LambdaAbstraction': {
            return `(${expr.variable} -> ${prettyExpression(expr.input)})`;
        }
        case 'BinaryOp': {
            const left = prettyExpression(expr.left);
            const right = prettyExpression(expr.right);
            const op = prettyOp(expr.op);
            return `(${left} ${op} ${right})`;
        }
        case 'UnaryOp': {
            const op = prettyOp(expr.op);
            const input = prettyExpression(expr.input);
            return `(${op} ${input})`;
        }
        default:
            return tojson(expr);
    }
}

export function prettyOp(op) {
    // See src/mongo/db/query/optimizer/syntax/syntax.h, PATHSYNTAX_OPNAMES.
    switch (op) {
        /* comparison operations */
        case 'Eq':
            return '==';
        case 'EqMember':
            return 'in';
        case 'Neq':
            return '!=';
        case 'Gt':
            return '>';
        case 'Gte':
            return '>=';
        case 'Lt':
            return '<';
        case 'Lte':
            return '<=';
        case 'Cmp3w':
            return '<=>';

        /* binary operations */
        case 'Add':
            return '+';
        case 'Sub':
            return '-';
        case 'Mult':
            return '*';
        case 'Div':
            return '/';

        /* unary operations */
        case 'Neg':
            return '-';

        /* logical operations */
        case 'And':
            return 'and';
        case 'Or':
            return 'or';
        case 'Not':
            return 'not';

        default:
            return op;
    }
}

/**
 * Helper function to remove UUIDs of collections in the supplied database from a V1 or V2 optimizer
 * explain.
 */
export function removeUUIDsFromExplain(db, explain) {
    const listCollsRes = db.runCommand({listCollections: 1}).cursor.firstBatch;
    let plan = explain.queryPlanner.winningPlan.optimizerPlan.plan.toString();

    for (let entry of listCollsRes) {
        const uuidStr = entry.info.uuid.toString().slice(6).slice(0, -2);
        plan = plan.replaceAll(uuidStr, "");
    }
    return plan;
}

export function navigateToPath(doc, path) {
    let result;
    let field;

    try {
        result = doc;
        for (field of path.split(".")) {
            assert(result.hasOwnProperty(field));
            result = result[field];
        }
        return result;
    } catch (e) {
        jsTestLog("Error navigating to path '" + path + "'");
        jsTestLog("Missing field: " + field);
        printjson(result);
        throw e;
    }
}

export function navigateToPlanPath(doc, path) {
    return navigateToPath(doc, "queryPlanner.winningPlan.optimizerPlan." + path);
}

export function navigateToRootNode(doc) {
    return navigateToPath(doc, "queryPlanner.winningPlan.optimizerPlan");
}

export function assertValueOnPathFn(value, doc, path, fn) {
    try {
        assert.eq(value, fn(doc, path));
    } catch (e) {
        jsTestLog("Assertion error.");
        printjson(doc);
        throw e;
    }
}

export function assertValueOnPath(value, doc, path) {
    assertValueOnPathFn(value, doc, path, navigateToPath);
}

export function assertValueOnPlanPath(value, doc, path) {
    assertValueOnPathFn(value, doc, path, navigateToPlanPath);
}

export function runWithParams(keyValPairs, fn) {
    let prevVals = [];

    try {
        for (let i = 0; i < keyValPairs.length; i++) {
            const flag = keyValPairs[i].key;
            const valIn = keyValPairs[i].value;
            const val = (typeof valIn === 'object') ? JSON.stringify(valIn) : valIn;

            let getParamObj = {};
            getParamObj["getParameter"] = 1;
            getParamObj[flag] = 1;
            const prevVal = db.adminCommand(getParamObj);
            prevVals.push(prevVal[flag]);

            let setParamObj = {};
            setParamObj["setParameter"] = 1;
            setParamObj[flag] = val;
            assert.commandWorked(db.adminCommand(setParamObj));
        }

        return fn();
    } finally {
        for (let i = 0; i < keyValPairs.length; i++) {
            const flag = keyValPairs[i].key;

            let setParamObj = {};
            setParamObj["setParameter"] = 1;
            setParamObj[flag] = prevVals[i];

            assert.commandWorked(db.adminCommand(setParamObj));
        }
    }
}

export function round2(n) {
    return (Math.round(n * 100) / 100);
}

/**
 * Force cardinality estimation mode: "histogram", "heuristic", or "sampling". We need to force the
 * use of the new optimizer.
 */
export function forceCE(mode) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryCardinalityEstimatorMode: mode}));
}

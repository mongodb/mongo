import {getAggPlanStage, getQueryPlanner, isAggregationPlan} from "jstests/libs/analyze_plan.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

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
        } else if (node.queryPlan) {
            node = node.queryPlan;
        } else if (node.child) {
            node = node.child;
        } else if (node.inputStage) {
            node = node.inputStage;
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
 * Parameter 'internalPipelineLengthLimit' depends on the platform and the build type.
 */
export function getExpectedPipelineLimit(database) {
    const buildInfo = assert.commandWorked(database.adminCommand("buildInfo"));
    const isDebug = buildInfo.debug;
    const isS390X =
        "buildEnvironment" in buildInfo ? buildInfo.buildEnvironment.distarch == "s390x" : false;
    return isDebug ? 200 : (isS390X ? 700 : 1000);
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
        'queryPlan',
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
    let plan = explain.queryPlanner.winningPlan.queryPlan.plan.toString();

    for (let entry of listCollsRes) {
        const uuidStr = entry.info.uuid.toString().slice(6).slice(0, -2);
        plan = plan.replaceAll(uuidStr, "");
    }
    return plan;
}

export function navigateToPath(doc, path) {
    let result = doc;
    // Drop empty path components so we can treat '' as a 0-element path,
    // and also not worry about extra '.' when concatenating paths.
    let components = path.split(".").filter(s => s.length > 0);
    try {
        for (; components.length > 0; components = components.slice(1)) {
            assert(result.hasOwnProperty(components[0]));
            result = result[components[0]];
        }
        return result;
    } catch (e) {
        const field = components[0];
        const suffix = components.join('.');
        jsTestLog(`Error navigating to path '${path}'\n` +
                  `because the suffix '${suffix}' does not exist,\n` +
                  `because the field '${field}' does not exist in this subtree:`);
        printjson(result);
        jsTestLog("The entire tree was: ");
        printjson(doc);
        throw e;
    }
}

export function navigateToPlanPath(doc, path) {
    return navigateToPath(doc, "queryPlanner.winningPlan.queryPlan." + path);
}

export function navigateToRootNode(doc) {
    return navigateToPath(doc, "queryPlanner.winningPlan.queryPlan");
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

// TODO SERVER-84743: Consolidate the following two functions.
/**
 * This is meant to be used by standalone passthrough tests, no need to pass in the db variable.
 */
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

/**
 * This can be used for both standalone and sharded cases.
 */
export function runWithParamsAllNodes(db, keyValPairs, fn) {
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

            setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), flag, val);
        }

        return fn();
    } finally {
        for (let i = 0; i < keyValPairs.length; i++) {
            const flag = keyValPairs[i].key;

            setParameterOnAllHosts(
                DiscoverTopology.findNonConfigNodes(db.getMongo()), flag, prevVals[i]);
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

load("jstests/libs/analyze_plan.js");

/*
 * Utility for checking if the query optimizer is enabled.
 */
function checkCascadesOptimizerEnabled(theDB) {
    const param = theDB.adminCommand({getParameter: 1, featureFlagCommonQueryFramework: 1});
    return param.hasOwnProperty("featureFlagCommonQueryFramework") &&
        param.featureFlagCommonQueryFramework.value;
}

/**
 * Given the result of an explain command, returns whether the bonsai optimizer was used.
 */
function usedBonsaiOptimizer(explain) {
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
function leftmostLeafStage(node) {
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
        } else {
            break;
        }
    }
    return node;
}

/**
 * Get a very simplified version of a plan, which only includes nodeType and nesting structure.
 */
function getPlanSkeleton(node, options = {}) {
    const {extraKeepKeys = [], keepKeysDeep = []} = options;

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
                    if (keepKeysDeep.includes(key)) {
                        return [key, node[key]];
                    } else if (key === 'interval') {
                        return [key, prettyInterval(node[key])];
                    } else if (key === 'filter') {
                        return [key, prettyExpression(node[key])];
                    } else {
                        return [key, getPlanSkeleton(node[key], options)];
                    }
                }));
    }
}

function prettyInterval(compoundInterval) {
    // Takes an array of intervals, each one applying to one component of a compound index key.
    // Try to format it as a string.
    // If either bound is not Constant, return the original JSON unchanged.
    let result = '';
    for (const {lowBound, highBound} of compoundInterval) {
        if (lowBound.bound.nodeType !== 'Const' || highBound.bound.nodeType !== 'Const') {
            return compoundInterval;
        }

        const lowInclusive = lowBound.inclusive;
        const highInclusive = highBound.inclusive;
        assert.eq(typeof lowInclusive, 'boolean');
        assert.eq(typeof highInclusive, 'boolean');

        result += ' ';
        result += lowInclusive ? '[ ' : '( ';
        result += tojson(lowBound.bound.value);
        result += ', ';
        result += tojson(highBound.bound.value);
        result += highInclusive ? ' ]' : ' )';
    }
    return result.trim();
}

function prettyExpression(expr) {
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
        default:
            return tojson(expr);
    }
}

function prettyOp(op) {
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

function navigateToPath(doc, path) {
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

function navigateToPlanPath(doc, path) {
    return navigateToPath(doc, "queryPlanner.winningPlan.optimizerPlan." + path);
}

function assertValueOnPathFn(value, doc, path, fn) {
    try {
        assert.eq(value, fn(doc, path));
    } catch (e) {
        jsTestLog("Assertion error.");
        printjson(doc);
        throw e;
    }
}

function assertValueOnPath(value, doc, path) {
    assertValueOnPathFn(value, doc, path, navigateToPath);
}

function assertValueOnPlanPath(value, doc, path) {
    assertValueOnPathFn(value, doc, path, navigateToPlanPath);
}

/**
 * Pretty-printing helpers for query plans in explain.
 */
import {prettyPrintTree} from "jstests/query_golden/libs/pretty_tree.js";
import {normalizePlan, getWinningPlanFromExplain, kExplainChildFieldNames} from "jstests/libs/query/analyze_plan.js";

//
// Helpers used below to get an abbreviated join order.
//

function getStageAbbreviation(stageName) {
    switch (stageName) {
        case "HASH_JOIN_EMBEDDING":
            return "HJ";
        case "NESTED_LOOP_JOIN":
            return "NLJ";
        case "INDEX_NESTED_LOOP_JOIN":
            return "INLJ";
        default:
            return stageName;
    }
}

function formatEmbeddingField(field) {
    if (field && field !== "none") {
        return field;
    }
    return "_";
}

function abbreviate(node) {
    const abbrev = getStageAbbreviation(node.stage);
    if (abbrev == node.stage) {
        if (node.nss) {
            return `${node.stage} [${node.nss}]`;
        }
        return abbrev;
    }
    const l = formatEmbeddingField(node.leftEmbeddingField);
    const r = formatEmbeddingField(node.rightEmbeddingField);
    const children = node.inputStages.map(abbreviate);
    assert.eq(children.length, 2);
    return `(${abbrev} ${l} = ${children[0]}, ${r} = ${children[1]})`;
}

//
// End of helpers.
//

/**
 * Helper function to extract a one line join order from the input plan.
 * Useful for checking if this is a duplicate join order.
 */
export function getJoinOrderOneLine(plan) {
    const winningPlan = normalizePlan(plan, false /*shouldFlatten*/);
    const x = abbreviate(winningPlan);
    return x;
}

/**
 * Same as above, but for the winning plan in an 'explain'.
 */
export function getWinningJoinOrderOneLine(explain) {
    return getJoinOrderOneLine(getWinningPlanFromExplain(explain));
}

//
// Helpers used below to get a query plan pretty-printed as a tree.
//

function shortenField(node, fieldName) {
    return node[fieldName] ? " [" + node[fieldName] + "]" : "";
}

function printPlanNode(node) {
    // Place any fields with special formatting/ that we don't want in the default output here.
    const bannedFieldNames = ["stage", "planNodeId", "nss", "joinPredicates"].concat(kExplainChildFieldNames);

    const entries = Object.entries(node).filter(([f, _]) => !bannedFieldNames.includes(f));
    let str = `${node.stage}${shortenField(node, "nss")}${shortenField(node, "joinPredicates")}\n`;
    for (let i = 0; i < entries.length; i++) {
        const [f, v] = entries[i];
        if (f == "filter" && Object.entries(v).length == 0) {
            // Omit empty filters.
            continue;
        }
        str += `${f}: ${tojsononeline(v)}${i < entries.length - 1 ? "\n" : ""}`;
    }
    return str;
}

function getNodeChildren(node) {
    let children = [];
    for (const [field, entry] of Object.entries(node)) {
        if (kExplainChildFieldNames.includes(field)) {
            if (Array.isArray(entry)) {
                children = children.concat(entry);
            } else {
                children.push(entry);
            }
        }
    }
    return children;
}

//
// End of helpers.
//

/**
 * Pretty-prints the given plan as a tree.
 */
export function prettyPrintPlan(plan) {
    const winningPlan = normalizePlan(plan, false /*shouldFlatten*/);
    prettyPrintTree(winningPlan, printPlanNode, getNodeChildren);
}

/**
 * Same as above, but extracts the winning plan from 'explain' to print.
 */
export function prettyPrintWinningPlan(explain) {
    return prettyPrintPlan(getWinningPlanFromExplain(explain));
}

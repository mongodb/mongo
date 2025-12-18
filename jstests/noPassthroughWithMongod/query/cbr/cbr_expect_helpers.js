/**
 * Helpers for setting expectations on the cardinality estimates of query plans.
 */

import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

/* Wrapper for an explain plan to allow chained expectations
 *
 * getPlan(...)
 *     .expect("FETCH", ce.eq(10))
 *     .expect("IXSCAN", ce.eq(123));
 */
export class Stage {
    children;
    explain;

    constructor(stageExplain) {
        this.explain = stageExplain;
        const children = stageExplain.inputStage ? [stageExplain.inputStage] : stageExplain.inputStages || [];
        this.children = children.map((c) => new Stage(c));
    }

    get name() {
        return this.explain.stage;
    }

    get ce() {
        return this.explain.cardinalityEstimate;
    }

    get numKeysEstimate() {
        return this.explain.numKeysEstimate;
    }

    toString(indent = 0) {
        let current = `${" ".repeat(indent)}${this.name} CE=${this.ce}`;
        if (this.numKeysEstimate != undefined) {
            current += ` numKeysEstimate=${this.numKeysEstimate}`;
        }
        current += "\n";
        return current + this.children.map((c) => c.toString(indent + 4)).join("");
    }

    is(stageName) {
        return this.name == stageName;
    }

    hasStage(stageName) {
        return this.is(stageName) || this.children.some((s) => s.hasStage(stageName));
    }

    getStage(stageName) {
        return this.is(stageName) ? this : this.children.find((s) => s.getStage(stageName));
    }

    #expectInner(stageName, ...expectations) {
        return this.is(stageName)
            ? expectations.every((e) => e(this))
            : this.children.some((s) => s.#expectInner(stageName, ...expectations));
    }

    expect(stageName, ...expectations) {
        assert(this.hasStage(stageName), {
            msg: `Stage "${stageName}" doesn't exist in plan`,
            plan: this,
        });
        assert(this.#expectInner(stageName, ...expectations), {
            msg: `Stage "${stageName}" doesn't meet expectations: ${expectations.join(" && ")}`,
            plan: this,
        });
        return this;
    }
}

class ArithmeticExpectations {
    name;
    getter;

    constructor(name, getter) {
        this.name = name;
        this.getter = getter;
    }
    eq(expected) {
        let func = (stage) => {
            const actual = this.getter(stage);
            return expected == actual;
        };
        func.toString = () => `${this.name} == ${expected}`;
        return func;
    }

    between(expectedLow, expectedHigh) {
        let func = (stage) => {
            const actual = this.getter(stage);
            return actual >= expectedLow && actual <= expectedHigh;
        };
        func.toString = () => `${expectedLow} <= ${this.name}  <= ${expectedHigh}`;
        return func;
    }

    near(expected) {
        let func = this.between(expected * 0.9, expected * 1.1);
        func.toString = () => `${this.name} ~= ${expected}`;
        return func;
    }
}

/* Used to set an expectation on the cardinality estimate of a stage.
 * e.g.,
 *    new Stage(queryPlanner.winningPlan).expect("IXSCAN", ce.eq(10));
 */
export const ce = new ArithmeticExpectations("cardinalityEstimate", (stage) => stage.ce);

/* Used to set an expectation on the numKeysEstimate of an IXSCAN stage.
 * e.g.,
 *    new Stage(queryPlanner.winningPlan).expect("IXSCAN", ce.eq(10), numKeys.eq(10));
 */
export const numKeys = new ArithmeticExpectations("numKeysEstimate", (stage) => stage.numKeysEstimate);

/* Used to set an expectation on the key pattern of an IXSCAN.
 * e.g.,
 *    new Stage(queryPlanner.winningPlan).expect("IXSCAN", keyPattern({foo:1}));
 */
export function keyPattern(expectedObj) {
    let func = (stage) => tojson(stage.explain.keyPattern) == tojson(expectedObj);
    func.toString = () => `keyPattern == ${tojson(expectedObj)}`;
    return func;
}

/* Used to set an expectation on the filter of a stage.
 * e.g.,
 *    new Stage(queryPlanner.winningPlan).expect("IXSCAN", filter({foo:{"$eq":1}}));
 */
export function filter(expectedObj) {
    let func = (stage) => tojson(stage.explain.filter) == tojson(expectedObj);
    func.toString = () => `filter == ${tojson(expectedObj)}`;
    return func;
}

/**
 * Wrap each candidate plan (winning or rejected) in a Stage object.
 */
export function getPlans(explain) {
    const planner = getQueryPlanner(explain);
    return [planner.winningPlan].concat(planner.rejectedPlans).map((plan) => new Stage(plan));
}

/**
 * Get all plans from `explain` which use `stage`.
 * e.g.,
 *     getPlansWithStage(explain, "AND_SORTED");
 *
 * Useful for making assertions about CE for some variety of solutions,
 * even if isn't necessarily the winning plan.
 */
export function getPlansWithStage(explain, stage) {
    return getPlans(explain).filter((s) => s.hasStage(stage));
}

export default {getPlansWithStage, getPlans, ce, numKeys, filter, Stage};

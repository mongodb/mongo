/**
 * Test that verifies the execution of the combination of queries made eligible for
 * pushdown by the QEMP project. Each query is run in SBE and compared against classic.
 *
 * @tags: [
 *   # QEMP started in 9.0.
 *   requires_fcv_90,
 *   requires_sbe,
 *   # TODO(SERVER-133361): allow this to run in sharded variants.
 *   assumes_standalone_mongod,
 *   # This test assumes pipeline optimization is enabled.
 *   requires_pipeline_optimization,
 *   do_not_wrap_aggregations_in_facets,
 *   # We modify the value of a query knob. setParameter is not persistent.
 *   does_not_support_stepdowns,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {describe, it} from "jstests/libs/mochalite.js";

const books = db[jsTestName()];
const editions = db[jsTestName() + "_editions"];

books.drop();
editions.drop();

assert.commandWorked(
    books.insertMany([
        {
            _id: 0,
            title: "The C++ Programming Language",
            edition: "premium",
            altInfo: {
                title: "Le Langage de Programmation C++",
                edition: "standard",
                altInfo: {title: "A Linguagem de Programacao C++", edition: "standard"},
            },
        },
        {
            _id: 1,
            title: "Programming: Principles and Practice",
            edition: "premium",
            altInfo: {
                title: "Programmation: Principes et Pratique",
                edition: "premium",
                altInfo: {title: "Programacao: Principios e Pratica", edition: "standard"},
            },
        },
        {
            _id: 2,
            title: "The C Programming Language",
            edition: "standard",
            altInfo: {
                title: "Le Langage C",
                edition: "standard",
                altInfo: {title: "A Linguagem C", edition: "standard"},
            },
        },
        {
            _id: 3,
            title: "Object-Oriented Analysis and Design",
            edition: "standard",
            altInfo: {
                title: "Analyse et Conception Orientees Objet",
                edition: "standard",
                altInfo: {title: "Analise e Projeto Orientado a Objetos", edition: "standard"},
            },
        },
    ]),
);
assert.commandWorked(books.createIndex({edition: 1}));

assert.commandWorked(
    editions.insertMany([
        {edition: "premium", format: "hardcover"},
        {edition: "premium", format: "deluxe"},
        {edition: "standard", format: "paperback"},
        {edition: "standard", format: "ebook"},
    ]),
);

/**
 * The stages contained in STAGES are chosen so as to:
 * 1. Depend on the 'edition' field.
 * 2. Change the value of the 'edition' field.
 *
 * This enables us to build queries whose stages depend directly on the preceding stage,
 * allowing us to prevent stage reordering.
 */
const STAGES = [
    [
        {
            $lookup: {
                from: editions.getName(),
                localField: "edition",
                foreignField: "edition",
                as: "edition",
            },
        },
    ],
    [
        {
            $group: {
                _id: "$edition",
                edition: {$min: "$edition"},
                altInfo: {$min: "$altInfo"},
            },
        },
    ],
    [
        {
            $lookup: {
                from: editions.getName(),
                localField: "edition",
                foreignField: "edition",
                as: "edition",
            },
        },
        {$unwind: "$edition"},
    ],
    [{$match: {edition: {$in: ["standard", "premium"]}}}],
    [{$project: {edition: 1, altInfo: 1}}],
    [{$project: {edition: 0}}],
    [{$addFields: {edition: {$cond: [{$eq: ["$edition", "premium"]}, "standard", "premium"]}}}],
    [{$replaceRoot: {newRoot: {$cond: [{$eq: ["$edition", "premium"]}, "$altInfo", "$$ROOT"]}}}],
];

// Exclude pipelines whose pushdown rules didn't change in QEMP.
function exclude(pipeline) {
    return !pipeline.some((s) => s.hasOwnProperty("$lookup") || s.hasOwnProperty("$group"));
}

/**
 * This function generates the test cases verified later in the test.
 *
 * Each test case executes a single query. Currently we generate queries with 3 stages,
 * which are the result of the full cross products of STAGES.
 *
 * This enables us to cover all stage combinations enabled by QEMP, in addition to some
 * others.
 */
function generateTestCases() {
    const cases = [];
    for (const a of STAGES) {
        for (const b of STAGES) {
            for (const c of STAGES) {
                const pipeline = [...a, ...b, ...c];
                if (!exclude(pipeline)) {
                    cases.push(pipeline);
                }
            }
        }
    }
    return cases;
}

function pipelineToString(pipeline) {
    return '[ "' + pipeline.map((s) => Object.keys(s)[0]).join('", "') + '" ]';
}

function compareWithClassic(pipeline) {
    const sbeExplain = books.explain().aggregate(pipeline);

    // Some pipelines run in classic according to complex rules (dependency analysis,
    // plan based pushdown). Here we skip those pipelines.
    if (getEngine(sbeExplain) === "classic") {
        jsTest.log.info(`Skipping classic query: ${pipelineToString(pipeline)}`);
        return;
    }

    const saved = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
    );
    // Run on SBE (or part-SBE, part-classic).
    const engineResult = books.aggregate(pipeline).toArray();

    let classicResult;
    try {
        // Run on classic.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );
        assert.eq(getEngine(books.explain().aggregate(pipeline)), "classic", tojson(sbeExplain));
        classicResult = books.aggregate(pipeline).toArray();
    } finally {
        // Restore engine configuration.
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: saved.internalQueryFrameworkControl,
            }),
        );
    }

    // Compare results.
    jsTest.log.info("Comparing results", {engineResult, classicResult});
    assert.sameMembers(engineResult, classicResult, tojson(pipeline));
}

const testCases = generateTestCases();
jsTest.log.info("Generated test pipelines", {count: testCases.length});

describe("QEMP stage combinations", () => {
    for (const pipeline of testCases) {
        it(`pipeline: ${pipelineToString(pipeline)}`, () => {
            compareWithClassic(pipeline);
        });
    }
});

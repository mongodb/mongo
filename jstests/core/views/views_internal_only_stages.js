/**
 * Internal-only aggregation stages must be rejected in a view definition.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const viewOn = "coll";
const rejectedViewName = "rejectedView";

// On mongod the pipeline is validated on the user's connection and fails with 5491300. On a shard
// validating a create forwarded by mongos, the connection is internal and the rejection comes from
// the unconditional view-definition check.
const kRejectionCodes = [5491300, ErrorCodes.OptionNotSupportedOnView];

const kInternalStage = {
    $setVariableFromSubPipeline: {
        setVariable: "$$SEARCH_META",
        pipeline: [{$collStats: {count: {}}}],
    },
};

const kPipelines = [
    {name: "an internal-only stage at the top level", pipeline: [kInternalStage]},
    {
        name: "an internal-only stage inside a $lookup sub-pipeline",
        pipeline: [
            {
                $lookup: {from: viewOn, pipeline: [kInternalStage], as: "joined"},
            },
        ],
    },
    {
        name: "an internal-only stage inside a $unionWith sub-pipeline",
        pipeline: [
            {
                $unionWith: {coll: viewOn, pipeline: [kInternalStage]},
            },
        ],
    },
    {
        name: "an internal-only stage inside a $facet sub-pipeline",
        pipeline: [
            {
                $facet: {out: [kInternalStage]},
            },
        ],
    },
];

describe("internal-only stages in a view definition", function () {
    before(function () {
        assert.commandWorked(testDB.dropDatabase());
        assert.commandWorked(testDB[viewOn].insert({_id: 1, a: 1}));
    });

    after(function () {
        assert.commandWorked(testDB.dropDatabase());
    });

    for (const {name, pipeline} of kPipelines) {
        it(`rejects creating a view containing ${name}`, function () {
            assert.commandFailedWithCode(
                testDB.runCommand({create: rejectedViewName, viewOn, pipeline}),
                kRejectionCodes,
                `view definition containing ${name} was accepted`,
            );
            assert(
                !testDB.getCollectionNames().includes(rejectedViewName),
                `view definition containing ${name} was stored`,
                {name},
            );
        });

        it(`rejects collMod adding ${name} to an existing view`, function () {
            assert.commandWorked(
                testDB.runCommand({create: rejectedViewName, viewOn, pipeline: [{$match: {a: 1}}]}),
            );
            try {
                assert.commandFailedWithCode(
                    testDB.runCommand({collMod: rejectedViewName, viewOn, pipeline}),
                    kRejectionCodes,
                    `collMod with ${name} was accepted`,
                );
            } finally {
                assert.commandWorked(testDB.runCommand({drop: rejectedViewName}));
            }
        });
    }

    it("still accepts a plain view definition", function () {
        assert.commandWorked(
            testDB.runCommand({create: "plainView", viewOn, pipeline: [{$match: {a: 1}}]}),
        );
        assert(testDB.getCollectionNames().includes("plainView"));
    });

    it("accepts a view whose argument data is shaped like an internal stage", function () {
        // Literal data shaped like an internal stage specification is not a stage, so the view
        // must be creatable and readable.
        assert.commandWorked(
            testDB.runCommand({
                create: rejectedViewName,
                viewOn,
                pipeline: [{$project: {value: {$literal: kInternalStage}}}],
            }),
        );
        assert.eq(testDB[rejectedViewName].find().toArray().length, 1, {rejectedViewName});
        assert.commandWorked(testDB.runCommand({drop: rejectedViewName}));
    });
});

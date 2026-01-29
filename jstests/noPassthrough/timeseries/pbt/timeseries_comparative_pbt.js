/**
 * A property-based test that compares compatible command sequences between timeseries
 * and non-timeseries collections.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_timeseries,
 *   # Runs queries that may return many results, requiring getmores.
 *   requires_getmore,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

import {makeEmptyModel} from "jstests/libs/property_test_helpers/timeseries/command_grammar.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/libs/property_test_helpers/timeseries/command_arbitraries.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const tsBucketCollName = "system.buckets." + tsCollName;
const dbName = "test";

describe("Basic comparative PBT for timeseries inserts", () => {
    let tsColl;
    let ctrlColl;
    let bucketColl;

    beforeEach(() => {
        db[ctrlCollName].drop();
        db[tsCollName].drop();

        db.createCollection(ctrlCollName);
        db.createCollection(tsCollName, {timeseries: {timeField: "ts", metaField: "meta"}});

        ctrlColl = db.getCollection(ctrlCollName);
        tsColl = db.getCollection(tsCollName);
        bucketColl = db.getCollection(tsBucketCollName);
    });

    afterEach(() => {
        ctrlColl.drop();
        tsColl.drop();
    });

    after(() => {
        MongoRunner.stopMongod(conn);
    });

    function assertCollectionsMatch() {
        const tsDocs = tsColl.find().sort({_id: 1}).toArray();
        const ctrlDocs = ctrlColl.find().sort({_id: 1}).toArray();

        assert.sameMembers(ctrlDocs, tsDocs, "tsColl and ctrlColl diverged: timeseries vs control differ");
    }

    it("keeps tsColl and ctrlColl in sync under insert/batch-insert/delete", () => {
        const metaValue = "metavalu";

        const programArb = makeTimeseriesCommandSequenceArb(
            /* minCommands   */ 1,
            /* maxCommands   */ 30,
            /* timeField     */ "ts",
            /* metaField     */ "meta",
            /* metaValue     */ metaValue,
            /* minFields     */ 1,
            /* maxFields     */ 3,
            /* minDocs       */ 0,
            /* maxDocs       */ 10,
            /* ranges        */ {}, // {intRange, dateRange} if you want to override
            /* fieldNameArb  */ undefined, // use default short-string field names
        );

        fc.assert(
            fc.property(programArb, (program) => {
                const model = makeEmptyModel();
                const real = {tsColl, ctrlColl};

                for (const cmd of program) {
                    if (cmd.check(model)) {
                        cmd.run(model, real);
                    }
                }

                assertCollectionsMatch();
            }),
            {numRuns: 50},
        );
    });
});

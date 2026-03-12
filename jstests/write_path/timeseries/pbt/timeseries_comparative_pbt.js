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

import {describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const tsBucketCollName = "system.buckets." + tsCollName;
const timeField = "ts";
const metaField = "meta";

describe("Basic comparative PBT for timeseries inserts", () => {
    let tsColl;
    let ctrlColl;
    let bucketColl;

    const beforeHook = () => {
        db[ctrlCollName].drop();
        db[tsCollName].drop();

        db.createCollection(ctrlCollName);
        db.createCollection(tsCollName, {timeseries: {timeField: timeField, metaField: metaField}});

        ctrlColl = db.getCollection(ctrlCollName);
        tsColl = db.getCollection(tsCollName);
        bucketColl = db.getCollection(tsBucketCollName);
    };

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
            /* options       */ {}, // {intRange, dateRange} if you want to override
            /* fieldNameArb  */ undefined, // use default short-string field names
            /* replayPath    */ undefined, // replace this value with the replay path to replicate a failure
        );

        fc.assert(
            fc
                .property(programArb, (cmds) => {
                    const model = makeEmptyModel();
                    fc.modelRun(() => ({model: model, real: {tsColl, ctrlColl}}), cmds);
                    assertCollectionsMatch(tsColl, ctrlColl, bucketColl);
                })
                .beforeEach(beforeHook),
            {numRuns: 50},
        );
    });
});

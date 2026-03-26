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
import {getFcParams, getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const fcParams = getFcParams();
const fcAssertArgs = getFcAssertArgs();
const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";

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
        bucketColl = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl);
    };

    it("keeps tsColl and ctrlColl in sync under insert/batch-insert/delete", () => {
        const programArb = makeTimeseriesCommandSequenceArb(
            /* minCommands   */ fcParams.minCommands || 1,
            /* maxCommands   */ fcParams.maxCommands || 30,
            /* timeField     */ timeField,
            /* metaField     */ metaField,
            /* metaValue     */ metaValue,
            /* minFields     */ 1,
            /* maxFields     */ 3,
            /* minDocs       */ 0,
            /* maxDocs       */ 10,
            /* options       */ {}, // {intRange, dateRange} if you want to override
            /* fieldNameArb  */ undefined, // use default short-string field names
            /* replayPath    */ fcParams.replayPath,
        );

        fc.assert(
            fc
                .property(programArb, (cmds) => {
                    const model = makeEmptyModel(ctrlColl, bucketColl);
                    fc.modelRun(() => ({model: model, real: {tsColl, ctrlColl}}), cmds);
                    assertCollectionsMatch(tsColl, ctrlColl);
                })
                .beforeEach(beforeHook),
            fcAssertArgs,
        );
    });
});

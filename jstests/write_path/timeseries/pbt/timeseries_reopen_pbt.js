/**
 * A property-based test that exercises insertion into existing (old/closed) buckets via
 * InsertOldBucketCommand, interleaved with BatchInsertCommand to build up bucket state.
 * Stats accumulate numBucketsReopened from tsColl.stats().timeseries across all runs.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_timeseries,
 *   # Runs queries that may return many results, requiring getmores.
 *   requires_getmore,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   # This test reads collection stats.
 *   requires_collstats,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {
    makeBatchInsertCommandArb,
    makeInsertOldBucketCommandArb,
} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {assertCollectionValid, assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";
import {getFcParams, getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const fcParams = getFcParams();
const fcAssertArgs = getFcAssertArgs();

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";

describe("PBT exercising bucket reopening via InsertOldBucketCommand", () => {
    let tsColl;
    let ctrlColl;
    let bucketColl;
    let stats;

    const beforeHook = () => {
        db[ctrlCollName].drop();
        db[tsCollName].drop();

        db.createCollection(ctrlCollName);
        db.createCollection(tsCollName, {timeseries: {timeField, metaField}});

        ctrlColl = db.getCollection(ctrlCollName);
        tsColl = db.getCollection(tsCollName);
        bucketColl = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl);
    };

    beforeEach(function () {
        stats = {
            commands: {},
            numBucketsReopened: 0,
        };
    });

    afterEach(function () {
        jsTest.log.info({"Bucket reopening PBT stats": stats});
    });

    it("keeps tsColl and ctrlColl in sync and records bucket reopenings", () => {
        const batchInsertArb = makeBatchInsertCommandArb(
            timeField,
            metaField,
            metaValue,
            /* minFields */ 1,
            /* maxFields */ 3,
            /* minDocs   */ 1,
            /* maxDocs   */ 20,
        );

        const insertOldBucketArb = makeInsertOldBucketCommandArb(timeField, metaField);

        // Bias 5:1 towards batch inserts so buckets accumulate before reopening attempts.
        const programArb = fc.commands(
            [batchInsertArb, batchInsertArb, batchInsertArb, batchInsertArb, batchInsertArb, insertOldBucketArb],
            {
                maxCommands: fcParams.maxCommands || 100, // maxCommands
            },
        );

        fc.assert(
            fc
                .property(programArb, (cmds) => {
                    const model = makeEmptyModel(ctrlColl, bucketColl);
                    fc.modelRun(() => ({model, real: {tsColl, ctrlColl}}), cmds);
                    assertCollectionsMatch(tsColl, ctrlColl);
                    assertCollectionValid(tsColl);

                    // Accumulate command type counts.
                    for (const cmd of cmds.commands) {
                        const name = cmd.cmd.constructor.name;
                        stats.commands[name] = (stats.commands[name] || 0) + 1;
                    }

                    // Accumulate numBucketsReopened from this run's collection stats.
                    const collStats = assert.commandWorked(tsColl.stats());
                    if (collStats.timeseries) {
                        stats.numBucketsReopened += collStats.timeseries.numBucketsReopened ?? 0;
                    }
                })
                .beforeEach(beforeHook),
            fcAssertArgs,
        );
    });
});

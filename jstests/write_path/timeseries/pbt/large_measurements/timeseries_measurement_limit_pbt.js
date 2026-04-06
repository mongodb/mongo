/**
 * A property-based test that verifies timeseries and non-timeseries collections stay in sync
 * under inserts with measurements that approach the 4MB per-measurement size limit.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_timeseries,
 *   # Runs queries that may return many results, requiring getmores.
 *   requires_getmore,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   requires_collstats,
 * ]
 */

import {makeLargeStringArb} from "jstests/write_path/timeseries/pbt/lib/large_measurement_arbitraries.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {getFcParams} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";

import {Fixture} from "jstests/write_path/timeseries/pbt/large_measurements/generate_pbt_fixture.js";

const fcParams = getFcParams();

const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";
const ctrlCollName = jsTestName() + "_ctrl";
const tsCollName = jsTestName() + "_timeseries";

const fixture = new Fixture(db, ctrlCollName, tsCollName, metaField, timeField);

const dataSize = 4 * 1024 * 1024;
const largeStringArbFactory = () => {
    return makeLargeStringArb(dataSize);
};
const programArb = makeTimeseriesCommandSequenceArb(
    /* minCommands   */ 1,
    /* maxCommands   */ 30,
    /* timeField     */ timeField,
    /* metaField     */ metaField,
    /* metaValue     */ metaValue,
    /* minFields     */ 0,
    /* maxFields     */ 0,
    /* minDocs       */ 1,
    /* maxDocs       */ 10,
    /* options       */ {explicitArbitraries: {data: largeStringArbFactory}}, // {intRange, dateRange} if you want to override
    /* fieldNameArb  */ undefined, // use default short-string field names
    /* replayPath    */ fcParams.replayPath, // replace this value with the replay path to replicate a failure
);

fixture.run(programArb, "keeps tsColl and ctrlColl in sync under inserts that approach the Measurement 4MB limit");

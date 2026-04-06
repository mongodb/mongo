/**
 * A property-based test that verifies timeseries and non-timeseries collections stay in sync
 * under inserts with measurements that approach rollover limits in timeseries.
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

// Pack the documents into the same range based on granularity
const minDate = new Date("2026-03-25T00:00:00Z");
let maxDate = new Date(minDate);
maxDate.setHours(minDate.getHours() + 1);

const dataSize = 125 * 1024; // 125kB limit
const largeStringArbFactory = () => {
    return makeLargeStringArb(dataSize);
};
const programArb = makeTimeseriesCommandSequenceArb(
    fcParams.minCommands || 1,
    fcParams.maxCommands || 30,
    timeField,
    metaField,
    metaValue,
    /* minFields     */ 0, // use explicit arbitraries for precise size
    /* maxFields     */ 0,
    /* minDocs       */ 10,
    /* maxDocs       */ 1000, // 125 documents will hit the 16MB BSON limit
    /* options       */ {explicitArbitraries: {data: largeStringArbFactory}, dateRange: {min: minDate, max: maxDate}},
    /* fieldNameArb  */ undefined, // use default short-string field names
    /* replayPath    */ fcParams.replayPath, // replace this value with the replay path to replicate a failure
);

fixture.run(
    programArb,
    "keeps tsColl and ctrlColl in sync with large batch inserts that may reach rollover and/or BSON size limit",
);

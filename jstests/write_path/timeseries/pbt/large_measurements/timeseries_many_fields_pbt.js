/**
 * A property-based test that verifies timeseries and non-timeseries collections stay in sync
 * under inserts with documents containing a large number of numeric fields.
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

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

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

const minDate = new Date("2026-03-25T00:00:00Z");
let maxDate = new Date(minDate);
maxDate.setHours(minDate.getHours() + 1);

// Max measurement size divided by mean size of any measurement arbitrary, then truncated ~246,000
const maxMeasurementSize = 4 * 1024 * 1024;
const meanArbitrarySize = (8 + 16 + 4 + 8) / 4;
const fieldNameSize = 8;
let maxFields = Math.trunc(maxMeasurementSize / (meanArbitrarySize + fieldNameSize));

let counter = 0;
const uniquePaddedHexFieldNameArb = fc.constant(null).map(() => (counter++).toString(16).padStart(fieldNameSize, "0"));
const programArb = makeTimeseriesCommandSequenceArb(
    /* minCommands   */ 1,
    /* maxCommands   */ 30,
    /* timeField     */ timeField,
    /* metaField     */ metaField,
    /* metaValue     */ metaValue,
    /* minFields     */ 1,
    /* maxFields     */ maxFields,
    /* minDocs       */ 1,
    /* maxDocs       */ 100,
    /* options       */ {
        types: ["double", "decimal", "int", "long"],
        dateRange: {min: minDate, max: maxDate},
    },
    /* fieldNameArb  */ uniquePaddedHexFieldNameArb, // monotonically increasing field to be unique, for convenience, simply use padded hex
    /* replayPath    */ fcParams.replayPath, // replace this value with the replay path to replicate a failure
);

fixture.run(programArb, "keeps tsColl and ctrlColl in sync under batch inserts that have many fields");

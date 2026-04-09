/**
 * A property-based test that exercises geodata sync in timeseries collections.
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

import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {makeGeoArbFactory, makeGeoPointArb} from "jstests/write_path/timeseries/pbt/geodata/geodata_arbitraries.js";
import {getFcParams} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";

import {Fixture} from "jstests/write_path/timeseries/pbt/geodata/generate_pbt_fixture.js";

const fcParams = getFcParams();

const geoField = "loc";
const timeField = "ts";
const metaField = "meta";
const metaValue = "geospatial";
const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";

const fixture = new Fixture(db, ctrlCollName, tsCollName, metaField, timeField, geoField);

const programArb = makeTimeseriesCommandSequenceArb(
    /* minCommands   */ fcParams.minCommands || 1,
    /* maxCommands   */ fcParams.maxCommands || 30,
    /* timeField     */ timeField,
    /* metaField     */ metaField,
    /* metaValue     */ metaValue,
    /* minFields     */ 1,
    /* maxFields     */ 1,
    /* minDocs       */ 0,
    /* maxDocs       */ 100,
    /* options       */ {
        explicitArbitraries: {[geoField]: makeGeoArbFactory(makeGeoPointArb)},
    },
    /* fieldNameArb  */ undefined, // use default short-string field names
    /* replayPath    */ fcParams.replayPath,
);

fixture.run(programArb, "keeps tsColl and ctrlColl in sync under insert/batch-insert/delete of GeoPoint data");

/**
 * A property-based test that exercises geonear queries against timeseries collections.
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

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {
    makeBatchInsertCommandArb,
    makeDeleteByFilterCommandArb,
    makeDeleteByRandomIdCommandArb,
    makeInsertCommandArb,
} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {
    makeGeoArbFactory,
    makeGeoPointArb,
    makeGeospatialAggregationPipelineArb,
} from "jstests/write_path/timeseries/pbt/geodata/geodata_arbitraries.js";
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

const minDate = new Date("2026-03-25T00:00:00Z");
let maxDate = new Date(minDate);
maxDate.setHours(minDate.getHours() + 1);

const insertOptions = {
    explicitArbitraries: {
        [geoField]: makeGeoArbFactory(makeGeoPointArb, {
            longitudeRange: {min: -5.0, max: 5.0},
            latitudeRange: {min: -5.0, max: 5.0},
        }),
    },
    dateRange: {min: minDate, max: maxDate},
};
const [minFields, maxFields, minDocs, maxDocs] = [1, 3, 0, 100];
const insertArb = makeInsertCommandArb(timeField, metaField, metaValue, minFields, maxFields, insertOptions);
const batchInsertArb = makeBatchInsertCommandArb(
    timeField,
    metaField,
    metaValue,
    minFields,
    maxFields,
    minDocs,
    maxDocs,
    insertOptions,
);
const deleteArb = makeDeleteByRandomIdCommandArb();
const programArb = fc.commands(
    [insertArb, batchInsertArb, deleteArb],
    fcParams.maxCommands || 200,
    fcParams.replayPath,
);

const maxDistanceMeters = 10_000;
const aggregationCountRange = {minLength: 5, maxLength: 60};
const aggregationsArb = fc.array(
    makeGeospatialAggregationPipelineArb(geoField, maxDistanceMeters),
    aggregationCountRange,
);

fixture.run(
    programArb,
    "produces equal $geoNear aggregations across Insert, BatchInsert, and Delete Commands",
    aggregationsArb,
);

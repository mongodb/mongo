/**
 * A property-based test that exercises geodata in timeseries collection.
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

import {beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {
    makeGeoArbFactory,
    makeGeoPointArb,
    makeGeospatialQueryArb,
} from "jstests/write_path/timeseries/pbt/lib/geodata_arbitraries.js";
import {assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const tsBucketCollName = "system.buckets." + tsCollName;

const geoField = "loc";
const metaValue = "geospatial";

describe("Geospatial Query Comparative Test for Timeseries", () => {
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

        // This test needs to create 2dsphere indexes to properly exercise the timeseries write path.
        ctrlColl.createIndex({[geoField]: "2dsphere"});
        tsColl.createIndex({[geoField]: "2dsphere"});
    });

    it("keeps tsColl and ctrlColl in sync under insert/batch-insert/delete of GeoPoint data", () => {
        const programArb = makeTimeseriesCommandSequenceArb(
            /* minCommands   */ 1,
            /* maxCommands   */ 30,
            /* timeField     */ "ts",
            /* metaField     */ "meta",
            /* metaValue     */ metaValue,
            /* minFields     */ 1,
            /* maxFields     */ 1,
            /* minDocs       */ 0,
            /* maxDocs       */ 10,
            /* options       */ {
                explicitArbitraries: {[geoField]: makeGeoArbFactory(makeGeoPointArb)},
            },
            /* fieldNameArb  */ undefined, // use default short-string field names
        );

        fc.assert(
            fc.property(programArb, (cmds) => {
                const model = makeEmptyModel();
                fc.modelRun(() => ({model: model, real: {tsColl, ctrlColl}}), cmds);
                assertCollectionsMatch(tsColl, ctrlColl, bucketColl);
            }),
            {numRuns: 50},
        );
    });

    it("produces equal geonear queries", () => {
        const programArb = makeTimeseriesCommandSequenceArb(
            /* minCommands   */ 1,
            /* maxCommands   */ 30,
            /* timeField     */ "ts",
            /* metaField     */ "meta",
            /* metaValue     */ metaValue,
            /* minFields     */ 1,
            /* maxFields     */ 1,
            /* minDocs       */ 0,
            /* maxDocs       */ 10,
            /* options       */ {
                explicitArbitraries: {[geoField]: makeGeoArbFactory(makeGeoPointArb)},
            },
            /* fieldNameArb  */ undefined, // use default short-string field names
        );

        fc.assert(
            fc.property(
                programArb,
                fc.array(makeGeospatialQueryArb(geoField, 10000), {minLength: 1, maxLength: 40}),
                (cmds, queries) => {
                    const model = makeEmptyModel();
                    fc.modelRun(() => ({model: model, real: {tsColl, ctrlColl}}), cmds);
                    for (const query of queries) {
                        assertCollectionsMatch(tsColl, ctrlColl, bucketColl, query);
                    }
                },
            ),
            {numRuns: 50},
        );
    });
});

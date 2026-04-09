/**
 * Leverage mochalite to generate a describe block with necessary hooks for geodata PBTs.
 * This is needed to split up long-running test cases into separate files to minimize evergreen timeout.
 */

import {describe, it} from "jstests/libs/mochalite.js";

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";
import {getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const fcAssertArgs = getFcAssertArgs();

/**
 * The Fixture class maintains the functions for Mochalite and fast-check lifecycle hooks for
 * geodata PBTs. Specific test case files should set up the fixture and run it by passing a
 * programArbitrary as an argument to `run`.
 */
export class Fixture {
    constructor(db, ctrlCollName, tsCollName, metaField, timeField, geoField) {
        this.db = db;
        this.ctrlCollName = ctrlCollName;
        this.tsCollName = tsCollName;
        this.metaField = metaField;
        this.timeField = timeField;
        this.geoField = geoField;
        this.beforeHook();
    }

    // Create fresh collections for the property testing.
    beforeHook() {
        this.db[this.ctrlCollName].drop();
        this.db[this.tsCollName].drop();

        this.db.createCollection(this.ctrlCollName);
        this.db.createCollection(this.tsCollName, {timeseries: {timeField: this.timeField, metaField: this.metaField}});

        this.ctrlColl = this.db.getCollection(this.ctrlCollName);
        this.tsColl = this.db.getCollection(this.tsCollName);
        this.bucketColl = getTimeseriesCollForRawOps(this.tsColl.getDB(), this.tsColl);

        this.ctrlColl.createIndex({[this.geoField]: "2dsphere", [this.metaField]: 1});
        this.tsColl.createIndex({[this.geoField]: "2dsphere", [this.metaField]: 1});
    }

    run(programArb, descriptor, aggregationsArb = fc.constant([[]])) {
        describe("Geospatial Comparative PBT for timeseries collections", () => {
            it(descriptor, () => {
                fc.assert(
                    fc
                        .property(programArb, aggregationsArb, (cmds, pipelines) => {
                            const model = makeEmptyModel(this.ctrlColl, this.bucketColl);
                            fc.modelRun(
                                () => ({model: model, real: {tsColl: this.tsColl, ctrlColl: this.ctrlColl}}),
                                cmds,
                            );
                            for (const pipeline of pipelines) {
                                assertCollectionsMatch(this.tsColl, this.ctrlColl, pipeline);
                            }
                            // Next check every point in the collection and ensure that it can be queried using the index
                            const geoPoints = this.ctrlColl
                                .aggregate([
                                    {$group: {_id: null, locs: {$push: `$${this.geoField}`}}},
                                    {$unwind: "$locs"},
                                    {$replaceRoot: {newRoot: "$locs"}},
                                ])
                                .toArray();
                            for (const geoPoint of geoPoints) {
                                const intersectPipeline = [
                                    {$match: {[this.geoField]: {$geoIntersects: {$geometry: geoPoint}}}},
                                ];
                                assert.gt(this.tsColl.aggregate(intersectPipeline).toArray().length, 0, {
                                    error: "Point not found in aggregation",
                                    geoPoint,
                                });
                            }
                        })
                        .beforeEach(() => this.beforeHook()),
                    fcAssertArgs,
                );
            });
        });
    }
}

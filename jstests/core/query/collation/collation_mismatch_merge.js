/**
 * Tests $merge between clustered/non-clustered collections with different collations.
 * For now, only run in an unsharded environment.
 * TODO SERVER-117493 handle the sharding case
 *
 * @tags: [
 *     # Prevent sharding passthroughs for now.
 *     assumes_against_mongod_not_mongos,
 *     # Prevent clustered collection passthrough, since this test verifies this manually.
 *     expects_explicit_underscore_id_index,
 *     # Timeseries collections cannot have unique indexes.
 *     exclude_from_timeseries_crud_passthrough,
 *     # An identity view over a non-simple collation collection has a simple collation.
 *     incompatible_with_views
 * ]
 */

import {withEachMergeMode} from "jstests/aggregation/extras/merge_helpers.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);

const collations = [
    {name: "default", collation: null},
    {name: "caseInsensitive", collation: {locale: "en", strength: 2}},
];
const clustered = [
    {name: "nonclustered", clustered: false},
    {name: "clustered", clustered: true},
];

try {
    for (let srcCollation of collations) {
        for (let srcClustered of clustered) {
            for (let dstCollation of collations) {
                for (let dstClustered of clustered) {
                    jsTest.log.info(
                        `${srcClustered.name}_${srcCollation.name} -> ${dstClustered.name}_${dstCollation.name}`,
                    );
                    const src = testDB[`src_${srcClustered.name}_${srcCollation.name}`];
                    const dst = testDB[`dst_${dstClustered.name}_${dstCollation.name}`];
                    src.drop();
                    dst.drop();

                    let srcCmd = {create: src.getName()};
                    let dstCmd = {create: dst.getName()};
                    if (srcClustered.clustered) srcCmd.clusteredIndex = {key: {_id: 1}, unique: true};
                    if (dstClustered.clustered) dstCmd.clusteredIndex = {key: {_id: 1}, unique: true};
                    if (srcCollation.collation) srcCmd.collation = srcCollation.collation;
                    if (dstCollation.collation) dstCmd.collation = dstCollation.collation;
                    assert.commandWorked(testDB.runCommand(srcCmd));
                    assert.commandWorked(testDB.runCommand(dstCmd));

                    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
                        const mergeRes = testDB.runCommand({
                            aggregate: src.getName(),
                            pipeline: [
                                {
                                    $merge: {
                                        into: dst.getName(),
                                        whenMatched: whenMatchedMode,
                                        whenNotMatched: whenNotMatchedMode,
                                    },
                                },
                            ],
                            cursor: {},
                        });
                        if (srcCollation.name === dstCollation.name) {
                            assert.commandWorked(mergeRes);
                        } else {
                            assert.commandFailedWithCode(mergeRes, 51183);
                        }
                    });

                    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
                        const mergeRes = testDB.runCommand({
                            aggregate: src.getName(),
                            pipeline: [
                                {
                                    $merge: {
                                        into: dst.getName(),
                                        on: ["_id", "DNE"],
                                        whenMatched: whenMatchedMode,
                                        whenNotMatched: whenNotMatchedMode,
                                    },
                                },
                            ],
                            cursor: {},
                        });
                        assert.commandFailedWithCode(mergeRes, 51183);
                    });
                }
            }
        }
    }
} finally {
    testDB.dropDatabase();
}

/**
 * Verifies that BinData $convert behaves correctly in FCV upgrade/downgrade scenarios.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    testPerformUpgradeDowngradeReplSet
} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeDowngradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const collectionName = "coll";

const toIntViewName = "toIntView";
const toIntPipeline = [{$project: {intFromBindata: {$toInt: "$asBinData"}}}];

const toLongViewName = "toLongView";
const toLongPipeline = [{$project: {longFromBindata: {$toLong: "$asBinData"}}}];

const toDoubleViewName = "toDoubleView";
const toDoublePipeline = [{$project: {doubleFromBindata: {$toDouble: "$asBinData"}}}];

const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

function setupCollection(primaryConnection, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConnection), collectionName);

    if (shardingTest) {
        shardingTest.shardColl(coll, {asBinData: 1}, false);
    }

    assert.commandWorked(coll.insertMany([
        {
            _id: 0,
            asInt: NumberInt(42),
            asBinData: BinData(0, "AAAAKg=="),
        },
        {
            _id: 1,
            asLong: NumberLong(674),
            asBinData: BinData(0, "ogIAAA=="),
        },
        {
            _id: 2,
            asDouble: 13.199999809265137,
            asBinData: BinData(0, "QVMzMw=="),
        },
    ]));
}

function assertViewCanBeCreatedButNotExecuted(primaryConnection) {
    const db = getDB(primaryConnection);

    // View creation succeeds, but queries on the views fail due to unsupported bindata-to-int,
    // bindata-to-double and bindata-to-long conversions.
    db[toIntViewName].drop();
    assert.commandWorked(db.createView(toIntViewName, collectionName, toIntPipeline));
    assert.commandFailedWithCode(db.runCommand({find: toIntViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);

    db[toLongViewName].drop();
    assert.commandWorked(db.createView(toLongViewName, collectionName, toLongPipeline));
    assert.commandFailedWithCode(db.runCommand({find: toLongViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);

    db[toDoubleViewName].drop();
    assert.commandWorked(db.createView(toDoubleViewName, collectionName, toDoublePipeline));
    assert.commandFailedWithCode(db.runCommand({find: toDoubleViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);
}

function assertViewCanBeCreatedAndExecuted(primaryConnection) {
    const db = getDB(primaryConnection);

    db[toIntViewName].drop();
    assert.commandWorked(db.createView(toIntViewName, collectionName, toIntPipeline));
    assert.commandWorked(db.runCommand({find: toIntViewName, filter: {}}));

    db[toLongViewName].drop();
    assert.commandWorked(db.createView(toLongViewName, collectionName, toLongPipeline));
    assert.commandWorked(db.runCommand({find: toLongViewName, filter: {}}));

    db[toDoubleViewName].drop();
    assert.commandWorked(db.createView(toDoubleViewName, collectionName, toDoublePipeline));
    assert.commandWorked(db.runCommand({find: toDoubleViewName, filter: {}}));
}

function assertQueriesOnViewsFail(primaryConnection) {
    const db = getDB(primaryConnection);

    // Queries on views using BinData $convert numeric should fail after downgrading the FCV.
    assert.commandFailedWithCode(db.runCommand({find: toIntViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);
    assert.commandFailedWithCode(db.runCommand({find: toLongViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);
    assert.commandFailedWithCode(db.runCommand({find: toDoubleViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);

    // BinData to int / long / double conversion still succeeds with onError value.
    assert.commandWorked(db.runCommand({
        aggregate: collectionName,
        cursor: {},
        pipeline: [{
            $project: {
                intFromBindata: {
                    $convert: {
                        input: "$asBinData",
                        to: "int",
                        onError: "NULL",
                    }
                }
            }
        }]
    }));

    assert.commandWorked(db.runCommand({
        aggregate: collectionName,
        cursor: {},
        pipeline: [{
            $project: {
                longFromBindata: {
                    $convert: {
                        input: "$asBinData",
                        to: "long",
                        onError: "NULL",
                    }
                }
            }
        }]
    }));

    assert.commandWorked(db.runCommand({
        aggregate: collectionName,
        cursor: {},
        pipeline: [{
            $project: {
                doubleFromBindata: {
                    $convert: {
                        input: "$asBinData",
                        to: "double",
                        onError: "NULL",
                    }
                }
            }
        }]
    }));

    // However, they should not succeed with a 'byteOrder' argument.
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    stringFromUuid: {
                        $convert: {
                            input: "$asBinData",
                            to: "int",
                            byteOrder: "little",
                            onError: "NULL",
                        }
                    }
                }
            }]
        }),
        ErrorCodes.FailedToParse,
    );

    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    longFromBinData: {
                        $convert: {
                            input: "$asBinData",
                            to: "long",
                            byteOrder: "big",
                            onError: "NULL",
                        }
                    }
                }
            }]
        }),
        ErrorCodes.FailedToParse,
    );

    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    doubleFromBinData: {
                        $convert: {
                            input: "$asBinData",
                            to: "double",
                            byteOrder: "big",
                            onError: "NULL",
                        }
                    }
                }
            }]
        }),
        ErrorCodes.FailedToParse,
    );

    // Int, long and double to BinData should fail
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    BinDataFromInt: {
                        $convert: {
                            input: "$asInt",
                            to: "binData",
                        }
                    }
                }
            }]
        }),
        ErrorCodes.ConversionFailure,
    );

    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    BinDataFromInt:
                        {$convert: {input: "$asLong", to: "binData", byteOrder: "little"}}
                }
            }]
        }),
        ErrorCodes.FailedToParse,
    );

    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    BinDataFromDouble:
                        {$convert: {input: "$asDouble", to: "binData", byteOrder: "little"}}
                }
            }]
        }),
        ErrorCodes.FailedToParse,
    );
}

testPerformUpgradeDowngradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertViewCanBeCreatedButNotExecuted,
    whenSecondariesAreLatestBinary: assertViewCanBeCreatedButNotExecuted,
    whenBinariesAreLatestAndFCVIsLastLTS: assertQueriesOnViewsFail,
    whenFullyUpgraded: assertViewCanBeCreatedAndExecuted,
});

testPerformUpgradeDowngradeSharded({
    setupFn: setupCollection,
    whenFullyDowngraded: assertViewCanBeCreatedButNotExecuted,
    whenOnlyConfigIsLatestBinary: assertViewCanBeCreatedButNotExecuted,
    whenSecondariesAndConfigAreLatestBinary: assertViewCanBeCreatedButNotExecuted,
    whenMongosBinaryIsLastLTS: assertQueriesOnViewsFail,
    whenBinariesAreLatestAndFCVIsLastLTS: assertQueriesOnViewsFail,
    whenFullyUpgraded: assertViewCanBeCreatedAndExecuted,
});

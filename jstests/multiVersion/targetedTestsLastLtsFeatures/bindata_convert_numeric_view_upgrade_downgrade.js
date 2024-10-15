/**
 * Verifies that BinData $convert behaves correctly in FCV upgrade/downgrade scenarios.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const lastLTSVersion = {
    binVersion: "last-lts"
};
const latestVersion = {
    binVersion: "latest"
};

const collectionName = "coll";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [
        {...lastLTSVersion},
        {...lastLTSVersion},
    ],
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const getAdminDB = () => rst.getPrimary().getDB("admin");
const getDB = () => rst.getPrimary().getDB(jsTestName());

const coll = assertDropAndRecreateCollection(getDB(), collectionName);
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

const toIntViewName = "toIntView";
const toIntPipeline = [{$project: {intFromBindata: {$toInt: "$asBinData"}}}];

const toLongViewName = "toLongView";
const toLongPipeline = [{$project: {longFromBindata: {$toLong: "$asBinData"}}}];

const toDoubleViewName = "toDoubleView";
const toDoublePipeline = [{$project: {doubleFromBindata: {$toDouble: "$asBinData"}}}];

{
    const db = getDB();

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

// Upgrade the binaries and the FCV.
rst.upgradeSet({...latestVersion});
assert.commandWorked(
    getAdminDB().runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

{
    // View creation and usage both succeed after upgrade.
    const db = getDB();

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

// Downgrade FCV without restarting.
assert.commandWorked(
    getAdminDB().runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

{
    const db = getDB();

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

rst.stopSet();

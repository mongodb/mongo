/**
 * Verifies that BinData $convert behaves correctly in FCV upgrade/downgrade scenarios.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

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
rst.initiate();

const getAdminDB = () => rst.getPrimary().getDB("admin");
const getDB = () => rst.getPrimary().getDB(jsTestName());

const coll = assertDropAndRecreateCollection(getDB(), collectionName);
assert.commandWorked(coll.insertMany([
    {
        _id: 0,
        asUuid: UUID("867dee52-c331-484e-92d1-c56479b8e67e"),
        asString: "867dee52-c331-484e-92d1-c56479b8e67e",
    },
    {
        _id: 1,
        asUuid: UUID("b6684187-f74d-4872-818f-7a6e97fd5c04"),
        asString: "b6684187-f74d-4872-818f-7a6e97fd5c04",
    },
]));

const toUUIDViewName = "toUUIDView";
const toUUIDPipeline = [{$project: {uuidFromString: {$toUUID: "$asString"}}}];

const toStringViewName = "toStringView";
const toStringPipeline = [{$project: {stringFromUuid: {$toString: "$asUuid"}}}];

{
    const db = getDB();

    // View creation fails due to unknown $toUUID syntax.
    db[toUUIDViewName].drop();
    assert.commandFailedWithCode(db.createView(toUUIDViewName, collectionName, toUUIDPipeline),
                                 31325);

    // View creation succeeds, but queries on the view fail due to unsupported bindata-to-string
    // conversion.
    db[toStringViewName].drop();
    assert.commandWorked(db.createView(toStringViewName, collectionName, toStringPipeline));
    assert.commandFailedWithCode(db.runCommand({find: toStringViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);
}

// Upgrade the binaries and the FCV.
rst.upgradeSet({...latestVersion});
assert.commandWorked(
    getAdminDB().runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

{
    // View creation and usage both succeed after upgrade.
    const db = getDB();

    db[toUUIDViewName].drop();
    assert.commandWorked(db.createView(toUUIDViewName, collectionName, toUUIDPipeline));
    assert.commandWorked(db.runCommand({find: toUUIDViewName, filter: {}}));

    db[toStringViewName].drop();
    assert.commandWorked(db.createView(toStringViewName, collectionName, toStringPipeline));
    assert.commandWorked(db.runCommand({find: toStringViewName, filter: {}}));
}

// Downgrade FCV without restarting.
assert.commandWorked(
    getAdminDB().runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

{
    const db = getDB();

    // Queries on views using BinData $convert should fail after downgrading the FCV.
    assert.commandFailedWithCode(db.runCommand({find: toUUIDViewName, filter: {}}),
                                 ErrorCodes.QueryFeatureNotAllowed);
    assert.commandFailedWithCode(db.runCommand({find: toStringViewName, filter: {}}),
                                 ErrorCodes.ConversionFailure);

    // BinData to string conversion still succeeds with onError value.
    assert.commandWorked(db.runCommand({
        aggregate: collectionName,
        cursor: {},
        pipeline: [{
            $project: {
                stringFromUuid: {
                    $convert: {
                        input: "$asUuid",
                        to: "string",
                        onError: "NULL",
                    }
                }
            }
        }]
    }));

    // However, it should not succeed with a 'format' argument.
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{
                $project: {
                    stringFromUuid: {
                        $convert: {
                            input: "$asUuid",
                            to: "string",
                            format: "uuid",
                            onError: "NULL",
                        }
                    }
                }
            }]
        }),
        ErrorCodes.FailedToParse,
    );
}

rst.stopSet();

// Test writing to a foreign db using $out/$merge when the pipeline prefix reads from a foreign
// collection. The foreign collection and $out/$merge targets are collections of the same name but
// in different databases so we verify that the $lookup reads from the correct collection.
//
// @tags: [
//   # Serverless does not support $out
//   command_not_supported_in_serverless,
// ]

const localDB = db.getSiblingDB(`${jsTestName()}` +
                                "_local_db");
const outputDBName = `${jsTestName()}` +
    "_output_db";
const outputDB = db.getSiblingDB(outputDBName);
const uninvolvedDB = db.getSiblingDB(`${jsTestName()}` +
                                     "_uninvolved_db");

assert.commandWorked(localDB.dropDatabase());
assert.commandWorked(outputDB.dropDatabase());
assert.commandWorked(uninvolvedDB.dropDatabase());

// The name of the collection or view that the pipeline will be run against.
const localTargetName = "foo";
// The name of the collection or view that the $lookup will read from and the name of the output
// collection that the $merge will write to.
const foreignTargetName = "bar";

function runTest({
    localInput,
    foreignInput,
    foreignInput_uninvolvedDB,
    expectedOutput,
    pipeline,
    localTargetIsView,
    foreignTargetIsView,
}) {
    // Create variables for the local collection to insert documents into and the local target
    // to run the pipeline against. Also create a view named localTargetName is localCollIsView
    // is true.
    let localColl = localDB[localTargetName];
    let localTarget = localColl;
    if (localTargetIsView) {
        localColl = localDB[localTargetName + "_coll"];
        assert.commandWorked(
            localDB.createView(localTargetName, localColl.getName(), [] /* identity view */));
        localTarget = localDB[localTargetName];
    }

    // Create variables for the foreign collection to insert documents into and the foreign
    // target to run the pipeline against. Also create a view named foreignTargetName is
    // foreignCollIsView is true.
    let foreignColl = localDB[foreignTargetName];
    let foreignColl_uninvolvedDB = uninvolvedDB[foreignTargetName];
    if (foreignTargetIsView) {
        foreignColl = localDB[foreignTargetName + "_coll"];
        assert.commandWorked(
            localDB.createView(foreignTargetName, foreignColl.getName(), [] /* identity view */));

        foreignColl_uninvolvedDB = uninvolvedDB[foreignTargetName + "_coll"];
        assert.commandWorked(uninvolvedDB.createView(
            foreignTargetName, foreignColl_uninvolvedDB.getName(), [] /* identity view */));
    }

    // Populate the local collection.
    assert.commandWorked(localColl.insert(localInput));

    // Populate the foreign collections.
    assert.commandWorked(foreignColl.insert(foreignInput));
    assert.commandWorked(foreignColl_uninvolvedDB.insert(foreignInput_uninvolvedDB));

    // Make sure that outputDB actually exists since $merge in a sharded cluster requires the target
    // database to already exist.
    assert.commandWorked(outputDB.createCollection("uninvolvedColl"));

    // Run the pipeline against localColl which writes to outputColl.
    assert.commandWorked(
        localDB.runCommand({aggregate: localTarget.getName(), pipeline: pipeline, cursor: {}}));

    assert.sameMembers(outputDB[foreignTargetName].find().toArray(), expectedOutput);

    // Drop collections and/or views before the next iteration.
    assert.commandWorked(localDB.dropDatabase());
    assert.commandWorked(outputDB.dropDatabase());
    assert.commandWorked(uninvolvedDB.dropDatabase());
}

const localField = "_id";
const foreignField = "num";
const outputField = "_id";

const outSpec = {
    "$out": {"db": outputDB.getName(), "coll": foreignTargetName}
};
const mergeSpec = {
    "$merge": {"into": {"db": outputDB.getName(), "coll": foreignTargetName}}
};

// $lookup/$graphLookup tests
{
    const localInput = {[localField]: 1};
    const foreignInput = {[foreignField]: 1, [outputField]: 2};
    const foreignInput_uninvolvedDB = {[foreignField]: 1, [outputField]: 3};
    const expectedOutput = [{[outputField]: 2}];

    const sharedArgs = {
        localInput: localInput,
        foreignInput: foreignInput,
        foreignInput_uninvolvedDB: foreignInput_uninvolvedDB,
        expectedOutput: expectedOutput
    };

    const unwindSpec = {"$unwind": "$" + outputField};
    const projectSpec = {"$project": {[outputField]: "$" + outputField + "." + outputField}};

    // $lookup tests
    {
        const lookupSpec = {
            "$lookup": {
                "from": foreignTargetName,
                "as": outputField,
                "localField": localField,
                "foreignField": foreignField
            }
        };

        // $out tests
        {
            const pipeline = [lookupSpec, unwindSpec, projectSpec, outSpec];
            const args = {...sharedArgs, pipeline: pipeline};

            runTest({...args, localTargetIsView: false, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: false, foreignTargetIsView: true});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: true});
        }

        // $merge tests
        {
            const pipeline = [lookupSpec, unwindSpec, projectSpec, mergeSpec];
            const args = {...sharedArgs, pipeline: pipeline};

            runTest({...args, localTargetIsView: false, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: false, foreignTargetIsView: true});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: true});
        }
    }

    // $graphLookup tests
    {
        const graphLookupSpec = {
            "$graphLookup": {
                "from": foreignTargetName,
                "startWith": "$" + localField,
                "connectFromField": "does_not_exist",
                "connectToField": foreignField,
                "as": outputField
            }
        };

        // $out tests
        {
            const pipeline = [graphLookupSpec, unwindSpec, projectSpec, outSpec];
            const args = {...sharedArgs, pipeline: pipeline};

            runTest({...args, localTargetIsView: false, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: false, foreignTargetIsView: true});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: true});
        }

        // $merge tests
        {
            const pipeline = [graphLookupSpec, unwindSpec, projectSpec, mergeSpec];
            const args = {...sharedArgs, pipeline: pipeline};

            runTest({...args, localTargetIsView: false, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: false, foreignTargetIsView: true});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: false});
            runTest({...args, localTargetIsView: true, foreignTargetIsView: true});
        }
    }
}

// $unionWith tests
{
    const localInput = {[localField]: 1};
    const foreignInput = {[outputField]: 2};
    const foreignInput_uninvolvedDB = {[outputField]: 3};
    const expectedOutput = [{[localField]: 1}, {[outputField]: 2}];

    const sharedArgs = {
        localInput: localInput,
        foreignInput: foreignInput,
        foreignInput_uninvolvedDB: foreignInput_uninvolvedDB,
        expectedOutput: expectedOutput
    };

    const unionWithSpec = {
        "$unionWith": {
            "coll": foreignTargetName,
        }
    };

    // $out tests
    {
        const pipeline = [unionWithSpec, outSpec];
        const args = {...sharedArgs, pipeline: pipeline};

        runTest({...args, localTargetIsView: false, foreignTargetIsView: false});
        runTest({...args, localTargetIsView: false, foreignTargetIsView: true});
        runTest({...args, localTargetIsView: true, foreignTargetIsView: false});
        runTest({...args, localTargetIsView: true, foreignTargetIsView: true});
    }

    // $merge tests
    {
        const pipeline = [unionWithSpec, outSpec];
        const args = {...sharedArgs, pipeline: pipeline};

        runTest({...args, localTargetIsView: false, foreignTargetIsView: false});
        runTest({...args, localTargetIsView: false, foreignTargetIsView: true});
        runTest({...args, localTargetIsView: true, foreignTargetIsView: false});
        runTest({...args, localTargetIsView: true, foreignTargetIsView: true});
    }
}

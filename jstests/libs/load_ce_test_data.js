load("jstests/libs/ce_stats_utils.js");

/**
 * Analyze all fields and create statistics.
 * Create single-field indexes on the fields with indexed flag.
 */
function analyzeAndIndexEnabledFields(db, coll, fields) {
    for (const field of fields) {
        assert.commandWorked(db.runCommand({analyze: coll.getName(), key: field.fieldName}));
        if (field.indexed) {
            assert.commandWorked(coll.createIndex({[field.fieldName]: 1}));
        }
    }
}

/**
 * Load a dataset described in the 'dbMetadata' global variable.
 */
function importDataset(dbName, dataDir, dbMetadata) {
    const testDB = db.getSiblingDB(dbName);
    print("Running mongoimport\n");
    for (const collMetadata of dbMetadata) {
        const collName = collMetadata.collectionName;
        const coll = testDB[collName];
        print(`Importing ${collName}\n`);
        const restore_rc = runProgram('mongoimport',
                                      '--db',
                                      dbName,
                                      '--verbose',
                                      '--host',
                                      'localhost:20000',
                                      '--file',
                                      `${dataDir}${collName}.dat`,
                                      '--drop');
        assert.eq(restore_rc, 0);

        // Create single-field indexes and analyze each field.
        analyzeAndIndexEnabledFields(testDB, coll, collMetadata.fields);
    }
    print("Done mongorestore\n");
}

/**
 * Load a JSON dataset stored as an array of names of data files, where each file contains
 * a variable that holds an object with the properties{collName, collData}.
 * For instance:
 * ce_data_20_1 = {collName: "ce_data_20",
 *                 collData: [{"_id": 0, "uniform_int_0-1000-1": 899, ...}, ...]}
 */
function loadJSONDataset(db, dataSet, dataDir, dbMetadata) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

    for (const collMetadata of dbMetadata) {
        coll = db[collMetadata.collectionName];
        coll.drop();
    }

    for (const chunkName of dataSet) {
        chunkFilePath = `${dataDir}${chunkName}`;
        print(`Loading chunk file: ${chunkFilePath}\n`);
        load(chunkFilePath);
        // At this point there is a variable named as the value of chunkName.
        coll = eval(`db[${chunkName}.collName]`);
        eval(`assert.commandWorked(coll.insertMany(${chunkName}.collData, {ordered: false}));`);
        // Free the chunk memory after insertion into the DB
        eval(`${chunkName} = null`);
    }

    // TODO: This is better done by the CE-testing script because it knows better what fields to
    // analyze. Create single-field indexes and analyze each field. for (const collMetadata of
    // dbMetadata) {
    //     print(`\nIndexing collection: ${collMetadata.collectionName}`);
    //     coll = db[collMetadata.collectionName];
    //     analyzeAndIndexEnabledFields(db, coll, collMetadata.fields);
    // }
}

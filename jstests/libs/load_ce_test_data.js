load("jstests/libs/ce_stats_utils.js");

/**
 * Create single-field indexes on the fields with indexed flag.
 */
function indexEnabledFields(coll, fields) {
    for (const field of fields) {
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

        // Create single-field indexes
        indexEnabledFields(coll, collMetadata.fields);

        // TODO: Create compound indexes. I doubt we will need it for CE testing.
        // for (indexFields of collMetadata.compound_indexes) {
        //}
    }
    print("Done mongorestore\n");
}

/**
 * Load a JSON dataset stored as an array of pairs of collection name, and data.
 * For instance:
 * [{collName: "physical_scan_5000", collData: [{_id: 3, field1: "some_string"}, ...]} ...]
 */
function loadJSONDataset(db, dataSet, dbMetadata) {
    for (dataElem of dataSet) {
        print(`\nInserting collection: ${dataElem.collName}`);
        coll = db[dataElem.collName];
        coll.drop();
        assert.commandWorked(coll.insertMany(dataElem.collData));
    }

    // TODO: check that each dataSet field is present in collMetadata.

    // Create single-field indexes
    for (const collMetadata of dbMetadata) {
        coll = db[collMetadata.collectionName];
        indexEnabledFields(coll, collMetadata.fields);
    }
}

'use strict';

/**
 * create_database.js
 *
 * Repeatedly creates and drops a database, with the focus on creation using different name casing.
 * Create using all different methods, implicitly by inserting, creating views/indexes etc.
 *
 * Each thread uses its own database, though sometimes threads may try to create databases with
 * names that only differ in case, expecting the appriopriate error code.
 */
var $config = (function() {

    let data = {
        checkCommandResult: function checkCommandResult(mayFailWithDatabaseDifferCase, res) {
            if (mayFailWithDatabaseDifferCase && !res.ok)
                assertAlways.commandFailedWithCode(res, ErrorCodes.DatabaseDifferCase);
            else
                assertAlways.commandWorked(res);
            return res;
        },

        checkWriteResult: function checkWriteResult(mayFailWithDatabaseDifferCase, res) {
            if (mayFailWithDatabaseDifferCase && res.hasWriteError())
                assertAlways.writeErrorWithCode(res, ErrorCodes.DatabaseDifferCase);
            else
                assertAlways.writeOK(res);
            return res;
        }
    };

    let states = {
        init: function init(db, collName) {
            let uniqueNr = this.tid;
            let semiUniqueNr = Math.floor(uniqueNr / 2);

            // The semiUniqueDBName may clash and result in a DatabaseDifferCas error on creation,
            // while the uniqueDBName does not clash. The unique and created variables track this.
            this.semiUniqueDBName =
                (this.tid % 2 ? 'create_database' : 'CREATE_DATABASE') + semiUniqueNr;
            this.uniqueDBName = 'CreateDatabase' + uniqueNr;
            this.myDB = db.getSiblingDB(this.uniqueDBName);
            this.created = false;
            this.unique = true;

        },

        useSemiUniqueDBName: function useSemiUniqueDBName(db, collName) {
            this.myDB = db.getSiblingDB(this.semiUniqueDBName);
            this.unique = false;
        },

        createView: function createView(db, collName) {
            this.created =
                this.checkCommandResult(!this.unique, this.myDB.createView(collName, "nil", [])).ok;
        },

        createCollection: function createCollection(db, collName) {
            this.created =
                this.checkCommandResult(!this.unique, this.myDB.createCollection(collName)).ok;
        },

        createIndex: function createIndex(db, collName) {
            let background = Math.random > 0.5;
            let res = this.myDB.getCollection(collName).createIndex({x: 1}, {background});
            this.created |=
                this.checkCommandResult(!this.unique, res).createdCollectionAutomatically;
        },

        insert: function insert(db, collName) {
            this.created |= this.checkWriteResult(!this.created && !this.unique,
                                                  this.myDB.getCollection(collName).insert({x: 1}))
                                .nInserted == 1;
        },

        upsert: function upsert(db, collName) {
            this.created |= this.checkWriteResult(!this.created && !this.unique,
                                                  this.myDB.getCollection(collName).update(
                                                      {x: 1}, {x: 2}, {upsert: 1}))
                                .nUpserted == 1;
        },

        drop: function drop(db, collName) {
            if (this.created)
                assertAlways(this.myDB.getCollection(collName).drop());
        },

        dropDatabase: function dropDatabase(db, collName) {
            if (this.created)
                assertAlways.commandWorked(this.myDB.dropDatabase());
        },

        listDatabases: function listDatabases(db, collName) {
            for (let database of db.adminCommand({listDatabases: 1}).databases) {
                let res = db.getSiblingDB(database.name).runCommand({listCollections: 1});
                assertAlways.commandWorked(res);
                assertAlways.neq(database.name, this.myDB.toString(), "this DB shouldn't exist");
            }
        },

        listDatabasesNameOnly: function listDatabases(db, collName) {
            for (let database of db.adminCommand({listDatabases: 1, nameOnly: 1}).databases) {
                let res = db.getSiblingDB(database.name).runCommand({listCollections: 1});
                assertAlways.commandWorked(res);
                assertAlways.neq(database.name, this.myDB.toString(), "this DB shouldn't exist");
            }
        },
    };

    var transitions = {
        init: {
            useSemiUniqueDBName: 0.25,
            createView: 0.25,
            createCollection: 0.125,
            createIndex: 0.125,
            insert: 0.125,
            upsert: 0.125
        },
        useSemiUniqueDBName: {createCollection: 0.75, createView: 0.25},
        createView: {dropDatabase: 0.5, drop: 0.5},
        createCollection: {dropDatabase: 0.25, createIndex: 0.25, insert: 0.25, upsert: 0.25},
        createIndex: {insert: 0.25, upsert: 0.25, dropDatabase: 0.5},
        insert: {dropDatabase: 0.2, drop: 0.05, insert: 0.5, upsert: 0.25},
        upsert: {dropDatabase: 0.2, drop: 0.05, insert: 0.25, upsert: 0.5},
        drop: {dropDatabase: 0.75, init: 0.25},  // OK to leave the empty database behind sometimes
        dropDatabase: {init: 0.75, listDatabases: 0.15, listDatabasesNameOnly: 0.10},
        listDatabases: {init: 0.75, listDatabases: 0.15, listDatabasesNameOnly: 0.10},
        listDatabasesNameOnly: {init: 0.75, listDatabases: 0.10, listDatabasesNameOnly: 0.15},
    };

    return {
        data,
        // We only run a few iterations to reduce the amount of data cumulatively
        // written to disk by mmapv1. For example, setting 10 threads and 180
        // iterations (with an expected 6 transitions per create/drop roundtrip)
        // causes this workload to write at least 32MB (.ns and .0 files) * 10 threads
        // * 30 iterations worth of data to disk, or about 10GB, which can be slow on
        // test hosts.
        threadCount: 10,
        iterations: 180, states, transitions,
    };
})();

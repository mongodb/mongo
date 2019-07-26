/**
 * This verifies that an invalid authSchema document causes MongoDB to fail to start, except in the
 * presence of startupAuthSchemaValidation=false.
 *
 * @tags: [requires_persistence]
 */
(function() {

const dbpath = MongoRunner.dataPath + "validateAuthSchemaOnStartup/";
resetDbpath(dbpath);
const dbName = "validateAuthSchemaOnStartup";
const authSchemaColl = "system.version";

let mongod = MongoRunner.runMongod({dbpath: dbpath, auth: ""});
let adminDB = mongod.getDB('admin');

// Create a user.
adminDB.createUser(
    {user: "root", pwd: "root", roles: [{role: 'userAdminAnyDatabase', db: 'admin'}]});
assert(adminDB.auth("root", "root"));

MongoRunner.stopMongod(mongod);

// Start without auth to corrupt the authSchema document.
mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
adminDB = mongod.getDB('admin');

let currentVersion = adminDB[authSchemaColl].findOne({_id: 'authSchema'}).currentVersion;

// Invalidate the authSchema document.
assert.commandWorked(adminDB[authSchemaColl].update({_id: 'authSchema'}, {currentVersion: 'asdf'}));
MongoRunner.stopMongod(mongod);

// Confirm start up fails, even without --auth.
assert.eq(null, MongoRunner.runMongod({dbpath: dbpath, noCleanData: true}));

// Confirm startup works with the flag to disable validation so the document can be repaired.
mongod = MongoRunner.runMongod(
    {dbpath: dbpath, noCleanData: true, setParameter: "startupAuthSchemaValidation=false"});
adminDB = mongod.getDB('admin');
assert.commandWorked(
    adminDB[authSchemaColl].update({_id: 'authSchema'}, {currentVersion: currentVersion}));
MongoRunner.stopMongod(mongod);

// Confirm everything is normal again.
mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, auth: ""});
adminDB = mongod.getDB('admin');
assert(adminDB.auth("root", "root"));

MongoRunner.stopMongod(mongod);
})();

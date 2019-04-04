/**
 * This verifies that an invalid authSchema document causes MerizoDB to fail to start, except in the
 * presence of startupAuthSchemaValidation=false.
 *
 * @tags: [requires_persistence]
 */
(function() {

    const dbpath = MongoRunner.dataPath + "validateAuthSchemaOnStartup/";
    resetDbpath(dbpath);
    const dbName = "validateAuthSchemaOnStartup";
    const authSchemaColl = "system.version";

    let merizod = MongoRunner.runMongod({dbpath: dbpath, auth: ""});
    let adminDB = merizod.getDB('admin');

    // Create a user.
    adminDB.createUser(
        {user: "root", pwd: "root", roles: [{role: 'userAdminAnyDatabase', db: 'admin'}]});
    assert(adminDB.auth("root", "root"));

    MongoRunner.stopMongod(merizod);

    // Start without auth to corrupt the authSchema document.
    merizod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
    adminDB = merizod.getDB('admin');

    let currentVersion = adminDB[authSchemaColl].findOne({_id: 'authSchema'}).currentVersion;

    // Invalidate the authSchema document.
    assert.commandWorked(
        adminDB[authSchemaColl].update({_id: 'authSchema'}, {currentVersion: 'asdf'}));
    MongoRunner.stopMongod(merizod);

    // Confirm start up fails, even without --auth.
    assert.eq(null, MongoRunner.runMongod({dbpath: dbpath, noCleanData: true}));

    // Confirm startup works with the flag to disable validation so the document can be repaired.
    merizod = MongoRunner.runMongod(
        {dbpath: dbpath, noCleanData: true, setParameter: "startupAuthSchemaValidation=false"});
    adminDB = merizod.getDB('admin');
    assert.commandWorked(
        adminDB[authSchemaColl].update({_id: 'authSchema'}, {currentVersion: currentVersion}));
    MongoRunner.stopMongod(merizod);

    // Confirm everything is normal again.
    merizod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, auth: ""});
    adminDB = merizod.getDB('admin');
    assert(adminDB.auth("root", "root"));

    MongoRunner.stopMongod(merizod);
})();

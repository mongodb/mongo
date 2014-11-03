// Tests command line options for storage engine.

var baseName = "jstests_nopassthrough_storage_options";

// Cannot provide command line options for a non-active storage engine.
// For example, --smallfiles is a mmapv1-only option that cannot be used when
// the "devnull" storage engine is used.
var port = allocatePorts(1)[0];
var dppath = MongoRunner.dataPath + baseName;
var ret = runMongoProgram('mongod',
                          '--port', port, '--dbpath', MongoRunner.dataPath + baseName,
                          '--storageEngine', 'devnull',
                          '--smallfiles');
assert.neq(0, ret,
    'mongod should fail to start up when given incompatible storage options on the command line.');

// Cannot select "devnull" storage engine on the command when configuration file
// contains a "mmapv1" option.
ret = runMongoProgram('mongod',
                      '--port', port, '--dbpath', MongoRunner.dataPath + baseName,
                      '--storageEngine', 'devnull',
                      '--config', 'jstests/libs/config_files/enable_prealloc.json');
assert.neq(0, ret,
    'mongod should fail to start up when configuration file contains options for a storage ' +
    'engine different from the one specified on the command line.');

print(baseName + " succeeded.");

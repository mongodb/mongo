// mongotop_stress.js; ensure that running mongotop, even when the server is
// under heavy load, works as expected
var testName = 'mongotop_stress';
load('jstests/top/util/mongotop_common.js');

(function() {
  jsTest.log('Testing mongotop\'s performance under load');

  var runTests = function(topology, passthrough) {
    jsTest.log('Using ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();
    db = conn.getDB('foo'); // eslint-disable-line no-native-reassign

    // concurrently insert documents into thousands of collections
    var stressShell = '\nprint(\'starting read/write stress test\'); \n' +
    '   if (\'' + passthrough.name + '\' === \'auth\')' +
    '       db.getSiblingDB(\'admin\').auth(\'' + authUser + '\',\'' + authPassword + '\'); ' +
    '   var dbName = (Math.random() + 1).toString(36).substring(7); ' +
    '   var clName = (Math.random() + 1).toString(36).substring(7); ' +
    '   for (var i = 0; i < 10000; ++i) { ' +
    '       db.getSiblingDB(dbName).getCollection(clName).find({ x: i }).forEach(); \n' +
    '       sleep(1); \n' +
    '       db.getSiblingDB(dbName).getCollection(clName).insert({ x: i }); \n' +
    '       sleep(1);\n' +
    '   }\n';

    for (var i = 0; i < 10; ++i) {
      startParallelShell(stressShell);
    }

    // wait a bit for the stress to kick in
    sleep(5000);
    jsTest.log('Current operation(s)');
    printjson(db.currentOp());

    // ensure tool runs without error
    clearRawMongoProgramOutput();
    assert.eq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--json', '--rowcount', 1].concat(passthrough.args)), 0, 'failed 1');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
  });
}());

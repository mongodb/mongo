// mongotop_reports.js; ensure that running mongotop reports accurately on operations
// going on in namespaces
var testName = 'mongotop_reports';
load('jstests/top/util/mongotop_common.js');

(function() {
  jsTest.log('Testing mongotop\'s reporting fidelity');
  var assert = extendedAssert;
  var read = 'read';
  var write = 'write';

  var runReportTest = function(topology, passthrough, test) {
    jsTest.log('Using ' + passthrough.name + ' passthrough on ' + test.name + ' shell');
    var t = topology.init(passthrough);
    var conn = t.connection();
    db = conn.getDB('foo'); // eslint-disable-line no-native-reassign
    db.dropDatabase();
    assert.eq(db.bar.count(), 0, 'drop failed');

    // start the parallel shell command
    if (passthrough.name === auth.name) {
      var authCommand = '\n db.getSiblingDB(\'admin\').auth(\'' + authUser + '\',\'' + authPassword + '\'); \n';
      test.shellCommand = authCommand + test.shellCommand;
    }
    startParallelShell(test.shellCommand);

    // allow for command to actually start
    sleep(5000);

    // ensure tool runs without error
    clearRawMongoProgramOutput();
    var ret = executeProgram(['mongotop', '--port', conn.port, '--json', '--rowcount', 1].concat(passthrough.args));
    assert.eq(ret.exitCode, 0, 'failed 1');
    var parsedOutput;
    assert.eq.soon('object', function() {
      parsedOutput = JSON.parse(extractJSON(ret.getOutput()));
      return typeof parsedOutput;
    }, 'invalid JSON 1');

    // ensure only the active namespaces reports a non-zero value
    for (var namespace in parsedOutput.totals) {
      if (!parsedOutput.totals.hasOwnProperty(namespace)) {
        continue;
      }
      var isAuthActivity = namespace.indexOf('.system.') !== -1;
      var isReplActivity = namespace.indexOf('local.') !== -1;

      // authentication and replication activity should be ignored
      if (isAuthActivity || isReplActivity) {
        continue;
      }

      var nsDetails = parsedOutput.totals[namespace];
      assert.neq(nsDetails, undefined, 'no details reported for namespace ' + namespace);

      var comparator = 'eq';
      var shouldHaveActivity = test.namespaces.filter(function(testSpace) { // eslint-disable-line no-loop-func
        return testSpace === namespace;
      });

      // return the opposite comparator if this namespace should have activity
      if (shouldHaveActivity.length !== 0) {
        comparator = 'neq';
      }

      test.indicators.forEach(function(indicator) { // eslint-disable-line no-loop-func
        ['count', 'time'].forEach(function(metric) {
          assert[comparator](nsDetails[indicator][metric], 0, 'unexpected ' + indicator + ' activity on ' + namespace + '; ' + metric + ': ' + nsDetails[indicator][metric]);
          if (test.indicators.length === 1) {
            // read or write shell
            var opposite = read;
            if (test.name === read) {
              opposite = write;
            }
            // ensure there's no activity on the inactive metric
            // sometimes the readings are a bit out of sync - making some
            // allowance to prevent test flakiness
            assert.between(0, nsDetails[opposite][metric], 1, 'unexpected ' + opposite + ' (opposite) activity on ' + namespace + '; ' + metric + ': ' + nsDetails[opposite][metric]);
          } else {
            // read/write shell should have read and write activity
            assert[comparator](nsDetails[read][metric], 0, 'unexpected ' + read + ' activity (read/write) on ' + namespace + '; ' + metric + ': ' + nsDetails[read][metric]);
            assert[comparator](nsDetails[write][metric], 0, 'unexpected ' + write + ' activity (read/write) on ' + namespace + '; ' + metric + ': ' + nsDetails[write][metric]);
          }
          var calculatedSum = nsDetails[read][metric] + nsDetails[write][metric];
          var expectedSum = nsDetails['total'][metric];

          // sometimes the total isn't exact - making some allowance to prevent
          // test flakiness
          assert.between(0, expectedSum - calculatedSum, 1, 'unexpected sum for metric ' + metric + ': expected ' + expectedSum + ' but got ' + calculatedSum);
        });
      });
    }
    t.stop();
  };

  var runTests = function(topology, passthrough) {
    var readShell = '\nprint(\'starting read\'); \n' +
      'for (var i = 0; i < 1000000; ++i) \n{ ' +
      '  db.getSiblingDB(\'foo\').bar.find({ x: i }).forEach(function(){}); \n' +
      '  sleep(1); \n' +
      '}\n';

    var writeShell = '\nprint(\'starting write\'); \n' +
      'for (var i = 0; i < 1000000; ++i) { \n' +
      '  db.getSiblingDB(\'foo\').bar.insert({ x: i }); \n' +
      '  sleep(1); \n' +
      '}\n';

    var readWriteShell = '\nprint(\'starting read/write\'); \n' +
      'for (var i = 0; i < 1000000; ++i) \n{ ' +
      '  db.getSiblingDB(\'foo\').bar.insert({ x: i }); \n' +
      '  db.getSiblingDB(\'foo\').bar.find({ x: i }).forEach(function(){}); \n' +
      '  sleep(1); \n' +
      '}\n';

    var testSpaces = [
      ['foo.bar'],
      ['foo.bar', 'bar.foo'],
    ];

    var tests = [{
      name: read,
      indicators: [read],
      shellCommand: readShell,
    }, {
      name: write,
      indicators: [write],
      shellCommand: writeShell,
    }, {
      name: read + '/' + write,
      indicators: [read, write],
      shellCommand: readWriteShell,
    }];

    tests.forEach(function(test) {
      testSpaces.forEach(function(testSpace) {
        test.namespaces = testSpace;
        runReportTest(topology, passthrough, test);
      });
    });
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
  });
}());

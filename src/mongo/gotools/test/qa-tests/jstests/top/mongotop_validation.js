// mongotop_validation.js; ensure that running mongotop using invalid arguments
// fail as expected
var testName = 'mongotop_validation';
load('jstests/top/util/mongotop_common.js');

(function() {
  jsTest.log('Testing mongotop with invalid arguments');

  var runTests = function(topology, passthrough) {
    jsTest.log('Using ' + passthrough.name + ' passthrough');
    var t = topology.init(passthrough);
    var conn = t.connection();

    // checking the version should not return an error
    assert.eq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--version'].concat(passthrough.args)), 0, '--version assertion failure 1');


    // ensure tool returns an error...

    // when used with an invalid port
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', 55555].concat(passthrough.args)), 0, '--port assertion failure 1');
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', 'hello'].concat(passthrough.args)), 0, '--port assertion failure 2');
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', ''].concat(passthrough.args)), 0, '--port assertion failure 3');

    // when supplied invalid row counts
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--rowcount', '-2'].concat(passthrough.args)), 0, '--rowcount assertion failure 1');
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--rowcount', 'hello'].concat(passthrough.args)), 0, '--rowcount assertion failure 2');
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--rowcount', ''].concat(passthrough.args)), 0, '--rowcount assertion failure 3');

    // when supplied invalid sleep times
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '-4'].concat(passthrough.args)), 0, 'sleep time assertion failure 1');
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, 'forever'].concat(passthrough.args)), 0, 'sleep time assertion failure 2');

    // when supplied invalid options
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--elder'].concat(passthrough.args)), 0, 'invalid options failure 1');
    assert.neq(runMongoProgram.apply(this, ['mongotop', '--port', conn.port, '--price'].concat(passthrough.args)), 0, 'invalid options failure 2');

    t.stop();
  };

  // run with plain and auth passthroughs
  passthroughs.forEach(function(passthrough) {
    runTests(standaloneTopology, passthrough);
    runTests(replicaSetTopology, passthrough);
  });
}());

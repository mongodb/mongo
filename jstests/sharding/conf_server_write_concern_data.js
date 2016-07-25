/**
 * Test write concern with w parameter when writing data directly to the config namespaces works as
 * expected.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2});
    var confDB = st.s.getDB('config');
    var confDBOnConfigPrimary = st.configRS.getPrimary().getDB('config');
    var localDBOnConfigPrimary = st.configRS.getPrimary().getDB('local');

    // w:majority should work both through mongos and directly against the config server for both
    // local and non-local database
    assert.writeOK(confDB.settings.update(
        {_id: 'balancer'}, {$set: {stopped: true}}, {writeConcern: {w: 'majority'}}));
    assert.writeOK(confDBOnConfigPrimary.settings.update(
        {_id: 'balancer'}, {$set: {stopped: true}}, {writeConcern: {w: 'majority'}}));
    assert.writeOK(localDBOnConfigPrimary.TestDB.update(
        {_id: 'TestColl'}, {$set: {TestField: 'TestValue'}}, {writeConcern: {w: 'majority'}}));

    // w:1 should not work, unless it is for the local database
    assert.writeError(confDBOnConfigPrimary.settings.update(
        {_id: 'balancer'}, {$set: {stopped: true}}, {writeConcern: {w: 1}}));
    assert.writeOK(localDBOnConfigPrimary.TestDB.update(
        {_id: 'TestColl'}, {$set: {TestField: 'TestValue'}}, {writeConcern: {w: 1}}));

    // Write concerns other than w:1 and w:majority should fail.
    assert.writeError(
        confDB.settings.update({_id: 'balancer'}, {$set: {stopped: true}}, {writeConcern: {w: 2}}));
    assert.writeError(confDBOnConfigPrimary.settings.update(
        {_id: 'balancer'}, {$set: {stopped: true}}, {writeConcern: {w: 2}}));

    st.stop();
})();

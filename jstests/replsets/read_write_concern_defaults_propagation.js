// Tests propagation of RWC defaults across a replica set.
//
// @tags: [requires_fcv_44]

load("jstests/libs/read_write_concern_defaults_propagation.js");

(function() {
'use strict';

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();

// TODO: after SERVER-43720 is done, remove this line and uncomment the below line.
ReadWriteConcernDefaultsPropagation.runTests(primary, [primary]);
// ReadWriteConcernDefaultsPropagation.runTests(primary, secondaries);

rst.stopSet();
})();

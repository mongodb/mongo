(function() {
'use strict';

load("jstests/replsets/libs/rename_across_dbs.js");

const nodes = [{binVersion: 'latest'}, {binVersion: 'last-lts'}, {}];
const options = {
    nodes: nodes,
    setFeatureCompatibilityVersion: lastLTSFCV,
    dropTarget: true,
};

new RenameAcrossDatabasesTest(options).run();
}());

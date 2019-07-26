(function() {
'use strict';

load("jstests/libs/feature_compatibility_version.js");
load("jstests/replsets/libs/rename_across_dbs.js");

const nodes = [{binVersion: 'latest'}, {binVersion: 'last-stable'}, {}];
const options = {
    nodes: nodes,
    setFeatureCompatibilityVersion: lastStableFCV,
    dropTarget: true,
};

new RenameAcrossDatabasesTest(options).run();
}());

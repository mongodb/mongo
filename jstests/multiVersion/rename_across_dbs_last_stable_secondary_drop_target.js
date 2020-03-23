// @tags: [fix_for_fcv_46]
(function() {
'use strict';

load("jstests/replsets/libs/rename_across_dbs.js");

const nodes = [{binVersion: 'latest'}, {binVersion: 'last-stable'}, {}];
const options = {
    nodes: nodes,
    setFeatureCompatibilityVersion: lastStableFCV,
    dropTarget: true,
};

new RenameAcrossDatabasesTest(options).run();
}());

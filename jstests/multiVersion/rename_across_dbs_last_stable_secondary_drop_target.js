(function() {
    'use strict';

    load("jstests/replsets/libs/rename_across_dbs.js");

    const nodes = [{binVersion: 'latest'}, {binVersion: 'last-stable'}, {}];
    const options = {
        nodes: nodes,
        setFeatureCompatibilityVersion: '3.4',
        dropTarget: true,
    };

    new RenameAcrossDatabasesTest(options).run();
}());

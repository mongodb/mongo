(function() {
    'use strict';

    load("jstests/replsets/libs/rename_across_dbs.js");

    const nodes = [{binVersion: 'last-stable'}, {binVersion: 'latest'}, {}];
    const options = {
        nodes: nodes,
        setFeatureCompatibilityVersion: '3.4',
    };

    new RenameAcrossDatabasesTest(options).run();
}());

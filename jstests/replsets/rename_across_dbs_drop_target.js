(function() {
    'use strict';

    load("jstests/replsets/libs/rename_across_dbs.js");

    const options = {dropTarget: true};
    new RenameAcrossDatabasesTest(options).run();
}());

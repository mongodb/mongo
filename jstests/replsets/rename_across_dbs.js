(function() {
    'use strict';

    load("jstests/replsets/libs/rename_across_dbs.js");

    new RenameAcrossDatabasesTest().run();
}());

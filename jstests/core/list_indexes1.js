// Basic functional tests for the listIndexes command.

load("jstests/libs/list_indexes_lib.js");

basicTest();
invalidValueTest();
nonexistentCollectionTest();
nonexistentDatabaseTest();
cornerCaseTest();

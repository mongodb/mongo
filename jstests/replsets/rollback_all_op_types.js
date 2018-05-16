/*
 * Test to ensure that rollback is able to handle every supported operation type correctly.
 *
 * The purpose of this integration test is to check that rollback is able to parse and revert all
 * oplog entry types that would be generated in a real system. It provides a level of assurance at a
 * higher system layer than our unit tests, which can be considerably more "artificial". Unit tests
 * will mock many system components, and sometimes will mock behaviors that don't necessarily match
 * true system behavior i.e. mocking an oplog entry with an incorrect format. So, this integration
 * test provides an additional verification of rollback's correctness within a real replica set.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test_deluxe.js");

    let noOp = () => {};

    /**
     * All operation types that are able to be rolled back.
     *
     * Each operation type maps to an array of test objects that contains an 'init' function, an
     * 'op' function, and an optional 'description' field. Some operations depend on the current
     * state of the database, so the 'init' function provides a way to set up the database before an
     * operation is executed. All init functions are executed at the very beginning of the test, as
     * part of CommonOps. Also, to provide isolation between commands, each is given its own
     * database to execute in.
     *
     * Each operation has an array of test objects to allow testing of multiple variations of an
     * operation. Each test case in an array will be executed in isolation.
     *
     * Note: The 'dropDatabase' command is excluded and tested separately. It cannot be tested
     * directly using the RollbackTest fixture, since the command is always up-converted to use
     * majority write concern in 3.6.
     *
     */
    let rollbackOps = {
        "insert": [{
            init: (db, collName) => {
                assert.commandWorked(db.createCollection(collName));
            },
            op: (db, collName) => {
                assert.writeOK(db[collName].insert({_id: 0}));
            }
        }],
        "update": [{
            init: (db, collName) => {
                assert.writeOK(db[collName].insert({_id: 0, val: 0}));
            },
            op: (db, collName) => {
                assert.writeOK(db[collName].update({_id: 0}, {val: 1}));
            },
        }],
        "delete": [{
            init: (db, collName) => {
                assert.writeOK(db[collName].insert({_id: 0}));
            },
            op: (db, collName) => {
                assert.writeOK(db[collName].remove({_id: 0}));
            },
        }],
        "create": [{
            init: noOp,
            op: (db, collName) => {
                assert.commandWorked(db.createCollection(collName));
            },
        }],
        "drop": [{
            init: (db, collName) => {
                assert.commandWorked(db.createCollection(collName));
            },
            op: (db, collName) => {
                assert.commandWorked(db.runCommand({drop: collName}));
            },
        }],
        "createIndexes": [{
            init: (db, collName) => {
                assert.commandWorked(db.createCollection(collName));
            },
            op: (db, collName) => {
                assert.commandWorked(db.runCommand({
                    createIndexes: collName,
                    indexes: [{name: collName + "_index", key: {index_key: 1}}]
                }));
            }
        }],
        "dropIndexes": [
            {
              description: "singleIndex",
              init: (db, collName) => {
                  assert.commandWorked(db.runCommand({
                      createIndexes: collName,
                      indexes: [{name: collName + "_index", key: {index_key: 1}}]
                  }));
              },
              op: (db, collName) => {
                  assert.commandWorked(
                      db.runCommand({dropIndexes: collName, index: collName + "_index"}));
              }
            },
            {
              description: "allIndexes",
              init: (db, collName) => {
                  assert.commandWorked(db.runCommand({
                      createIndexes: collName,
                      indexes: [
                          {name: collName + "_index_0", key: {index_key_0: 1}},
                          {name: collName + "_index_1", key: {index_key_1: 1}},
                          {name: collName + "_index_2", key: {index_key_2: 1}}
                      ]
                  }));
              },
              op: (db, collName) => {
                  assert.commandWorked(db.runCommand({dropIndexes: collName, index: "*"}));
              }
            }
        ],
        "renameCollection": [
            {
              description: "withinSameDatabase",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName + "_source"));
              },
              op: (db, collName) => {
                  let nss = db[collName].getFullName();
                  assert.commandWorked(
                      db.adminCommand({renameCollection: nss + "_source", to: nss + "_dest"}));
              },
            },
            {
              description: "acrossDatabases",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName));
              },
              op: (db, collName) => {
                  let sourceNss = db[collName].getFullName();
                  let destNss = db.getName() + "_dest." + collName;
                  assert.commandWorked(db.adminCommand({renameCollection: sourceNss, to: destNss}));
              },
            },
            {
              description: "acrossDatabasesDropTarget",
              init: (db, collName) => {
                  let dbName = db.getName();
                  let destDb = db.getSiblingDB(dbName + "_dest");
                  assert.commandWorked(db.createCollection(collName));
                  assert.commandWorked(destDb.createCollection(collName));
              },
              op: (db, collName) => {
                  let sourceNss = db[collName].getFullName();
                  let destNss = db.getName() + "_dest." + collName;
                  assert.commandWorked(db.adminCommand(
                      {renameCollection: sourceNss, to: destNss, dropTarget: true}));
              },
            },
            {
              description: "dropTarget",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName + "_source"));
                  assert.commandWorked(db.createCollection(collName + "_dest"));
              },
              op: (db, collName) => {
                  let nss = db[collName].getFullName();
                  assert.commandWorked(db.adminCommand(
                      {renameCollection: nss + "_source", to: nss + "_dest", dropTarget: true}));
              },
            }

        ],
        "collMod": [
            {
              description: "allCollectionOptions",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName));
              },
              op: (db, collName) => {
                  assert.commandWorked(db.runCommand({
                      collMod: collName,
                      usePowerOf2Sizes: false,
                      noPadding: true,
                      validator: {a: 1},
                      validationLevel: "moderate",
                      validationAction: "warn"
                  }));
              }
            },
            {
              description: "validationOptionsWithoutValidator",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName));
              },
              op: (db, collName) => {
                  assert.commandWorked(db.runCommand(
                      {collMod: collName, validationLevel: "moderate", validationAction: "warn"}));
              }
            },
            {
              description: "existingValidationOptions",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName));
                  assert.commandWorked(db.runCommand(
                      {collMod: collName, validationLevel: "moderate", validationAction: "warn"}));
              },
              op: (db, collName) => {
                  assert.commandWorked(db.runCommand({
                      collMod: collName,
                      validator: {a: 1},
                      validationLevel: "moderate",
                      validationAction: "warn"
                  }));
              }
            }
        ],
        "convertToCapped": [{
            init: (db, collName) => {
                assert.commandWorked(db.createCollection(collName));
            },
            op: (db, collName) => {
                assert.commandWorked(db.runCommand({convertToCapped: collName, size: 1024}));
            },
        }],
        "applyOps": [
            {
              description: "multipleCRUDOps",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName));
              },
              // In 3.6 only document CRUD operations are grouped into a single applyOps oplog
              // entry.
              op: (db, collName) => {
                  let collInfo = db.getCollectionInfos({name: collName})[0];
                  let uuid = collInfo.info.uuid;
                  let coll = db.getCollection(collName);
                  let opsToApply = [
                      {op: "i", ns: coll.getFullName(), ui: uuid, o: {_id: 0}},
                      {
                        op: "u",
                        ns: coll.getFullName(),
                        ui: uuid,
                        o: {_id: 0, val: 1},
                        o2: {_id: 0},
                      },
                      {op: "d", ns: coll.getFullName(), ui: uuid, o: {_id: 0}}
                  ];
                  assert.commandWorked(db.adminCommand({applyOps: opsToApply}));
              }
            },
            {
              description: "opWithoutUUID",
              init: (db, collName) => {
                  assert.commandWorked(db.createCollection(collName));
              },
              // In 3.6 only document CRUD operations are grouped into a single applyOps oplog
              // entry.
              op: (db, collName) => {
                  let coll = db.getCollection(collName);
                  let opsToApply = [
                      {op: "i", ns: coll.getFullName(), o: {_id: 0}},
                  ];
                  assert.commandWorked(db.adminCommand({applyOps: opsToApply}));
              }
            }
        ]
    };

    let testCollName = "test";
    let opNames = Object.keys(rollbackOps);

    /**
     * Create the test name string given an operation name and the test case index. The test
     * name for the nth test case of an operation called "opName", with description "description",
     * will be "opName_<n>_description".
     */
    function opTestNameStr(opName, description, ind) {
        let opVariantName = opName + "_" + ind;
        if (description) {
            opVariantName = opVariantName + "_" + description;
        }
        return opVariantName;
    }

    /**
     * Operations that will be present on both nodes, before the common point.
     */
    let CommonOps = (node) => {
        // Ensure there is at least one common op between nodes.
        node.getDB("commonOp")["test"].insert({_id: "common_op"});

        // Run init functions for each op type. Each is given its own database to run in and a
        // standard collection name to use.
        jsTestLog("Performing init operations for every operation type.");
        opNames.forEach(opName => {
            let opObj = rollbackOps[opName];
            opObj.forEach((opVariantObj, ind) => {
                let opVariantName = opTestNameStr(opName, opVariantObj.description, ind);
                opVariantObj.init(node.getDB(opVariantName), testCollName);
            });
        });
    };

    /**
     * Operations that will be performed on the rollback node past the common point.
     */
    let RollbackOps = (node) => {

        // Returns a new object with any metadata fields from the given command object removed.
        function basicCommandObj(fullCommandObj) {
            let basicCommandObj = {};
            for (let field in fullCommandObj) {
                if (fullCommandObj.hasOwnProperty(field) && !field.startsWith("$")) {
                    basicCommandObj[field] = fullCommandObj[field];
                }
            }
            return basicCommandObj;
        }

        // Execute the operation given by 'opFn'. 'opName' is the string identifier of the
        // operation to be executed.
        function executeOp(opName, opFn) {
            // Override 'runCommand' so we can capture the raw command object for each operation
            // and log it, to improve diagnostics.
            const runCommandOriginal = Mongo.prototype.runCommand;
            Mongo.prototype.runCommand = function(dbName, commandObj, options) {
                jsTestLog("Executing command for '" + opName + "' test: \n" +
                          tojson(basicCommandObj(commandObj)));
                return runCommandOriginal.apply(this, arguments);
            };

            opFn(node.getDB(opName), testCollName);

            // Reset runCommand to its normal behavior.
            Mongo.prototype.runCommand = runCommandOriginal;
        }

        jsTestLog("Performing rollback operations for every operation type.");
        opNames.forEach(opName => {
            let opObj = rollbackOps[opName];
            // Execute all test cases for this operation type.
            jsTestLog("Performing '" + opName + "' operations.");
            opObj.forEach((opVariantObj, ind) => {
                let opVariantName = opTestNameStr(opName, opVariantObj.description, ind);
                executeOp(opVariantName, opVariantObj.op);
            });
        });

    };

    // Set up Rollback Test.
    let rollbackTest = new RollbackTestDeluxe();
    CommonOps(rollbackTest.getPrimary());

    // Perform the operations that will be rolled back.
    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    // Complete cycle one of rollback. Data consistency is checked automatically after entering
    // steady state.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Again, perform operations that will be rolled back. This time, each node in the replica set
    // has assumed a different role and will roll back operations that were applied in a different
    // state (e.g. as a SECONDARY as opposed to a PRIMARY).
    rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    // Complete cycle two of rollback.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Perform operations that will be rolled back one more time.
    rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(rollbackNode);

    // Complete cycle three of rollback. After this cycle is completed, the replica set returns to
    // its original topology.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Check the replica set.
    rollbackTest.stop();
})();

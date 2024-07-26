import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "validate";

const primary = rst.getPrimary();
const db = primary.getDB(dbName);

assert.commandWorked(db.createCollection(collName));
const coll = db.getCollection(collName);

const forceCheckpoint = () => {
    assert.commandWorked(db.adminCommand({fsync: 1}));
};

coll.drop();
assert.commandWorked(coll.createIndex({"a.b": 1, "a.c": "text"}));

forceCheckpoint();
let res = coll.validate();
assert.commandWorked(res);
assert(res.valid);

configureFailPoint(db, "enableCompoundTextIndexes", {}, "alwaysOn");
assert.commandWorked(coll.insert({"a": [{"b": 1, "c": "foo"}, {"b": 2, "c": "bar"}]}));
configureFailPoint(db, "enableCompoundTextIndexes", {}, "off");

forceCheckpoint();
let res1 = coll.validate();
assert(!res1.valid);
assert.eq(res1.errors.length, 2);
assert(res1.errors[0].startsWith("Could not build key for index"));

rst.stopSet(null, null, {skipValidation: true});
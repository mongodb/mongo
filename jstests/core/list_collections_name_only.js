// Test nameOnly option of listCollections
(function() {
"use strict";

var mydb = db.getSiblingDB("list_collections_nameonly");
var res;
var collObj;

assert.commandWorked(mydb.dropDatabase());
assert.commandWorked(mydb.createCollection("foo"));
res = mydb.runCommand({listCollections: 1, nameOnly: true});
assert.commandWorked(res);
collObj = res.cursor.firstBatch[0];
// collObj should only have name and type fields.
assert.eq('foo', collObj.name);
assert.eq('collection', collObj.type);
assert(!collObj.hasOwnProperty("idIndex"), tojson(collObj));
assert(!collObj.hasOwnProperty("options"), tojson(collObj));
assert(!collObj.hasOwnProperty("info"), tojson(collObj));

// listCollections for views still works
assert.commandWorked(mydb.createView("bar", "foo", []));
res = mydb.runCommand({listCollections: 1, nameOnly: true});
assert.commandWorked(res);
print(tojson(res));
collObj = res.cursor.firstBatch.filter(function(c) {
    return c.name === "bar";
})[0];
assert.eq('bar', collObj.name);
assert.eq('view', collObj.type);
assert(!collObj.hasOwnProperty("options"), tojson(collObj));
assert(!collObj.hasOwnProperty("info"), tojson(collObj));
}());

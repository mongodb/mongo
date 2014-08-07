/**
 * 1. check top numbers are correct
 */

var name = "toptest";

var testDB = db.getSiblingDB(name);
var testColl = testDB[name + "coll"]

testColl.drop()

var topResult = testDB.adminCommand("top");
printjson(topResult.totals[testColl.getFullName()]);

var inserts = 0;
for(i=0;i<20;i++) {
	testColl.insert({_id:i});
	inserts++;
}
var topResult = testDB.adminCommand("top");
print("inserted " + inserts)
printjson(topResult.totals[testColl.getFullName()]);
//verify only 20 inserts took place
assert(inserts, topResult.totals[testColl.getFullName()].insert.count);

testColl.drop()
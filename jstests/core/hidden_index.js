// Test expected behavior for hidden indexes. A hidden index is invisible to the query planner so
// it will not be used in planning. It is handled in the same way as other indexes by the index
// catalog and for TTL purposes.

(function () {
    'use strict';
    load("jstests/libs/analyze_plan.js");  // For getPlanStages.
    const testtab = db["hidden_index"];

    function usedIndex(query) {
	const explain = assert.commandWorked(testtab.find(query).explain());
	const ixScans = getPlanStages(explain.queryPlanner.winningPlan, "IXSCAN");
	return ixScans.length === 1;
    }

    function HiddenCheckTest(query, index_type, wildcard) {
	let index_name;
	if (wildcard)
	    index_name = 'a.$**_' + index_type;
	else
	    index_name = 'a_' + index_type;

	testtab.dropIndexes();
	if (wildcard)
	    assert.commandWorked(testtab.createIndex({"a.$**": index_type}));
	else
	    assert.commandWorked(testtab.createIndex({"a": index_type}));
	
	assert.eq(testtab.getIndexes()[1].hidden, undefined);
	assert(usedIndex(query));
	
	assert.commandWorked(testtab.hideIndex(index_name));
	assert(testtab.getIndexes()[1].hidden);
	if (index_type === "text") {
	    // errmsg: error processing query: ns=test.hidden_indexTree: TEXT : query=java,
	    // language=english, caseSensitive=0, diacriticSensitive=0,
	    // tag=NULL\nSort: {}\nProj: {}\n planner returned error ::
	    // caused by :: need exactly one text index for $text query
	    assert.throws(() => {testtab.find(query).explain();});
	    return;
	}
	assert(!usedIndex(query));
	
	assert.commandWorked(testtab.unhideIndex(index_name));
	assert.eq(testtab.getIndexes()[1].hidden, undefined);
	assert(usedIndex(query));
	
	assert.commandWorked(testtab.dropIndex(index_name));

	if (wildcard)
	    assert.commandWorked(testtab.createIndex({"a.$**": index_type}, {hidden: true}));
	else
	    assert.commandWorked(testtab.createIndex({"a": index_type}, {hidden: true}));

	assert(testtab.getIndexes()[1].hidden);
	assert(!usedIndex(query));
    }

    
    // Normal index testing.
    HiddenCheckTest({a: 1}, 1);
    // GEO index testing.
    HiddenCheckTest({a: { $geoWithin :
			  { $geometry :
			    { type : "Polygon" ,
                              coordinates : [ [
                                  [ 0 , 0 ] ,
                                  [ 3 , 6 ] ,
                                  [ 6 , 1 ] ,
                                  [ 0 , 0 ]
                              ] ]
			    } } } }, "2dsphere");

    // Fts index.
    HiddenCheckTest({ $text: { $search: "java" } }, "text");

    // Wildcard index.
    HiddenCheckTest({"a.f": 1}, 1, true);

    // Hidden index on capped collection.
    assert(testtab.drop());
    assert.commandWorked(db.createCollection("hidden_index", {capped: true, size: 100}));
    HiddenCheckTest({a: 1}, 1);
    
    // Can't hide index on system index.
    const systemtab = db.getSiblingDB('admin').system.version;
    assert.commandWorked(systemtab.createIndex({a: 1}));
    assert.commandFailedWithCode(systemtab.hideIndex("a_1"), 2);
    assert.commandWorked(systemtab.dropIndex("a_1"));
    assert.commandFailedWithCode(systemtab.createIndex({a: 1}, {hidden: true}), 2);

    // Can't hide _id index.
    assert.commandFailed(testtab.hideIndex("_id_"));

    // We can change ttl index and hide info at the same time.
    assert.commandWorked(testtab.dropIndexes());
    assert.commandWorked(testtab.createIndex({"tm": 1}, {expireAfterSeconds: 10}));
    assert.eq(testtab.getIndexes()[1].hidden, undefined);
    assert.eq(testtab.getIndexes()[1].expireAfterSeconds, 10);

    db.runCommand({"collMod": testtab.getName(), "index": {"name": "tm_1",
							   "expireAfterSeconds": 1,
							   "hidden": true
							  }});
    assert(testtab.getIndexes()[1].hidden);
    assert.eq(testtab.getIndexes()[1].expireAfterSeconds, 1);

    // Hidden index is only allowed with FCV 4.4.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.2"}));
    assert.commandFailedWithCode(testtab.createIndex({b: 1}, {hidden: true}), 31449);
    assert.commandWorked(testtab.createIndex({b: 1}));
    assert.commandFailedWithCode(testtab.hideIndex("b_1"), 2);
    assert.commandWorked(testtab.unhideIndex("tm_1"));
})();

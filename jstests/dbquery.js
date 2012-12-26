t = db.dbquery;

function assertSpecial(q, p) {
	assert( q._special , p + ": is not special : " + tojson(q._query) );
}

function assertHas(q, f, p) {
	assert( q._query[f] , p + ": doesn't have " + f + " : " + tojson(q._query) );
}

function assertNotHas(q, f, p) {
	assert( !q._query[f] , p + ": has " + f + " : " + tojson(q._query) );
}

var q_from_log1 = {$query: {x:1}, $orderby:{y:1}}
var q1 = t.find(q_from_log1)._ensureSpecial();
assertSpecial(q1, "q1")
assertHas(q1, "$query", "q1")
assertHas(q1, "orderby", "q1")
assertNotHas(q1, "$orderby", "q1")

var q_from_log2 = {$query: {}, $max:{y:1}, $min: {y:0}}
var q2 = t.find(q_from_log2)._ensureSpecial();
assertSpecial(q2, "q2")
assertHas(q2, "$query", "q2")
assertHas(q2, "$min", "q2")
assertHas(q2, "$max", "q2")

var q_from_log3 = {$query: {x:1}, $explain:true}
var q3 = t.find(q_from_log3)._ensureSpecial();
assertSpecial(q3, "q3")
assertHas(q3, "$query", "q3")
assertHas(q3, "$explain", "q3")

var q_from_log4 = {query: {x:1}, orderby: {y:0}}
var q4 = t.find(q_from_log4)._ensureSpecial();
assertSpecial(q4, "q4")
assertHas(q4, "$query", "q4")
assertHas(q4, "orderby", "q4")

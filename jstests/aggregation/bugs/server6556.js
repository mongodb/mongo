// ensure strings containing null characters dont end at that null

c = db.s6556;
c.drop();

c.save({foo:"as\0df"});

// compare the whole string, they should match
assert.eq(c.aggregate({$project: {_id: 0, matches: {$eq:["as\0df", "$foo"]}}}).result, [{matches:true}]);
// compare with the substring containing only the up to the null, they should not match
assert.eq(c.aggregate({$project: {_id: 0, matches: {$eq:["as\0df", {$substr:["$foo",0,3]}]}}}).result, [{matches:false}]);
// partial the other way shouldnt work either
assert.eq(c.aggregate({$project: {_id: 0, matches: {$eq:["as", "$foo"]}}}).result, [{matches:false}]);
// neither should one that differs after the null
assert.eq(c.aggregate({$project: {_id: 0, matches: {$eq:["as\0de", "$foo"]}}}).result, [{matches:false}]);
// should assert on fieldpaths with a null
assert.throws( function() {
    c.aggregate({$project: {_id: 0, matches: {$eq:["as\0df", "$f\0oo"]}}})
});
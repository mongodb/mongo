// From SERVER-2381 
// Tests to make sure that nested multi-key indexing works for geo indexes and is not used for direct position
// lookups

db.test.drop()
db.test.insert({ p : [1112,3473], t : [{ k : 'a', v : 'b' }, { k : 'c', v : 'd' }] })
db.test.ensureIndex({ p : '2d', 't.k' : 1 }, { min : 0, max : 10000 })

// Succeeds, since on direct lookup should not use the index
assert(1 == db.test.find({p:[1112,3473],'t.k':'a'}).count(), "A")
// Succeeds and uses the geo index
assert(1 == db.test.find({p:{$within:{$box:[[1111,3472],[1113,3475]]}}, 't.k' : 'a' }).count(), "B")


db.test.drop()
db.test.insert({ point:[ 1, 10 ], tags : [ { k : 'key', v : 'value' }, { k : 'key2', v : 123 } ] })
db.test.insert({ point:[ 1, 10 ], tags : [ { k : 'key', v : 'value' } ] })

db.test.ensureIndex({ point : "2d" , "tags.k" : 1, "tags.v" : 1 })

// Succeeds, since should now lookup multi-keys correctly
assert(2 == db.test.find({ point : { $within : { $box : [[0,0],[12,12]] } } }).count(), "C") 
// Succeeds, and should not use geoindex
assert(2 == db.test.find({ point : [1, 10] }).count(), "D")
assert(2 == db.test.find({ point : [1, 10], "tags.v" : "value" }).count(), "E")
assert(1 == db.test.find({ point : [1, 10], "tags.v" : 123 }).count(), "F")


db.test.drop()
db.test.insert({ point:[ 1, 10 ], tags : [ { k : { 'hello' : 'world'}, v : 'value' }, { k : 'key2', v : 123 } ] })
db.test.insert({ point:[ 1, 10 ], tags : [ { k : 'key', v : 'value' } ] })

db.test.ensureIndex({ point : "2d" , "tags.k" : 1, "tags.v" : 1 })

// Succeeds, should be able to look up the complex element
assert(1 == db.test.find({ point : { $within : { $box : [[0,0],[12,12]] } }, 'tags.k' : { 'hello' : 'world' } }).count(), "G")
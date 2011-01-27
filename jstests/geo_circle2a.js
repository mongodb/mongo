// From SERVER-2381 

db.test.drop()
db.test.insert({ p : [1112,3473], t : [{ k : 'a', v : 'b' }, { k : 'c', v : 'd' }] })
db.test.ensureIndex({ p : '2d', 't.k' : 1 }, { min : 0, max : 10000 })

// FIXME: FAILS, since multikeys nested as sub-objects are not returned by getKeys() for 2d indexing
//assert(1 == db.test.find({p:[1112,3473],'t.k':'a'}).count()), "A")
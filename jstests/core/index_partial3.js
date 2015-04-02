
t = db.index_filtered3;
t.drop();

t.insert( { x : 1 } );

function badFilter(filter) {
    var res = t.ensureIndex( { x : 1 } , { filter : filter } );
    assert(!res.ok, tojson(res));
    assert.eq(2, res.code, tojson(res));
    printjson(res);
}

badFilter(5);
badFilter({$and : 5});
badFilter({x : /abc/});


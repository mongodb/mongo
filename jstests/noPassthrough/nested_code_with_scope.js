// SERVER-44366 Test that nesting of CodeWithScope works properly

const nestedCWS = Code("function(){return 1;}", {
    f: Code("function(){return 2};", {
        f: Code("function(){return 3};", {
            f: Code("function(){return 4};", {
                f: Code("function(){return 5};", {
                    f: Code("function(){return 6};", {
                        f: Code("function(){return 7};", {
                            f: Code("function(){return 8};", {
                                f: Code("function(){return 9};", {
                                    f: Code("function(){return 10};", {
                                        f: Code("function(){return 11};", {
                                            f: Code("function(){return 12};", {
                                                f: Code("function(){return 13};", {
                                                    f: Code("function(){return 14};",
                                                            {f: Code("function(){return 15};", {})})
                                                })
                                            })
                                        })
                                    })
                                })
                            })
                        })
                    })
                })
            })
        })
    })
});
var conn = MongoRunner.runMongod({setParameter: "maxBSONDepth=30"});
var testDB = conn.getDB("nestedCWS");
var coll = testDB.getCollection("test");
coll.insert({_id: nestedCWS});

MongoRunner.stopMongod(conn);
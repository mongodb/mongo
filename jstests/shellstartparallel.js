function f() {
    throw "intentional_throw_to_test_assert_throws";
}
assert.throws(f);

// verify that join works
db.sps.drop();
join = startParallelShell("sleep(1000); db.sps.insert({x:1});");
join();
// print(db.sps.count()); // if it didn't work we might get zero here
assert.eq(1, db.sps.count(), "join problem?");

// test with a throw
join = startParallelShell("db.sps.insert({x:1}); throw 'intentionally_uncaught';");
join();
assert.eq(2, db.sps.count(), "join2 problem?");

print("shellstartparallel.js SUCCESS");

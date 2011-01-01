
// verify that join works
db.sps.drop();
join = startParallelShell("sleep(1000); db.sps.insert({x:1});");
join();
// print(db.sps.count()); // if it didn't work we might get zero here
assert(db.sps.count() == 1, "join problem?");

// test with a throw
join = startParallelShell("db.sps.insert({x:1}); throw 'intentionally_uncaught';");
join();
assert(db.sps.count() == 2, "join2 problem?");

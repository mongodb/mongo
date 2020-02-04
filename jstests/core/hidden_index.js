var rsTest = new ReplSetTest({ nodes: 2 });
rsTest.startSet({ oplogSize: 1000  });
rsTest.initiate();
rsTest.awaitSecondaryNodes();


var primary = rsTest.getPrimary();
var secondary = rsTest.getSecondary();
secondary.setSlaveOk();

var testtab = primary.getDB("test").hidden_index_tab;
var testtabsec = secondary.getDB("test").hidden_index_tab;

testtab.createIndex({a: 1});
testtab.createIndex({b: 1}, {hidden: true});

var res = testtab.find({a: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.inputStage.stage, "IXSCAN");
res = testtab.find({b: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.stage, "COLLSCAN");
sleep(1000);


res = testtabsec.find({a: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.inputStage.stage, "IXSCAN");
res = testtabsec.find({b: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.stage, "COLLSCAN");


testtab.hiddenIndex("a_1");
testtab.unhiddenIndex({b: 1});

res = testtab.find({a: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.stage, "COLLSCAN");
res = testtab.find({b: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.inputStage.stage, "IXSCAN");
sleep(1000);

res = testtabsec.find({a: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.stage, "COLLSCAN");
res = testtabsec.find({b: 1}).explain()
assert.eq(res.queryPlanner.winningPlan.inputStage.stage, "IXSCAN");


rsTest.stopSet();

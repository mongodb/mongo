// SERVER-2829 Test arrays matching themselves within a $in expression.

t = db.jstests_in8;
t.drop();

t.save({key: [1]});
t.save({key: ['1']});
t.save({key: [[2]]});

function doTest() {
    assert.eq(1, t.count({key: [1]}));
    assert.eq(1, t.count({key: {$in: [[1]]}}));
    assert.eq(1, t.count({key: {$in: [[1]], $ne: [2]}}));
    assert.eq(1, t.count({key: {$in: [['1']], $type: 2}}));
    assert.eq(1, t.count({key: ['1']}));
    assert.eq(1, t.count({key: {$in: [['1']]}}));
    assert.eq(1, t.count({key: [2]}));
    assert.eq(1, t.count({key: {$in: [[2]]}}));
}

doTest();
t.ensureIndex({key: 1});
doTest();

db = db.getSiblingDB('splits');

function setUp(db){
    dropColl(db);
}


function runAggregate(db, splitter){
    splitter = typeof splitter !== 'undefined' ? splitter: '|';
    var result = db.strings.aggregate( {$project: { _id:0, splited: {$split: ["$text", splitter]}}});
    return result.result[0];
};

function test1(db){
    db.strings.insert({'text': 'a|b'});
    var expected = {splited: ['a','b']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test1 failed');
    dropColl(db);
};

function test2(db){
    db.strings.insert({'text': '|ab'});
    var expected = {splited: ['','ab']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test2 failed');
    dropColl(db);
};

function test3(db){
    db.strings.insert({'text': 'ab|'});
    var expected = {splited: ['ab','']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test3 failed');
    dropColl(db);
};

function test4(db){
    db.strings.insert({'text': 'ab'});
    var expected = {splited: ['ab']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test4 failed');
    dropColl(db);
};

function test5(db){
    db.strings.insert({'text': ''});
    var expected = {splited: ['']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test5 failed');
    dropColl(db);
};


function test6(db){
    db.strings.insert({'text': null});
    var expected = {splited: ['']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test6 failed');
    dropColl(db);
};

function test7(db){
    db.strings.insert({'text': '||'});
    var expected = {splited: ['','','']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test7 failed');
    dropColl(db);
};

function test8(db){
    db.strings.insert({'text': '|a|'});
    var expected = {splited: ['','a','']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test8 failed');
    dropColl(db);
};

function test9(db){
    db.strings.insert({'text': 'a||b|'});
    var expected = {splited: ['a','', 'b','']};
    var result = runAggregate(db);
    assert.eq( expected, result, 'test9 failed');
    dropColl(db);
};

function test10(db){
    db.strings.insert({'text': 'a||b|'});
    var expected = {splited: ['a','b|']};
    var result = runAggregate(db, '||');
    assert.eq( expected, result, 'test10 failed');
    dropColl(db);
};

function test11(db){
    db.strings.insert({'text': 'ab'});
    var expected = {splited: ['ab']};
    var result = runAggregate(db, 'abc');
    assert.eq( expected, result, 'test11 failed');
    dropColl(db);
};

function test12(db){
    db.strings.insert({'text': 'a,b', 'splitable': ','});
    var expected = {splited: ['a','b']};
    var result = runAggregate(db, '$splitable');
    assert.eq( expected, result, 'test12 failed');
    dropColl(db);
};

function dropColl(db){
    db.strings.drop();
};



setUp(db);

test1(db);
test2(db);
test3(db);
test4(db);
test5(db);
test6(db);
test7(db);
test8(db);
test10(db);
test11(db);
test12(db);

//cleanUp();

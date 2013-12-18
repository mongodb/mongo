// SERVER-10651 MySQL's DATE_ADD DATE_SUB like functions for $project

load('jstests/aggregation/extras/utils.js');

// Fri, 30 Aug 2013 16:10:40 GMT in millis
var millis = 1377879040000;
// 15 seconds
var num = 15 * 1000;
// 10 Days in millis
var tenDays = 10 * 24 * 60 * 60 * 1000;
// 10 months after the inserted date
var tenMonthsLater = new Date(1404144640000);
// 10 months before the inserted date
var tenMonthsBefore = new Date(1351613440000);

// Clear db
db.s10651.drop();

// Populate db
db.s10651.save({date: new Date(millis), num: num});

function test(expression, expected) {
    var res = db.s10651.aggregate({$project: {out: expression}});
    assert.commandWorked(res, tojson(expression));
    assert.eq(res.result[0].out, expected, tojson(expression));
}
function fail(expression, code) {
    assertErrorCode(db.s10651, {$project: {out: expression}}, code);
}


test({$dateAdd: ['$date', '$num']}, new Date(millis + num));
test({$dateAdd: ['$date', [15, "seconds"]]}, new Date(millis + num));
test({$dateAdd: ['$date', [10, "days"]]}, new Date(millis + tenDays));
test({$dateAdd: ['$date', [10, "months"]]}, tenMonthsLater);

test({$dateSub: ['$date', '$num']}, new Date(millis - num));
test({$dateSub: ['$date', [15, "seconds"]]}, new Date(millis - num));
test({$dateSub: ['$date', [10, "days"]]}, new Date(millis - tenDays));
test({$dateSub: ['$date', [10, "months"]]}, tenMonthsBefore);

// Test fails
fail({$dateAdd: ['$date', '$date']}, 17092);
fail({$dateAdd: []}, 17106);
fail({$dateAdd: [millis, millis]}, 17095);
fail({$dateAdd: ['$date', "5 minutes"]}, 17094);
fail({$dateAdd: ['$date', [1]]}, 17104);
fail({$dateAdd: ['$date', ["10", "minutes"]]}, 17090);
fail({$dateAdd: ['$date', [10, 3]]}, 17093);

fail({$dateSub: ['$date', '$date']}, 17102);
fail({$dateSub: []}, 17105);
fail({$dateSub: [millis, millis]}, 17096);
fail({$dateSub: ['$date', "5 minutes"]}, 17101);
fail({$dateSub: ['$date', [1]]}, 17103);
fail({$dateSub: ['$date', ["10", "minutes"]]}, 17098);
fail({$dateSub: ['$date', [10, 3]]}, 17099);

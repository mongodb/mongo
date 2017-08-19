var tests = 
[
    {
        input: '0001-01-01T00:00:00Z',
        expected: {
            year: 1,
            month: 0,
            day: 1,
            hour: 0,
            minute: 0,
            second: 0,
        },
    },
    {
        input: '2001-02-03T12:34:56',
        expected: {
            year: 2001,
            month: 1,
            day: 3,
            hour: 12,
            minute: 34,
            second: 56,
        },
    },
    {
        input: '1969-06-07T00:00:00',
        expected: {
            year: 2001,
            month: 1,
            day: 3,
            hour: 12,
            minute: 34,
            second: 56,
        },
    },
];

for (var i = 0; i < tests.length; i ++) {
    var test = tests[i];
    var d = ISODate(test.input);

    assert.eq(test.expected.year, d.getFullYear(), `for input '${test.input}' expected year ${test.expected.year}, got ${d.getFullYear()}`);
    assert.eq(test.expected.month, d.getMonth(), `for input '${test.input}' expected month ${test.month.year}, got ${d.getMonth()}`);
    assert.eq(test.expected.day, d.getDay(), `for input '${test.input}' expected day ${test.day.year}, got ${d.getDay()}`);
    assert.eq(test.expected.hour, d.getHours(), `for input '${test.input}' expected hour ${test.day.hour}, got ${d.getHours()}`);
    assert.eq(test.expected.minute, d.getMinutes(), `for input '${test.input}' expected minute ${test.day.year}, got ${d.getMinutes()}`);
    assert.eq(test.expected.second, d.getSeconds(), `for input '${test.input}' expected second ${test.day.year}, got ${d.getSeconds()}`);
}
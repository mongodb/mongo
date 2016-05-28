// Find the decimal using query operators

(function() {
    'use strict';
    var col = db.decimal_find_query;
    col.drop();

    // Insert some sample data.

    assert.writeOK(col.insert([
        {'decimal': NumberDecimal('0')},
        {'decimal': NumberDecimal('0.00')},
        {'decimal': NumberDecimal('-0')},
        {'decimal': NumberDecimal('1.0')},
        {'decimal': NumberDecimal('1.00')},
        {'decimal': NumberDecimal('2.00')},
        {'decimal': NumberDecimal('12345678901234.56789012345678901234')},
        {'decimal': NumberDecimal('NaN')},
        {'decimal': NumberDecimal('-NaN')},
        {'decimal': NumberDecimal('Infinity')},
        {'decimal': NumberDecimal('-Infinity')},
    ]),
                   'Initial insertion failed');

    assert.eq(col.find({'decimal': {$eq: NumberDecimal('1')}}).count(), '2');
    assert.eq(col.find({'decimal': {$lt: NumberDecimal('1.00000000000001')}}).count(), 6);
    assert.eq(col.find({'decimal': {$gt: NumberDecimal('1.5')}}).count(), 3);

    assert.eq(col.find({'decimal': {$gte: NumberDecimal('2.000')}}).count(), 3);
    assert.eq(col.find({'decimal': {$lte: NumberDecimal('0.9999999999999999')}}).count(), 4);

    assert.eq(col.find({'decimal': {$nin: [NumberDecimal('Infinity'), NumberDecimal('-Infinity')]}})
                  .count(),
              9,
              'Infinity count incorrect');

    // Test $mod
    col.drop();
    assert.writeOK(col.insert([
        {'decimal': NumberDecimal('0')},
        {'decimal': NumberDecimal('0.00')},
        {'decimal': NumberDecimal('-0')},
        {'decimal': NumberDecimal('1.0')},
        {'decimal': NumberDecimal('1.00')},
        {'decimal': NumberDecimal('2.00')},
    ]),
                   '2 insertion failed');
    assert.eq(col.find({'decimal': {$mod: [2, 0]}}).count(), 4, "$mod count incorrect");
}());

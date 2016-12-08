(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  var dbName = 'test';
  var toolTest = getToolTest('views');
  var db = toolTest.db.getSiblingDB(dbName);
  var commonToolArgs = getCommonToolArguments();

  var exportTarget = 'views_export';
  removeFile(exportTarget);

  function addCitiesData() {
    db.cities.insertMany([{
      city: 'Boise',
      state: 'ID',
    }, {
      city: 'Pocatello',
      state: 'ID',
    }, {
      city: 'Nampa',
      state: 'ID',
    }, {
      city: 'Albany',
      state: 'NY',
    }, {
      city: 'New York',
      state: 'NY',
    }, {
      city: 'Los Angeles',
      state: 'CA',
    }, {
      city: 'San Jose',
      state: 'CA',
    }, {
      city: 'Cupertino',
      state: 'CA',
    }, {
      city: 'San Francisco',
      state: 'CA',
    }]);
  }

  function addStateView(state) {
    db.createCollection('cities'+state, {
      viewOn: 'cities',
      pipeline: [{
        $match: {state: state},
      }],
    });
  }

  addCitiesData();
  addStateView('ID');
  addStateView('NY');
  addStateView('CA');

  assert.eq(9, db.cities.count(), 'should have 9 cities');
  assert.eq(3, db.citiesID.count(), 'should have 3 cities in Idaho view');
  assert.eq(2, db.citiesNY.count(), 'should have 2 cities in New York view');
  assert.eq(4, db.citiesCA.count(), 'should have 4 cities in California view');

  var ret;

  ret = toolTest.runTool.apply(toolTest, ['export', '-o', exportTarget, '-d', dbName, '-c' , 'citiesCA']
      .concat(commonToolArgs));
  assert.eq(0, ret, 'export should succeed');

  db.dropDatabase();

  ret = toolTest.runTool.apply(toolTest, ['import', exportTarget, '-d', dbName, '-c' , 'CACities']
      .concat(commonToolArgs));
  assert.eq(0, ret, 'export should succeed');

  assert.eq(4, db.CACities.count(), 'restored view should have correct number of rows');

  removeFile(exportTarget);
  toolTest.stop();
}());

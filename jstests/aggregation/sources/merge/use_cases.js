/**
 * Tests a practical use case for $merge from a collection of samples to an hourly rollup output
 * collection.
 *
 * @tags: [requires_sharding]
 */
(function() {
    "use strict";

    Random.setRandomSeed();

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s.getDB("use_cases");

    const metricsColl = mongosDB["metrics"];
    const rollupColl = mongosDB["rollup"];

    function incDateByMinutes(date, mins) {
        return new Date(date.getTime() + (60 * 1000 * mins));
    }

    // Inserts 'nSamples' worth of random data starting at 'date'.
    function insertRandomData(coll, date, nSamples) {
        let ticksSum = 0, tempSum = 0;
        let bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nSamples; i++) {
            const randTick = Random.randInt(100);
            const randTemp = Random.randInt(100);
            ticksSum += randTick;
            tempSum += randTemp;
            bulk.insert({
                _id: incDateByMinutes(date, i * (60 / nSamples)),
                ticks: randTick,
                temp: randTemp
            });
        }
        assert.commandWorked(bulk.execute());

        return [ticksSum, tempSum];
    }

    // Runs a $merge aggregate on the metrics collection to the rollup collection, grouping by hour,
    // summing the ticks, and averaging the temps.
    function runAggregate({startDate, whenMatchedMode, whenNotMatchedMode}) {
        metricsColl.aggregate([
            {$match: {_id: {$gte: startDate}}},
            {
              $group: {
                  _id: {$dateToString: {format: "%Y-%m-%dT%H", date: "$_id"}},
                  ticks: {$sum: "$ticks"},
                  avgTemp: {$avg: "$temp"},
              }
            },
            {
              $merge: {
                  into: {db: rollupColl.getDB().getName(), coll: rollupColl.getName()},
                  whenMatched: whenMatchedMode,
                  whenNotMatched: whenNotMatchedMode
              }
            }
        ]);
    }

    // Shard the metrics (source) collection on _id, which is the date of the sample.
    const hourZero = new ISODate("2018-08-15T00:00:00.000Z");
    const hourOne = incDateByMinutes(hourZero, 60);
    st.shardColl(metricsColl, {_id: 1}, {_id: hourOne}, {_id: hourOne}, mongosDB.getName());

    // Insert sample documents into the metrics collection.
    const samplesPerHour = 10;
    let [ticksSum, tempSum] = insertRandomData(metricsColl, hourZero, samplesPerHour);

    runAggregate({startDate: hourZero, whenMatchedMode: "fail", whenNotMatchedMode: "insert"});

    // Verify the results of the $merge in the rollup collection.
    let res = rollupColl.find().sort({_id: 1});
    assert.eq([{_id: "2018-08-15T00", ticks: ticksSum, avgTemp: tempSum / samplesPerHour}],
              res.toArray());

    // Insert another hour's worth of data, and verify that the $merge will append the result to the
    // output collection.
    [ticksSum, tempSum] = insertRandomData(metricsColl, hourOne, samplesPerHour);

    runAggregate({startDate: hourOne, whenMatchedMode: "fail", whenNotMatchedMode: "insert"});

    res = rollupColl.find().sort({_id: 1}).toArray();
    assert.eq(2, res.length);
    assert.eq(res[1], {_id: "2018-08-15T01", ticks: ticksSum, avgTemp: tempSum / samplesPerHour});

    // Whoops, there was a mistake in the last hour of data. Let's re-run the aggregation and update
    // the rollup collection using the "replace".
    assert.commandWorked(metricsColl.update({_id: hourOne}, {$inc: {ticks: 10}}));
    ticksSum += 10;

    runAggregate({startDate: hourOne, whenMatchedMode: "replace", whenNotMatchedMode: "insert"});

    res = rollupColl.find().sort({_id: 1}).toArray();
    assert.eq(2, res.length);
    assert.eq(res[1], {_id: "2018-08-15T01", ticks: ticksSum, avgTemp: tempSum / samplesPerHour});

    // Shard the output collection into 2 chunks, and make the split hour 6.
    const hourSix = incDateByMinutes(hourZero, 60 * 6);
    st.shardColl(rollupColl, {_id: 1}, {_id: hourSix}, {_id: hourSix}, mongosDB.getName());

    // Insert hour 7 data into the metrics collection and re-run the aggregation.
    [ticksSum, tempSum] = insertRandomData(metricsColl, hourSix, samplesPerHour);

    runAggregate({startDate: hourSix, whenMatchedMode: "fail", whenNotMatchedMode: "insert"});

    res = rollupColl.find().sort({_id: 1}).toArray();
    assert.eq(3, res.length, tojson(res));
    assert.eq(res[2], {_id: "2018-08-15T06", ticks: ticksSum, avgTemp: tempSum / samplesPerHour});

    st.stop();
}());

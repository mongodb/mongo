(function() {
    "use strict";
    var name = "large_set";
    var replTest = new ReplSetTest({name: name,
                                    nodes: 30,
                                    oplogSize: 5,
                                    nodeOptions: {smallfiles:""} });
    var host = getHostName();

    var nodes = replTest.startSet();
    replTest.initiate({_id : name, members : [
        {_id : 0, host : host+":"+replTest.ports[0]},
        {_id : 1, host : host+":"+replTest.ports[1]},
        {_id : 2, host : host+":"+replTest.ports[2]},
        {_id : 3, host : host+":"+replTest.ports[3]},
        {_id : 4, host : host+":"+replTest.ports[4]},
        {_id : 5, host : host+":"+replTest.ports[5], "arbiterOnly": true},
        {_id : 6, host : host+":"+replTest.ports[6], "arbiterOnly": true},
        {_id : 7, host : host+":"+replTest.ports[7], "votes": 0},
        {_id : 8, host : host+":"+replTest.ports[8], "votes": 0},
        {_id : 9, host : host+":"+replTest.ports[9], "votes": 0},
        {_id : 10, host : host+":"+replTest.ports[10], "votes": 0},
        {_id : 11, host : host+":"+replTest.ports[11], "votes": 0},
        {_id : 12, host : host+":"+replTest.ports[12], "votes": 0},
        {_id : 13, host : host+":"+replTest.ports[13], "votes": 0},
        {_id : 14, host : host+":"+replTest.ports[14], "votes": 0},
        {_id : 15, host : host+":"+replTest.ports[15], "votes": 0},
        {_id : 16, host : host+":"+replTest.ports[16], "votes": 0},
        {_id : 17, host : host+":"+replTest.ports[17], "votes": 0},
        {_id : 18, host : host+":"+replTest.ports[18], "votes": 0},
        {_id : 19, host : host+":"+replTest.ports[19], "votes": 0},
        {_id : 20, host : host+":"+replTest.ports[20], "votes": 0},
        {_id : 21, host : host+":"+replTest.ports[21], "votes": 0},
        {_id : 22, host : host+":"+replTest.ports[22], "votes": 0},
        {_id : 23, host : host+":"+replTest.ports[23], "votes": 0},
        {_id : 24, host : host+":"+replTest.ports[24], "votes": 0},
        {_id : 25, host : host+":"+replTest.ports[25], "votes": 0},
        {_id : 26, host : host+":"+replTest.ports[26], "votes": 0},
        {_id : 27, host : host+":"+replTest.ports[27], "votes": 0},
        {_id : 28, host : host+":"+replTest.ports[28], "votes": 0},
        {_id : 29, host : host+":"+replTest.ports[29], "votes": 0}
    ]});

    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    var master = replTest.getMaster();
    assert.writeOK(master.getDB("test").foo.save({ a: "test" },
                                                 { writeConcern: { w: 'majority' }}));

    replTest.stop(2);
    replTest.stop(3);
    replTest.stop(4);
    jsTest.log("3 voting nodes taken down; attempting impossible, majority writeConcern write");
    assert.writeError(master.getDB("test").foo.save({ a: "test" },
                                                    { writeConcern: { w: 'majority',
                                                                      wtimeout: 10*1000 }}));
    replTest.stopSet();
}());

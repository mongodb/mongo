
SlowWeeklyMongod = function(name) {
    this.name = name;
    this.start = new Date();

    this.conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    this.port = this.conn.port;
};

SlowWeeklyMongod.prototype.getDB = function(name) {
    return this.conn.getDB(name);
};

SlowWeeklyMongod.prototype.stop = function() {
    MongoRunner.stopMongod(this.conn);
    var end = new Date();
    print("slowWeekly test: " + this.name + " completed successfully in " +
          ((end.getTime() - this.start.getTime()) / 1000) + " seconds");
};

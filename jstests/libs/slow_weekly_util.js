
SlowWeeklyBongod = function(name) {
    this.name = name;
    this.start = new Date();

    this.conn = BongoRunner.runBongod({smallfiles: "", nojournal: ""});
    this.port = this.conn.port;
};

SlowWeeklyBongod.prototype.getDB = function(name) {
    return this.conn.getDB(name);
};

SlowWeeklyBongod.prototype.stop = function() {
    BongoRunner.stopBongod(this.conn);
    var end = new Date();
    print("slowWeekly test: " + this.name + " completed successfully in " +
          ((end.getTime() - this.start.getTime()) / 1000) + " seconds");
};

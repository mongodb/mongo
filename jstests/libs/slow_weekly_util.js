
SlowWeeklyMongod = function( name ) {
    this.name = name;
    this.port = 30201;
    
    this.start = new Date();

    this.conn = MongoRunner.runMongod({port: this.port, smallfiles: "", nojournal: ""});
};

SlowWeeklyMongod.prototype.getDB = function( name ) {
    return this.conn.getDB( name );
}

SlowWeeklyMongod.prototype.stop = function(){
    MongoRunner.stopMongod( this.conn );
    var end = new Date();
    print( "slowWeekly test: " + this.name + " completed successfully in " + ( ( end.getTime() - this.start.getTime() ) / 1000 ) + " seconds" );
};


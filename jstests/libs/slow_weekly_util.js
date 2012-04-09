
SlowWeeklyMongod = function( name ) {
    this.name = name;
    this.port = 30201;
    
    this.start = new Date();

    this.conn = startMongodEmpty("--port", this.port, "--dbpath", "/data/db/" + this.name , "--smallfiles", "--nojournal" );
};

SlowWeeklyMongod.prototype.getDB = function( name ) {
    return this.conn.getDB( name );
}

SlowWeeklyMongod.prototype.stop = function(){
    stopMongod( this.port );
    var end = new Date();
    print( "slowWeekly test: " + this.name + " completed succesfully in " + ( ( end.getTime() - this.start.getTime() ) / 1000 ) + " seconds" );
};


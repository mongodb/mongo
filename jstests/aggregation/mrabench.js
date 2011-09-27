/*
  In order to run this, you need to have a local copy of the usage data.

  One way to do this is to dump and restore it using mongodump and mongorestore
  */

db = db.getSiblingDB( "mongousage" )

function rollupMap() {
    emit( this._id.t , { total : this.value , unique : 1 } )
}

function rollupReduce(key, values) {
    var res = { total : 0 , unique : 0 };
    for ( var i=0; i<values.length; i++ ){
        res.total += values[i].total;
        res.unique += values[i].unique;
    }
    return res;
}

function mrrollups() {

    res = db.gen.monthly.ip.mapReduce( rollupMap , rollupReduce ,
				       { out : "gen.monthly" } )
    res.find().sort( { _id : -1 } ).forEach( printjsononeline )

    res = db.gen.weekly.ip.mapReduce( rollupMap , rollupReduce ,
				      { out : "gen.weekly" } )
    res.find().sort( { _id : -1 } ).forEach( printjsononeline )
}

function rollupMonthlyMR() {
    resMonthlyMR = db.gen.monthly.ip.mapReduce( rollupMap , rollupReduce ,
						{ out: { inline : 1 }} )
}

function rollupWeeklyMR() {
    resWeeklyMR = db.gen.weekly.ip.mapReduce( rollupMap , rollupReduce ,
					      { out : {inline : 1 }} )
}

function rollupMonthlyA() {
    resMonthlyA = db.runCommand( { aggregate: "gen.monthly.ip", pipeline : [
	{ $group : {
	    _id : { month: "_id.t" },
	    total : { $sum : "$value" },
	    unique : { $sum : 1 }
	}}
    ]});
}

function rollupWeeklyA() {
    resWeeklyA = db.runCommand( { aggregate: "gen.weekly.ip", pipeline : [
	{ $group : {
	    _id : { month: "_id.t" },
	    total : { $sum : "$value" },
	    unique : { $sum : 1 }
	}}
    ]});
}

/* sample aggregate command queries */

// make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSisterDB("mydb");

// just passing through fields
var p1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	tags : 1,
	pageViews : 1
    }}
]});

// unwinding an array
var p2 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }}
]});

// pulling values out of subdocuments
var p3 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	otherfoo : "other.foo",
	otherbar : "other.bar"
    }}
]});

// projection includes a computed value
var p4 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	daveWroteIt : { $eq:["$author", "dave"] }
    }}
]});

// projection includes a virtual (fabricated) document
var p5 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	pageViews : 1,
	tag : { $unwind : "tags" }
    }},
    { $project : {
	author : 1,
	subDocument : { foo : "pageViews", bar : "tag"  }
    }}
]});

// multi-step aggregate
// nested expressions in computed fields
var p6 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $project : {
	author : 1,
	tag : 1,
	pageViews : 1,
	daveWroteIt : { $eq:["$author", "dave"] },
	weLikeIt : { $or:[ { $eq:["$author", "dave"] },
			   { $eq:["$tag", "good"] } ] }
    }}
]});

// slightly more complex computed expression; $ifnull
var p7 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	theSum : { $add:["$pageViews",
			 { $ifnull:["$other.foo",
				    "$other.bar"] } ] }
    }}
]});

// dotted path inclusion; _id exclusion
var p8 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	_id : 0,
	author : 1,
	tag : { $unwind : "tags" },
	"comments.author" : 1
    }}
]});


// simple matching
var m1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $match : { author : "dave" } }
]});

// combining matching with a projection
var m2 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	title : 1,
	author : 1,
	pageViews : 1,
	tag : { $unwind : "tags" },
	comments : 1
    }},
    { $match : { tag : "nasty" } }
]});


// group by tag
var g1 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $group : {
	_id: { tag : 1 },
	docsByTag : { $sum : 1 },
	viewsByTag : { $sum : "$pageViews" }
    }}
]});

// $max, and averaging in a final projection
var g2 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $group : {
	_id: { tag : 1 },
	docsByTag : { $sum : 1 },
	viewsByTag : { $sum : "$pageViews" },
	mostViewsByTag : { $max : "$pageViews" },
    }},
    { $project : {
	_id: false,
	tag : "_id.tag",
	mostViewsByTag : 1,
	docsByTag : 1,
	viewsByTag : 1,
	avgByTag : { $divide:["$viewsByTag", "$docsByTag"] }
    }}
]});

// $push as an accumulator; can pivot data
var g3 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" }
    }},
    { $group : {
	_id : { tag : 1 },
	authors : { $push : "$author" }
    }}
]});

// $avg, and averaging in a final projection
var g4 = db.runCommand(
{ aggregate : "article", pipeline : [
    { $project : {
	author : 1,
	tag : { $unwind : "tags" },
	pageViews : 1
    }},
    { $group : {
	_id: { tag : 1 },
	docsByTag : { $sum : 1 },
	viewsByTag : { $sum : "$pageViews" },
	avgByTag : { $avg : "$pageViews" },
    }}
]});

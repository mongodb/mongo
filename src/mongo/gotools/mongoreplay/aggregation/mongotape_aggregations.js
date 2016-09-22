//mongoreplay_aggregations.js


//max_latency_per_type returns an aggregation pipeline to view the average and max latency for each type in a 
//mongoreplay report. Latency is calculated as the amount of time between when an operation is played and when it completes
function max_latency_per_type(){
  return  [
    {$project:
      {
        "optype":
          { $ifNull :
            [
              {$concat : ["cmd_", "$command"]},
              {$concat: ["op_", "$op"]}
            ]
          },
        "latency_us": 1
      }
    },
    {$group :
      {
        _id: "$optype",
        "max_latency": {$max : "$latency_us"}, 
		"avg_latency": {$avg: "$latency_us"}
      }
    }
  ]
}


//average_and_peak_lag returns an aggregation pipeline to view the average and max lag for a mongoreplay report
//Lag is calculated as the amount of time between when an operation is scheduled to be played as per the playback information
//and when it actually is played in mongoreplay
function average_and_peak_lag() {
  return [
    {$group: {
      _id: null,
      "average_lag_us": {$avg : "$playbacklag_us"},
      "max_lag_us": {$max: "$playbacklag_us"}
      }
    },
    {$project : {
        "_id" :0,
        "average_lag_us" :1,
        "max_lag_us": 1,
      }
    }
  ]
}

//latency_delta returns an aggregation pipeline to view the difference in latency in two mongoreplay reports
//The 'other_collection' is the collection you wish to take a delta against
function latency_delta(other_collection) {
  return [
    {$lookup :
      {
        from: other_collection,
        localField: "order",
        foreignField: "order",
        as: "join",
      }
    },
    {$unwind: '$join'},
    {$project:
      {
        order: 1,
        latencyDiff : { $subtract: ["$latency_us", "$join.latency_us"]},
        op: 1,
		command: 1
      }
    },
    {$sort:
      {
        latencyDiff: -1
      }
    }
  ]
}


//latency_delta returns an aggregation pipeline to view the difference in errors in two mongoreplay reports
//This pipeline returns all ops where the errors in one report do not match the errors in another
//The 'other_collection' is the collection you wish to take a diff against
function errors_diff(other_collection) {
  return [
    { $lookup :
      {
        from: other_collection,
        localField: "order",
        foreignField: "order",
        as: "join",
      }
    },
    {$unwind: '$join'},
    {$project:
      {
        original_errors:"$errors",
        replayed_errors:"$join.errors",
        order: 1,
        type: 1
      }
    },
    {$match:
      {
        replayed_errors: {$ne: "$original_errors"},
		$or :
		  [
		  {replayed_errors: {$exists: true}},  
		  {original_errors: {$exists: true}}
		  ]
      }
    }
  ]
}
//average_latency_compare returns a pipeline to view the average latency of each optype in different mongoreplay reports
//the 'other_collection' is the collection you wish to compare a different collection to 
function average_latency_compare(other_collection) {
  return [
    { $lookup :
      {
        from: other_collection,
        localField: "order",
        foreignField: "order",
        as: "join",
      }
    },
    {$unwind : '$join'},
    {$project:
      {
        optype:
          { $ifNull : [
              {$concat : ["cmd_", "$command"]},
              {$concat: ["op_", "$op"]}
            ]
          },
        original_latency_us: "$latency_us",
        replayed_latency_us: "$join.latency_us",
        order: 1

      }
    },
    {$group :
      {
        _id : "$optype",
        avg_original_latency_us : { $avg: '$original_latency_us'},
        avg_replayed_latency_us : { $avg: '$replayed_latency_us'}
      }
    }
  ]
}

//getStats executes an aggregation query using the pipeline function specified.
//it is a helper function to execute any of the mongoreplay statics functions
//the aggregation_func is a function that returns an aggregation pipeline and collection1 and collection2 are string names of the 
//collections you wish to input to your pipeline. Collection2 may be optional depending on the aggregation_func you use
function getStats(aggregation_func, collection1, collection2) {
  var aggregation = aggregation_func(collection2)
  var coll = db.getCollection(collection1)
  return coll.aggregate(aggregation)
}

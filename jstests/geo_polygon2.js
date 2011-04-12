//
// More tests for N-dimensional polygon querying
//

// Create a polygon of some shape (no holes)
// using turtle graphics.  Basically, will look like a very contorted octopus (quad-pus?) shape.
// There are no holes, but some edges will probably touch.
// TODO:  Rotate as well, to check?

var numTests = 5

for ( var test = 0; test < numTests; test++ ) {

	var numTurtles = 4;
	var gridSize = [ 8, 8 ];
	var turtleSteps = 30;

	Random.srand( 1337 + test );

	var grid = []
	for ( var i = 0; i < gridSize[0]; i++ ) {
		grid.push( new Array( gridSize[1] ) )
	}

	grid.toString = function() {

		var gridStr = "";
		for ( var j = grid[0].length - 1; j >= -1; j-- ) {
			for ( var i = 0; i < grid.length; i++ ) {
				if ( i == 0 )
					gridStr += ( j == -1 ? " " : j ) + ": "
				if ( j != -1 )
					gridStr += "[" + ( grid[i][j] != undefined ? grid[i][j] : " " ) + "]"
				else
					gridStr += " " + i + " "
			}
			gridStr += "\n"
		}

		return gridStr;
	}

	var turtles = []
	for ( var i = 0; i < numTurtles; i++ ) {

		var up = ( i % 2 == 0 ) ? i - 1 : 0;
		var left = ( i % 2 == 1 ) ? ( i - 1 ) - 1 : 0;

		turtles[i] = [
				[ Math.floor( gridSize[0] / 2 ), Math.floor( gridSize[1] / 2 ) ],
				[ Math.floor( gridSize[0] / 2 ) + left, Math.floor( gridSize[1] / 2 ) + up ] ];

		grid[turtles[i][1][0]][turtles[i][1][1]] = i

	}

	grid[Math.floor( gridSize[0] / 2 )][Math.floor( gridSize[1] / 2 )] = "S"

	// print( grid.toString() )

	var pickDirections = function() {

		var up = Math.floor( Random.rand() * 3 )
		if ( up == 2 )
			up = -1

		if ( up == 0 ) {
			var left = Math.floor( Random.rand() * 3 )
			if ( left == 2 )
				left = -1
		} else
			left = 0

		if ( Random.rand() < 0.5 ) {
			var swap = left
			left = up
			up = swap
		}

		return [ left, up ]
	}

	for ( var s = 0; s < turtleSteps; s++ ) {

		for ( var t = 0; t < numTurtles; t++ ) {

			var dirs = pickDirections()
			var up = dirs[0]
			var left = dirs[1]

			var lastTurtle = turtles[t][turtles[t].length - 1]
			var nextTurtle = [ lastTurtle[0] + left, lastTurtle[1] + up ]

			if ( nextTurtle[0] >= gridSize[0] || nextTurtle[1] >= gridSize[1] || nextTurtle[0] < 0 || nextTurtle[1] < 0 )
				continue;

			if ( grid[nextTurtle[0]][nextTurtle[1]] == undefined ) {
				turtles[t].push( nextTurtle )
				grid[nextTurtle[0]][nextTurtle[1]] = t;
			}

		}
	}

	// print( grid.toString() )

	turtlePaths = []
	for ( var t = 0; t < numTurtles; t++ ) {

		turtlePath = []

		var nextSeg = function(currTurtle, prevTurtle) {

			var pathX = currTurtle[0]

			if ( currTurtle[1] < prevTurtle[1] ) {
				pathX = currTurtle[0] + 1
				pathY = prevTurtle[1]
			} else if ( currTurtle[1] > prevTurtle[1] ) {
				pathX = currTurtle[0]
				pathY = currTurtle[1]
			} else if ( currTurtle[0] < prevTurtle[0] ) {
				pathX = prevTurtle[0]
				pathY = currTurtle[1]
			} else if ( currTurtle[0] > prevTurtle[0] ) {
				pathX = currTurtle[0]
				pathY = currTurtle[1] + 1
			}

			// print( " Prev : " + prevTurtle + " Curr : " + currTurtle + " path
			// : "
			// + [pathX, pathY]);

			return [ pathX, pathY ]
		}

		for ( var s = 1; s < turtles[t].length; s++ ) {

			currTurtle = turtles[t][s]
			prevTurtle = turtles[t][s - 1]

			turtlePath.push( nextSeg( currTurtle, prevTurtle ) )

		}

		for ( var s = turtles[t].length - 2; s >= 0; s-- ) {

			currTurtle = turtles[t][s]
			prevTurtle = turtles[t][s + 1]

			turtlePath.push( nextSeg( currTurtle, prevTurtle ) )

		}

		// printjson( turtlePath )

		// End of the line is not inside our polygon.
		var lastTurtle = turtles[t][turtles[t].length - 1]
		grid[lastTurtle[0]][lastTurtle[1]] = undefined

		fixedTurtlePath = []
		for ( var s = 1; s < turtlePath.length; s++ ) {

			if ( turtlePath[s - 1][0] == turtlePath[s][0] && turtlePath[s - 1][1] == turtlePath[s][1] )
				continue;

			var up = turtlePath[s][1] - turtlePath[s - 1][1]
			var right = turtlePath[s][0] - turtlePath[s - 1][0]
			var addPoint = ( up != 0 && right != 0 )

			if ( addPoint && up != right ) {
				fixedTurtlePath.push( [ turtlePath[s][0], turtlePath[s - 1][1] ] )
			} else if ( addPoint ) {
				fixedTurtlePath.push( [ turtlePath[s - 1][0], turtlePath[s][1] ] )
			}

			fixedTurtlePath.push( turtlePath[s] )

		}

		// printjson( fixedTurtlePath )

		turtlePaths.push( fixedTurtlePath )

	}

	print( grid.toString() )

	var polygon = []
	for ( var t = 0; t < turtlePaths.length; t++ ) {
		for ( var s = 0; s < turtlePaths[t].length; s++ ) {
			polygon.push( turtlePaths[t][s] )
		}
	}

	printjson( polygon )

	t = db.polytest2
	t.drop()

	var pointsIn = 0
	var pointsOut = 0
	for ( var j = grid[0].length - 1; j >= 0; j-- ) {
		for ( var i = 0; i < grid.length; i++ ) {
			t.insert( { loc : [ i + 0.5, j + 0.5 ] } )
			if ( grid[i][j] != undefined )
				pointsIn++
			else
				pointsOut++
		}
	}

	t.ensureIndex( { loc : "2d" } )

	print( "Points in : " )
	print( t.find( { loc : { "$within" : { "$polygon" : polygon } } } ).count() )
	
	assert.eq( gridSize[0] * gridSize[1], t.find().count() )
	assert.eq( pointsIn, t.find( { loc : { "$within" : { "$polygon" : polygon } } } ).count() );

}

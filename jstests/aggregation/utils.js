/*
  Utility functions used to test aggregation
*/

function anyEq(al, ar, v) {
    if (typeof(v) == 'undefined')
	v = false;

    if (al instanceof Array) {
	if (!(ar instanceof Array)) {
	    if (v)
		print('anyEq: ar is not an array ' + ar);
	    return false;
	}

	if (!arrayEq(al, ar, v)) {
	    if (v)
		print('anyEq: arrayEq(al, ar): false; al=' + al + ' ar=' + ar);
	    return false;
	}
    } else if (al instanceof Object) {
	if (!(ar instanceof Object)) {
	    if (v)
		print('anyEq: ar is not an object ' + ar);
	    return false;
	}

	if (!documentEq(al, ar, v)) {
	    if (v)
		print('anyEq: documentEq(al, ar): false; al=' + al + ' ar=' + ar);
	    return false;
	}
    }
    else if (al != ar) {
	if (v)
	    print('anyEq: (al != ar): false; al=' + al + ' ar=' + ar);
	return false;
    }

    /* if we got here, they matched */
    return true;
}

/*
  Compare two documents for equality.  Returns true or false.  Only equal if
  they have the exact same set of properties, and all the properties' values
  match.
*/
function documentEq(dl, dr, v) {
    if (typeof(v) == 'undefined')
	v = false;

    /* make sure these are both objects */
    if (!(dl instanceof Object)) {
	if (v)
	    print('documentEq:  dl is not an object ' + dl);
	return false;
    }
    if (!(dr instanceof Object)) {
	if (v)
	    print('documentEq:  dr is not an object ' + dr);
	return false;
    }

    /* start by checking for all of dl's properties in dr */
    for(var propertyName in dl) {
	/* skip inherited properties */
	if (!dl.hasOwnProperty(propertyName))
	    continue;

	/* the documents aren't equal if they don't both have the property */
	if (!dr.hasOwnProperty(propertyName)) {
	    if (v)
		print('documentEq: dr doesn\'t have property ' + propertyName);
	    return false;
	}

	/* if the property is the _id, they don't have to be equal */
	if (propertyName == '_id')
	    continue;

	if (!anyEq(dl[propertyName], dr[propertyName], v)) {
	    return false;
	}
    }

    /* now make sure that dr doesn't have any extras that dl doesn't have */
    for(var propertyName in dr) {
	if (!dr.hasOwnProperty(propertyName))
	    continue;

	/* if dl doesn't have this complain; if it does, we compared it above */
	if (!dl.hasOwnProperty(propertyName)) {
	    if (v)
		print('documentEq: dl is missing property ' +
		      propertyName);
	    return false;
	}
    }

    /* if we got here, the two documents are an exact match */
    return true;
}

function arrayEq(al, ar, v) {
    if (typeof(v) == 'undefined')
	v = false;

    /* check that these are both arrays */
    if (!(al instanceof Array)) {
	if (v)
	    print('arrayEq: al is not an array: ' + al);
	return false;
    }

    if (!(ar instanceof Array)) {
	if (v)
	    print('arrayEq: ar is not an array: ' + ar);
	return false;
    }

    if (al.length != ar.length) {
	if (v)
	    print('arrayEq: array lengths do not match: ' + al +
		  ', ' + ar);
	return false;
    }

    var i = 0;
    var j = 0;
    while ( i < al.length ) {
        if (anyEq(al[i], ar[j], v) ) {
            j = 0;
            i++;
        } else if ( j < ar.length ) {
            j++;
        } else {
            return false;
        }
    }

    /* if we got here, the two objects matched */
    return true;
}

/*
  Make a shallow copy of an array.
*/
function arrayShallowCopy(a) {
    assert(a instanceof Array, 'arrayShallowCopy: argument is not an array');

    var c = [];
    for(var i = 0; i < a.length; ++i)
	c.push(a[i]);

    return c;
}

/*
  Compare two sets of documents (expressed as arrays) to see if they match.
  The two sets must have the same documents, although the order need not
  match.

  Are non-scalar values references?
*/
function resultsEq(rl, rr, v) {
    if (typeof(v) == 'undefined')
	v = false;

    /* make clones of the arguments so that we don't damage them */
    rl = arrayShallowCopy(rl);
    rr = arrayShallowCopy(rr);

    if (rl.length != rr.length) {
	if (v)
	    print('resultsEq:  array lengths do not match ' +
		  rl + ', ' + rr);
	return false;
    }

    for(var i = 0; i < rl.length; ++i) {
	var foundIt = false;

	/* find a match in the other array */
	for(var j = 0; j < rr.length; ++j) {
	    if (!anyEq(rl[i], rr[j], v))
		continue;
	    
	    /*
	      Because we made the copies above, we can edit these out of the
	      arrays so we don't check on them anymore.

	      For the inner loop, we're going to be skipping out, so we don't
	      need to be too careful.
	    */
	    rr.splice(j, 1);
	    foundIt = true;
	    break;
	    /* TODO */
	}

	if (!foundIt) {
	/* if we got here, we didn't find this item */
	    if (v)
		print('resultsEq: search target missing index:  ' + i);
	    return false;
	}
    }

    /* if we got here, everything matched */
    assert(!rr.length);
    return true;
}

function orderedArrayEq(al, ar, v) {
    if (al.length != ar.length) {
	if (v)
	    print('orderedArrayEq:  array lengths do not match ' +
		  al + ', ' + ar);
	return false;
    }

    /* check the elements in the array */
    for(var i = 0; i < al.length; ++i) {
	if (!anyEq(al[i], ar[i], v))
	    return false;
    }

    /* if we got here, everything matched */
    return true;
}


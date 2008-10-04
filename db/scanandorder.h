// scanandorder.h

#pragma once

/* todo:
   _ handle compound keys with differing directions.  we don't handle this yet: neither here nor in indexes i think!!!
   _ limit amount of data
*/

/* see also IndexDetails::getKeysFromObject, which needs some merging with this. */

class KeyType : boost::noncopyable {
public:
	JSObj pattern; // e.g., { ts : -1 }
public:
	KeyType(JSObj _keyPattern) {
		pattern = _keyPattern;
		assert( !pattern.isEmpty() );
	}

	// returns the key value for o
	JSObj getKeyFromObject(JSObj o) { 
		return o.extractFields(pattern);
	}
};

/* todo:
   _ respect limit
   _ check for excess mem usage
   _ response size limit from runquery; push it up a bit.
*/

inline bool fillQueryResultFromObj(BufBuilder& b, set<string> *filter, JSObj& js) { 
	if( filter ) {
		JSObj x;
		bool ok = x.addFields(js, *filter) > 0;
		if( ok ) 
			b.append((void*) x.objdata(), x.objsize());
		return ok;
	}

	b.append((void*) js.objdata(), js.objsize());
	return true;
}

typedef multimap<JSObj,JSObj> BestMap;
class ScanAndOrder { 
  BestMap best; // key -> full object
  int startFrom;
  int limit;   // max to send back.
  KeyType order;
  int dir;
  unsigned approxSize;
  
  void _add(JSObj& k, JSObj o) { 
    best.insert(make_pair(k,o));
  }
  
  // T may be iterator or reverse_iterator
  void _addIfBetter(JSObj& k, JSObj o, BestMap::iterator i) {
    const JSObj& worstBestKey = i->first;
    int c = worstBestKey.woCompare(k);
    if( (c<0 && dir<0) || (c>0&&dir>0) ) {
      // k is better, 'upgrade'
      best.erase(i);
      _add(k, o);
    }
  }

public:
 ScanAndOrder(int _startFrom, int _limit, JSObj _order) : 
  startFrom(_startFrom), order(_order) {
    limit = _limit > 0 ? _limit + startFrom : 0x7fffffff;
    approxSize = 0;
    
    // todo: do order right for compound keys.  this is temp.
    dir = 1;
    Element e = order.pattern.firstElement();
    if( e.type() == Number && e.number() < 0 ) {
      dir = -1;
    }
  }
  
  void add(JSObj o) { 
    JSObj k = order.getKeyFromObject(o);
    if( (int) best.size() < limit ) {
      approxSize += k.objsize();
      uassert( "too much key data for sort() with no index", approxSize < 1 * 1024 * 1024 );
      _add(k, o);
      return;
    }
    BestMap::iterator i;
    if( dir < 0 )
      i = best.begin();
    else { 
      assert( best.end() != best.begin() );
      i = best.end(); i--;
    }
    _addIfBetter(k, o, i);
  }
  
  template<class T>
    void _fill(BufBuilder& b, set<string> *filter, int& nout, T begin, T end) { 
    int n = 0;
    int nFilled = 0;
    for( T i = begin; i != end; i++ ) {
      n++;
      if( n <= startFrom )
	continue;
      JSObj& o = i->second;
      if( fillQueryResultFromObj(b, filter, o) ) { 
	nFilled++;
	if( nFilled >= limit )
	  goto done;
	uassert( "too much data for sort() with no index", b.len() < 4000000 ); // appserver limit
      }
    }
  done:
    nout = nFilled;
  }
  
  /* scanning complete. stick the query result in b for n objects. */
  void fill(BufBuilder& b, set<string> *filter, int& nout) { 
    //    for( BestMap::iterator i = best.begin(); i != best.end(); i++ )
    //      cout << "  fill:" << i->first.toString() << endl;
    //    for( BestMap::reverse_iterator i = best.rbegin(); i != best.rend(); i++ )
    //      cout << "  fillr:" << i->first.toString() << endl;
    if( dir > 0 )
      _fill(b, filter, nout, best.begin(), best.end());
    else
      _fill(b, filter, nout, best.rbegin(), best.rend());
  }
  
};

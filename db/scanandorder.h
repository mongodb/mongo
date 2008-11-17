/* scanandorder.h
   Order results (that aren't already indexes and in order.)
*/

/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

/* todo:
   _ handle compound keys with differing directions.  we don't handle this yet: neither here nor in indexes i think!!!
   _ limit amount of data
*/

/* see also IndexDetails::getKeysFromObject, which needs some merging with this. */

class KeyType : boost::noncopyable {
public:
	BSONObj pattern; // e.g., { ts : -1 }
public:
	KeyType(BSONObj _keyPattern) {
		pattern = _keyPattern;
		assert( !pattern.isEmpty() );
	}

	// returns the key value for o
	BSONObj getKeyFromObject(BSONObj o) { 
		return o.extractFields(pattern);
	}
};

/* todo:
   _ respect limit
   _ check for excess mem usage
   _ response size limit from runquery; push it up a bit.
*/

inline bool fillQueryResultFromObj(BufBuilder& b, set<string> *filter, BSONObj& js) { 
	if( filter ) {
		BSONObj x;
		bool ok = x.addFields(js, *filter) > 0;
		if( ok ) 
			b.append((void*) x.objdata(), x.objsize());
		return ok;
	}

	b.append((void*) js.objdata(), js.objsize());
	return true;
}

typedef multimap<BSONObj,BSONObj> BestMap;
class ScanAndOrder { 
  BestMap best; // key -> full object
  int startFrom;
  int limit;   // max to send back.
  KeyType order;
  int dir;
  unsigned approxSize;
  
  void _add(BSONObj& k, BSONObj o) { 
    best.insert(make_pair(k,o));
  }
  
  // T may be iterator or reverse_iterator
  void _addIfBetter(BSONObj& k, BSONObj o, BestMap::iterator i) {
    const BSONObj& worstBestKey = i->first;
    int c = worstBestKey.woCompare(k);
    if( (c<0 && dir<0) || (c>0&&dir>0) ) {
      // k is better, 'upgrade'
      best.erase(i);
      _add(k, o);
    }
  }

public:
 ScanAndOrder(int _startFrom, int _limit, BSONObj _order) : 
  startFrom(_startFrom), order(_order) {
    limit = _limit > 0 ? _limit + startFrom : 0x7fffffff;
    approxSize = 0;
    
    // todo: do order right for compound keys.  this is temp.
    dir = 1;
    BSONElement e = order.pattern.firstElement();
    if( e.number() < 0 ) {
      dir = -1;
    }
  }

  int size() const { return best.size(); }
  
  void add(BSONObj o) { 
    BSONObj k = order.getKeyFromObject(o);
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
      BSONObj& o = i->second;
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

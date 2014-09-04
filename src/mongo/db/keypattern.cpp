// @file keypattern.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/keypattern.h"

#include "mongo/db/hasher.h"
#include "mongo/db/index_names.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    KeyPattern::KeyPattern( const BSONObj& pattern ): _pattern( pattern ) {}

    bool KeyPattern::isIdKeyPattern(const BSONObj& pattern) {
        BSONObjIterator i(pattern);
        BSONElement e = i.next();
        // _id index must have form exactly {_id : 1} or {_id : -1}.
        // Allows an index of form {_id : "hashed"} to exist but
        // do not consider it to be the primary _id index
        return (0 == strcmp(e.fieldName(), "_id"))
               && (e.numberInt() == 1 || e.numberInt() == -1)
               && i.next().eoo();
    }

    BSONObj KeyPattern::extractShardKeyFromQuery(const BSONObj& query) const {

        if (_pattern.isEmpty())
            return BSONObj();

        if (mongoutils::str::equals(_pattern.firstElement().valuestrsafe(), "hashed")) {
            BSONElement fieldVal = query.getFieldDotted(_pattern.firstElementFieldName());
            return BSON(_pattern.firstElementFieldName() <<
                        BSONElementHasher::hash64(fieldVal , BSONElementHasher::DEFAULT_HASH_SEED));
        }

        return query.extractFields(_pattern);
    }

    bool KeyPattern::isOrderedKeyPattern(const BSONObj& pattern) {
        return IndexNames::BTREE == IndexNames::findPluginName(pattern);
    }

    BSONObj KeyPattern::extractShardKeyFromDoc(const BSONObj& doc) const {
        BSONMatchableDocument matchable(doc);
        return extractShardKeyFromMatchable(matchable);
    }

    BSONObj KeyPattern::extractShardKeyFromMatchable(const MatchableDocument& matchable) const {

        if ( _pattern.isEmpty() )
            return BSONObj();

        BSONObjBuilder keyBuilder;

        BSONObjIterator patternIt(_pattern);
        while (patternIt.more()) {

            BSONElement patternEl = patternIt.next();
            ElementPath path;
            path.init(patternEl.fieldName());

            MatchableDocument::IteratorHolder matchIt(&matchable, &path);
            if (!matchIt->more())
                return BSONObj();
            BSONElement matchEl = matchIt->next().element();
            // We sometimes get eoo(), apparently
            if (matchEl.eoo() || matchIt->more())
                return BSONObj();

            if (mongoutils::str::equals(patternEl.valuestrsafe(), "hashed")) {
                keyBuilder.append(patternEl.fieldName(),
                                  BSONElementHasher::hash64(matchEl,
                                                            BSONElementHasher::DEFAULT_HASH_SEED));
            }
            else {
                // NOTE: The matched element may *not* have the same field name as the path -
                // index keys don't contain field names, for example
                keyBuilder.appendAs(matchEl, patternEl.fieldName());
            }
        }

        return keyBuilder.obj();
    }

    BSONObj KeyPattern::extendRangeBound( const BSONObj& bound , bool makeUpperInclusive ) const {
        BSONObjBuilder newBound( bound.objsize() );

        BSONObjIterator src( bound );
        BSONObjIterator pat( _pattern );

        while( src.more() ){
            massert( 16649 ,
                     str::stream() << "keyPattern " << _pattern << " shorter than bound " << bound,
                     pat.more() );
            BSONElement srcElt = src.next();
            BSONElement patElt = pat.next();
            massert( 16634 ,
                     str::stream() << "field names of bound " << bound
                                   << " do not match those of keyPattern " << _pattern ,
                                   str::equals( srcElt.fieldName() , patElt.fieldName() ) );
            newBound.append( srcElt );
        }
        while( pat.more() ){
            BSONElement patElt = pat.next();
            // for non 1/-1 field values, like {a : "hashed"}, treat order as ascending
            int order = patElt.isNumber() ? patElt.numberInt() : 1;
            // flip the order semantics if this is an upper bound
            if ( makeUpperInclusive ) order *= -1;

            if( order > 0 ){
                newBound.appendMinKey( patElt.fieldName() );
            }
            else {
                newBound.appendMaxKey( patElt.fieldName() );
            }
        }
        return newBound.obj();
    }

    BoundList KeyPattern::flattenBounds( const BSONObj& keyPattern, const IndexBounds& indexBounds ) {
        invariant(indexBounds.fields.size() == (size_t)keyPattern.nFields());

        // If any field is unsatisfied, return empty bound list.
        for (vector<OrderedIntervalList>::const_iterator it = indexBounds.fields.begin();
                it != indexBounds.fields.end(); it++) {
            if (it->intervals.size() == 0) {
                return BoundList();
            }
        }
        // To construct our bounds we will generate intervals based on bounds for
        // the first field, then compound intervals based on constraints for the first
        // 2 fields, then compound intervals for the first 3 fields, etc.
        // As we loop through the fields, we start generating new intervals that will later
        // get extended in another iteration of the loop.  We define these partially constructed
        // intervals using pairs of BSONObjBuilders (shared_ptrs, since after one iteration of the
        // loop they still must exist outside their scope).
        typedef vector< pair< shared_ptr<BSONObjBuilder> ,
                              shared_ptr<BSONObjBuilder> > > BoundBuilders;
        BoundBuilders builders;
        builders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ),
                                       shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
        BSONObjIterator keyIter( keyPattern );
        // until equalityOnly is false, we are just dealing with equality (no range or $in queries).
        bool equalityOnly = true;

        for (size_t i = 0; i < indexBounds.fields.size(); i++) {
            BSONElement e = keyIter.next();

            StringData fieldName = e.fieldNameStringData();

            // get the relevant intervals for this field, but we may have to transform the
            // list of what's relevant according to the expression for this field
            const OrderedIntervalList& oil = indexBounds.fields[i];
            const vector<Interval>& intervals = oil.intervals;

            if ( equalityOnly ) {
                if ( intervals.size() == 1 && intervals.front().isPoint() ){
                    // this field is only a single point-interval
                    BoundBuilders::const_iterator j;
                    for( j = builders.begin(); j != builders.end(); ++j ) {
                        j->first->appendAs( intervals.front().start, fieldName );
                        j->second->appendAs( intervals.front().end, fieldName );
                    }
                }
                else {
                    // This clause is the first to generate more than a single point.
                    // We only execute this clause once. After that, we simplify the bound
                    // extensions to prevent combinatorial explosion.
                    equalityOnly = false;

                    BoundBuilders newBuilders;

                    for(BoundBuilders::const_iterator it = builders.begin(); it != builders.end(); ++it ) {
                        BSONObj first = it->first->obj();
                        BSONObj second = it->second->obj();

                        for ( vector<Interval>::const_iterator interval = intervals.begin();
                                interval != intervals.end(); ++interval )
                        {
                            uassert( 17439,
                                     "combinatorial limit of $in partitioning of results exceeded" ,
                                     newBuilders.size() < MAX_IN_COMBINATIONS );
                            newBuilders.push_back(
                                     make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ),
                                                shared_ptr<BSONObjBuilder>( new BSONObjBuilder())));
                            newBuilders.back().first->appendElements( first );
                            newBuilders.back().second->appendElements( second );
                            newBuilders.back().first->appendAs( interval->start, fieldName );
                            newBuilders.back().second->appendAs( interval->end, fieldName );
                        }
                    }
                    builders = newBuilders;
                }
            }
            else {
                // if we've already generated a range or multiple point-intervals
                // just extend what we've generated with min/max bounds for this field
                BoundBuilders::const_iterator j;
                for( j = builders.begin(); j != builders.end(); ++j ) {
                    j->first->appendAs( intervals.front().start, fieldName );
                    j->second->appendAs( intervals.back().end, fieldName );
                }
            }
        }
        BoundList ret;
        for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i )
            ret.push_back( make_pair( i->first->obj(), i->second->obj() ) );
        return ret;
    }

} // namespace mongo

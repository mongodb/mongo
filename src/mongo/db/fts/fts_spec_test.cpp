// fts_spec_test.cpp

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

#include "mongo/pch.h"

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
    namespace fts {

        TEST( FTSSpec, Fix1 ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            BSONObj fixed = FTSSpec::fixSpec( user );
            BSONObj fixed2 = FTSSpec::fixSpec( fixed );
            ASSERT_EQUALS( fixed, fixed2 );
        }

        TEST( FTSSpec, DefaultLanguage1 ) {
            BSONObj user = BSON( "key" << BSON( "text" << "fts" ) <<
                                 "default_language" << "spanish" );

            try {
                BSONObj fixed = FTSSpec::fixSpec( user );
            }
            catch ( UserException& e ) {
                ASSERT(false);
            }
        }

        TEST( FTSSpec, DefaultLanguage2 ) {
            BSONObj user = BSON( "key" << BSON( "text" << "fts" ) <<
                                 "default_language" << "spanglish" );

            try {
                BSONObj fixed = FTSSpec::fixSpec( user );
                ASSERT(false);
            }
            catch ( UserException& e ) {}
        }

        TEST( FTSSpec, ScoreSingleField1 ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            FTSSpec spec( FTSSpec::fixSpec( user ) );

            TermFrequencyMap m;
            spec.scoreDocument( BSON( "title" << "cat sat run" ),
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &m );
            ASSERT_EQUALS( 3U, m.size() );
            ASSERT_EQUALS( m["cat"], m["sat"] );
            ASSERT_EQUALS( m["cat"], m["run"] );
            ASSERT( m["cat"] > 0 );
        }

        TEST( FTSSpec, ScoreMultipleField1 ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            FTSSpec spec( FTSSpec::fixSpec( user ) );

            TermFrequencyMap m;
            spec.scoreDocument( BSON( "title" << "cat sat run" << "text" << "cat book" ),
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &m );

            ASSERT_EQUALS( 4U, m.size() );
            ASSERT_EQUALS( m["sat"], m["run"] );
            ASSERT( m["sat"] > 0 );

            ASSERT( m["cat"] > m["sat"] );
            ASSERT( m["cat"] > m["book"] );
            ASSERT( m["book"] > 0 );
            ASSERT( m["book"] < m["sat"] );
        }


        TEST( FTSSpec, ScoreRepeatWord ) {
            BSONObj user = BSON( "key" << BSON( "title" << "fts" <<
                                                "text" << "fts" ) <<
                                 "weights" << BSON( "title" << 10 ) );

            FTSSpec spec( FTSSpec::fixSpec( user ) );

            TermFrequencyMap m;
            spec.scoreDocument( BSON( "title" << "cat sat sat run run run" ),
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &m );
            ASSERT_EQUALS( 3U, m.size() );
            ASSERT( m["cat"] > 0 );
            ASSERT( m["sat"] > m["cat"] );
            ASSERT( m["run"] > m["sat"] );

        }

        TEST( FTSSpec, Extra1 ) {
            BSONObj user = BSON( "key" << BSON( "data" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( user ) );
            ASSERT_EQUALS( 0U, spec.numExtraBefore() );
            ASSERT_EQUALS( 0U, spec.numExtraAfter() );
        }

        TEST( FTSSpec, Extra2 ) {
            BSONObj user = BSON( "key" << BSON( "data" << "fts" << "x" << 1 ) );
            BSONObj fixed = FTSSpec::fixSpec( user );
            FTSSpec spec( fixed );
            ASSERT_EQUALS( 0U, spec.numExtraBefore() );
            ASSERT_EQUALS( 1U, spec.numExtraAfter() );
            ASSERT_EQUALS( StringData("x"), spec.extraAfter(0) );

            BSONObj fixed2 = FTSSpec::fixSpec( fixed );
            ASSERT_EQUALS( fixed, fixed2 );
        }

        TEST( FTSSpec, Extra3 ) {
            BSONObj user = BSON( "key" << BSON( "x" << 1 << "data" << "fts" ) );
            BSONObj fixed = FTSSpec::fixSpec( user );

            ASSERT_EQUALS( BSON( "x" << 1 <<
                                 "_fts" << "text" <<
                                 "_ftsx" << 1 ),
                           fixed["key"].Obj() );
            ASSERT_EQUALS( BSON( "data" << 1 ),
                           fixed["weights"].Obj() );

            BSONObj fixed2 = FTSSpec::fixSpec( fixed );
            ASSERT_EQUALS( fixed, fixed2 );

            FTSSpec spec( fixed );
            ASSERT_EQUALS( 1U, spec.numExtraBefore() );
            ASSERT_EQUALS( StringData("x"), spec.extraBefore(0) );
            ASSERT_EQUALS( 0U, spec.numExtraAfter() );

            BSONObj prefix;

            ASSERT( spec.getIndexPrefix( BSON( "x" << 2 ), &prefix ).isOK() );
            ASSERT_EQUALS( BSON( "x" << 2 ), prefix );

            ASSERT( spec.getIndexPrefix( BSON( "x" << 3 << "y" << 4 ), &prefix ).isOK() );
            ASSERT_EQUALS( BSON( "x" << 3 ), prefix );

            ASSERT( !spec.getIndexPrefix( BSON( "x" << BSON( "$gt" << 5 ) ), &prefix ).isOK() );
            ASSERT( !spec.getIndexPrefix( BSON( "y" << 4 ), &prefix ).isOK() );
            ASSERT( !spec.getIndexPrefix( BSONObj(), &prefix ).isOK() );
        }

        // Test for correct behavior when encountering nested arrays (both directly nested and
        // indirectly nested).

        TEST( FTSSpec, NestedArraysPos1 ) {
            BSONObj user = BSON( "key" << BSON( "a.b" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( user ) );

            // The following document matches {"a.b": {$type: 2}}, so "term" should be indexed.
            BSONObj obj = fromjson("{a: [{b: ['term']}]}"); // indirectly nested arrays
            TermFrequencyMap m;
            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &m );
            ASSERT_EQUALS( 1U, m.size() );
        }

        TEST( FTSSpec, NestedArraysPos2 ) {
            BSONObj user = BSON( "key" << BSON( "$**" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( user ) );

            // The wildcard spec implies a full recursive traversal, so "term" should be indexed.
            BSONObj obj = fromjson("{a: {b: [['term']]}}"); // directly nested arrays
            TermFrequencyMap m;
            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &m );
            ASSERT_EQUALS( 1U, m.size() );
        }

        TEST( FTSSpec, NestedArraysNeg1 ) {
            BSONObj user = BSON( "key" << BSON( "a.b" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( user ) );

            // The following document does not match {"a.b": {$type: 2}}, so "term" should not be
            // indexed.
            BSONObj obj = fromjson("{a: {b: [['term']]}}"); // directly nested arrays
            TermFrequencyMap m;
            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &m );
            ASSERT_EQUALS( 0U, m.size() );
        }

        // Multi-language test_1: test independent stemming per sub-document
        TEST( FTSSpec, NestedLanguages_PerArrayItemStemming ) {
            BSONObj indexSpec = BSON( "key" << BSON( "a.b.c" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
            TermFrequencyMap tfm;

            BSONObj obj = fromjson(
                "{ a :"
                "  { b :"
                "    [ { c : \"walked\", language : \"english\" },"
                "      { c : \"camminato\", language : \"italian\" },"
                "      { c : \"ging\", language : \"german\" } ]"
                "   }"
                " }" );

            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &tfm );

            set<string> hits;
            hits.insert("walk");
            hits.insert("cammin");
            hits.insert("ging");

            for (TermFrequencyMap::const_iterator i = tfm.begin(); i!=tfm.end(); ++i) {
                string term = i->first;
                ASSERT_EQUALS( 1U, hits.count( term ) );
            }

        }

        // Multi-language test_2: test nested stemming per sub-document
        TEST( FTSSpec, NestedLanguages_PerSubdocStemming ) {
            BSONObj indexSpec = BSON( "key" << BSON( "a.b.c" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
            TermFrequencyMap tfm;

            BSONObj obj = fromjson(
                "{ language : \"english\","
                "  a :"
                "  { language : \"danish\","
                "    b :"
                "    [ { c : \"foredrag\" },"
                "      { c : \"foredragsholder\" },"
                "      { c : \"lector\" } ]"
                "  }"
                "}" );

            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &tfm );

            set<string> hits;
            hits.insert("foredrag");
            hits.insert("foredragshold");
            hits.insert("lector");

            for (TermFrequencyMap::const_iterator i = tfm.begin(); i!=tfm.end(); ++i) {
                string term = i->first;
                ASSERT_EQUALS( 1U, hits.count( term ) );
            }

        }

        // Multi-language test_3: test nested arrays
        TEST( FTSSpec, NestedLanguages_NestedArrays ) {
            BSONObj indexSpec = BSON( "key" << BSON( "a.b.c" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
            TermFrequencyMap tfm;

            BSONObj obj = fromjson(
                "{ language : \"english\","
                "  a : ["
                "  { language : \"danish\","
                "    b :"
                "    [ { c : [\"foredrag\"] },"
                "      { c : [\"foredragsholder\"] },"
                "      { c : [\"lector\"] } ]"
                "  } ]"
                "}" );

            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &tfm );

            set<string> hits;
            hits.insert("foredrag");
            hits.insert("foredragshold");
            hits.insert("lector");

            for (TermFrequencyMap::const_iterator i = tfm.begin(); i!=tfm.end(); ++i) {
                string term = i->first;
                ASSERT_EQUALS( 1U, hits.count( term ) );
            }

        }

        // Multi-language test_4: test pruning
        TEST( FTSSpec, NestedLanguages_PathPruning ) {
            BSONObj indexSpec = BSON( "key" << BSON( "a.b.c" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
            TermFrequencyMap tfm;

            BSONObj obj = fromjson(
                "{ language : \"english\","
                "  a : "
                "  { language : \"danish\","
                "    bc : \"foo\","
                "    b : { d: \"bar\" },"
                "    b :"
                "    [ { c : \"foredrag\" },"
                "      { c : \"foredragsholder\" },"
                "      { c : \"lector\" } ]"
                "  }"
                "}" );

            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &tfm );

            set<string> hits;
            hits.insert("foredrag");
            hits.insert("foredragshold");
            hits.insert("lector");

            for (TermFrequencyMap::const_iterator i = tfm.begin(); i!=tfm.end(); ++i) {
                string term = i->first;
                ASSERT_EQUALS( 1U, hits.count( term ) );
            }

        }

        // Multi-language test_5: test wildcard spec
        TEST( FTSSpec, NestedLanguages_Wildcard ) {
            BSONObj indexSpec = BSON( "key" << BSON( "$**" << "fts" ) );
            FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
            TermFrequencyMap tfm;

            BSONObj obj = fromjson(
                "{ language : \"english\","
                "  b : \"walking\","
                "  c : { e: \"walked\" },"
                "  d : "
                "  { language : \"danish\","
                "    e :"
                "    [ { f : \"foredrag\" },"
                "      { f : \"foredragsholder\" },"
                "      { f : \"lector\" } ]"
                "  }"
                "}" );

            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &tfm );

            set<string> hits;
            hits.insert("foredrag");
            hits.insert("foredragshold");
            hits.insert("lector");
            hits.insert("walk");

            for (TermFrequencyMap::const_iterator i = tfm.begin(); i!=tfm.end(); ++i) {
                string term = i->first;
                ASSERT_EQUALS( 1U, hits.count( term ) );
            }

        }

        // Multi-language test_6: test wildcard spec with override
        TEST( FTSSpec, NestedLanguages_WildcardOverride ) {
            BSONObj indexSpec = BSON( "key" << BSON( "$**" << "fts" ) <<
                                      "weights" << BSON( "d.e.f" << 20 ) );
            FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
            TermFrequencyMap tfm;

            BSONObj obj = fromjson(
                "{ language : \"english\","
                "  b : \"walking\","
                "  c : { e: \"walked\" },"
                "  d : "
                "  { language : \"danish\","
                "    e :"
                "    [ { f : \"foredrag\" },"
                "      { f : \"foredragsholder\" },"
                "      { f : \"lector\" } ]"
                "  }"
                "}" );

            spec.scoreDocument( obj,
                                FTSLanguage::makeFTSLanguage( "english" ).getValue(),
                                "",
                                false,
                                &tfm );

            set<string> hits;
            hits.insert("foredrag");
            hits.insert("foredragshold");
            hits.insert("lector");
            hits.insert("walk");

            for (TermFrequencyMap::const_iterator i = tfm.begin(); i!=tfm.end(); ++i) {
                string term = i->first;
                ASSERT_EQUALS( 1U, hits.count( term ) );
            }

        }


    }
}

// histogramtests.cpp : histogram.{h,cpp} unit tests

/**
 *    Copyright (C) 2010 10gen Inc.
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

#include "../pch.h"

#include "dbtests.h"
#include "../util/histogram.h"

namespace mongo {

    using mongo::Histogram;

    class BoundariesInit {
    public:
        void run() {
            Histogram::Options opts;
            opts.numBuckets = 3;
            opts.bucketSize = 10;
            Histogram h( opts );

            ASSERT_EQUALS( h.getBucketsNum(), 3u );

            ASSERT_EQUALS( h.getCount( 0 ), 0u );
            ASSERT_EQUALS( h.getCount( 1 ), 0u );
            ASSERT_EQUALS( h.getCount( 2 ), 0u );

            ASSERT_EQUALS( h.getBoundary( 0 ), 10u );
            ASSERT_EQUALS( h.getBoundary( 1 ), 20u );
            ASSERT_EQUALS( h.getBoundary( 2 ), numeric_limits<uint32_t>::max() );
        }
    };

    class BoundariesExponential {
    public:
        void run() {
            Histogram::Options opts;
            opts.numBuckets = 4;
            opts.bucketSize = 125;
            opts.exponential = true;
            Histogram h( opts );

            ASSERT_EQUALS( h.getBoundary( 0 ), 125u );
            ASSERT_EQUALS( h.getBoundary( 1 ), 250u );
            ASSERT_EQUALS( h.getBoundary( 2 ), 500u );
            ASSERT_EQUALS( h.getBoundary( 3 ), numeric_limits<uint32_t>::max() );
        }
    };

    class BoundariesFind {
    public:
        void run() {
            Histogram::Options opts;
            opts.numBuckets = 3;
            opts.bucketSize = 10;
            Histogram h( opts );

            h.insert( 10 );  // end of first bucket
            h.insert( 15 );  // second bucket
            h.insert( 18 );  // second bucket

            ASSERT_EQUALS( h.getCount( 0 ), 1u );
            ASSERT_EQUALS( h.getCount( 1 ), 2u );
            ASSERT_EQUALS( h.getCount( 2 ), 0u );
        }
    };

    class HistogramSuite : public Suite {
    public:
        HistogramSuite() : Suite( "histogram" ) {}

        void setupTests() {
            add< BoundariesInit >();
            add< BoundariesExponential >();
            add< BoundariesFind >();
            // TODO: complete the test suite
        }
    } histogramSuite;

}  // anonymous namespace

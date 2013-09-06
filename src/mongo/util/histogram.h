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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#ifndef UTIL_HISTOGRAM_HEADER
#define UTIL_HISTOGRAM_HEADER

#include <string>
#include <stdint.h>

namespace mongo {

    /**
     * A histogram for a 32-bit integer range.
     */
    class Histogram {
    public:
        /**
         * Construct a histogram with 'numBuckets' buckets, optionally
         * having the first bucket start at 'initialValue' rather than
         * 0. By default, the histogram buckets will be 'bucketSize' wide.
         *
         * Usage example:
         *   Histogram::Options opts;
         *   opts.numBuckets = 3;
         *   opts.bucketSize = 10;
         *   Histogram h( opts );
         *
         *   Generates the bucket ranges [0..10],[11..20],[21..max_int]
         *
         * Alternatively, the flag 'exponential' could be turned on, in
         * which case a bucket's maximum value will be
         *    initialValue + bucketSize * 2 ^ [0..numBuckets-1]
         *
         * Usage example:
         *   Histogram::Options opts;
         *   opts.numBuckets = 4;
         *   opts.bucketSize = 125;
         *   opts.exponential = true;
         *   Histogram h( opts );
         *
         *   Generates the bucket ranges [0..125],[126..250],[251..500],[501..max_int]
         */
        struct Options {
            uint32_t numBuckets;
            uint32_t bucketSize;
            uint32_t initialValue;

            // use exponential buckets?
            bool            exponential;

            Options()
                : numBuckets(0)
                , bucketSize(0)
                , initialValue(0)
                , exponential(false) {}
        };
        explicit Histogram( const Options& opts );
        ~Histogram();

        /**
         * Find the bucket that 'element' falls into and increment its count.
         */
        void insert( uint32_t element );

        /**
         * Render the histogram as string that can be used inside an
         * HTML doc.
         */
        std::string toHTML() const;

        // testing interface below -- consider it private

        /**
         * Return the count for the 'bucket'-th bucket.
         */
        uint64_t getCount( uint32_t bucket ) const;

        /**
         * Return the maximum element that would fall in the
         * 'bucket'-th bucket.
         */
        uint32_t getBoundary( uint32_t bucket ) const;

        /**
         * Return the number of buckets in this histogram.
         */
        uint32_t getBucketsNum() const;

    private:
        /**
         * Returns the bucket where 'element' should fall
         * into. Currently assumes that 'element' is greater than the
         * minimum 'inialValue'.
         */
        uint32_t _findBucket( uint32_t element ) const;

        uint32_t  _initialValue;  // no value lower than it is recorded
        uint32_t  _numBuckets;    // total buckets in the histogram

        // all below owned here
        uint32_t* _boundaries;    // maximum element of each bucket
        uint64_t* _buckets;       // current count of each bucket

        Histogram( const Histogram& );
        Histogram& operator=( const Histogram& );
    };

}  // namespace mongo

#endif  //  UTIL_HISTOGRAM_HEADER

/**
 *    Copyright (C) 2015 MongoDB Inc.
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

namespace mongo {

class BSONObj;
class LiteParsedQuery;

/**
 * Suite of find/getMore related functions used in both the mongod and mongos query paths.
 */
class FindCommon {
public:
    // The size threshold at which we stop adding result documents to a getMore response or a find
    // response that has a batchSize set (i.e. once the sum of the document sizes in bytes exceeds
    // this value, no further documents are added).
    static const int kMaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    /**
     * Returns true if enough results have been prepared to stop adding more to the first batch.
     *
     * If 'pq' does not have a batchSize, the default batchSize is respected.
     */
    static bool enoughForFirstBatch(const LiteParsedQuery& pq,
                                    long long numDocs,
                                    int bytesBuffered);

    /**
     * Returns true if enough results have been prepared to stop adding more to a getMore batch.
     *
     * An 'effectiveBatchSize' value of zero is interpreted as the absence of a batchSize;
     * in this case, returns true only once the size threshold is exceeded. If 'effectiveBatchSize'
     * is positive, returns true once either are added until we have either satisfied the batch size
     * or exceeded the size threshold.
     */
    static bool enoughForGetMore(long long effectiveBatchSize,
                                 long long numDocs,
                                 int bytesBuffered);

    /**
     * Transforms the raw sort spec into one suitable for use as the ordering specification in
     * BSONObj::woCompare().
     *
     * In particular, eliminates text score meta-sort from 'sortSpec'.
     *
     * The input must be validated (each BSON element must be either a number or text score
     * meta-sort specification).
     */
    static BSONObj transformSortSpec(const BSONObj& sortSpec);
};

}  // namespace mongo

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

#include "mongo/platform/basic.h"

#include "mongo/db/query/find_common.h"

#include "mongo/db/query/lite_parsed_query.h"

namespace mongo {

bool FindCommon::enoughForFirstBatch(const LiteParsedQuery& pq,
                                     long long numDocs,
                                     int bytesBuffered) {
    if (!pq.getBatchSize()) {
        // If there is no batch size, we stop generating additional results as soon as we have
        // either 101 documents or at least 1MB of data.
        return (bytesBuffered > 1024 * 1024) || numDocs >= LiteParsedQuery::kDefaultBatchSize;
    }

    // If there is a batch size, we add results until either satisfying this batch size or exceeding
    // the 4MB size threshold.
    return numDocs >= *pq.getBatchSize() || bytesBuffered > kMaxBytesToReturnToClientAtOnce;
}

bool FindCommon::enoughForGetMore(long long ntoreturn, long long numDocs, int bytesBuffered) {
    return (ntoreturn && numDocs >= ntoreturn) || (bytesBuffered > kMaxBytesToReturnToClientAtOnce);
}

}  // namespace mongo

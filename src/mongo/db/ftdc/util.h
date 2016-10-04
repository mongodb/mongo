/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/ftdc/decompressor.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Utilities for inflating and deflating BSON documents and metric arrays
 */
namespace FTDCBSONUtil {

/**
* Type of FTDC document.
*
* NOTE: Persisted to disk via BSON Objects.
*/
enum class FTDCType : std::int32_t {
    /**
    * A metadata document is composed of a header + an array of bson documents
    *
    * See createBSONMetadataChunkDocument
    */
    kMetadata = 0,

    /**
    * A metrics chunk is composed of a header + a compressed metric chunk.
    *
    * See createBSONMetricChunkDocument
    */
    kMetricChunk = 1,
};


/**
 * Extract an array of numbers from a pair of documents. Verifies the pair of documents have same
 * structure.
 *
 * Types considered numbers for the purpose of metrics:
 *  - double - encoded as an integer, loses fractional components via truncation
 *  - 32-bit integer
 *  - 64-integer
 *  - bool
 *  - date
 *  - timestamp
 *    Note: Timestamp is encoded as two integers, the timestamp value followed by the increment.
 *
 * Two documents are considered the same if satisfy the following criteria:
 *
 * Criteria: During a Depth First traversal of the document:
 *  1. Each element has the same name regardless of its type.
 *  2. The same number of elements exist in each document.
 *  3. The types of each element are the same.
 *     Note: Double, Int, and Long are treated as equivalent for this purpose.
 *
 * @param referenceDoc A reference document to use the as the definition of the correct schema.
 * @param doc A second document to compare against the reference document and extract metrics
 * from
 * @param metrics A vector of metrics that were extracted from the doc
 *
 * \return false if the documents differ in terms of metrics
 */
StatusWith<bool> extractMetricsFromDocument(const BSONObj& referenceDoc,
                                            const BSONObj& doc,
                                            std::vector<std::uint64_t>* metrics);

/**
 * Construct a document from a reference document and array of metrics.
 *
 * @param referenceDoc A reference document to use the as the definition of the correct schema.
 * @param builder A BSON builder to construct a single document into. Each document will be a
 *copy
 * of the reference document with the numerical fields replaced with values from metrics array.
 * @param metrics A vector of metrics for all documents
 * @param pos A position into the array of metrics to start iterating from.
 *
 * \return Status if the decompression of the buffer was successful or failed. Decompression may
 * fail if the buffer is not valid.
 */
Status constructDocumentFromMetrics(const BSONObj& referenceDoc,
                                    BSONObjBuilder& builder,
                                    const std::vector<std::uint64_t>& metrics,
                                    size_t* pos);

/**
 * Construct a document from a reference document and array of metrics. See documentation above.
 */
StatusWith<BSONObj> constructDocumentFromMetrics(const BSONObj& ref,
                                                 const std::vector<std::uint64_t>& metrics);

/**
 * Create BSON metadata document for storage. The passed in document is embedded as the doc
 * field in the example above. For the _id field, the specified date is used.
 *
 * Example:
 * {
 *  "_id" : Date_t
 *  "type" : 0
 *  "doc" : { ... }
 * }
 */
BSONObj createBSONMetadataDocument(const BSONObj& metadata, Date_t date);

/**
 * Create a BSON metric chunk document for storage. The passed in document is embedded as the
 * data field in the example above. For the _id field, the date is specified by the caller
 * since the metric chunk usually composed of multiple samples gathered over a period of time.
 *
 * Example:
 * {
 *  "_id" : Date_t
 *  "type" : 1
 *  "data" : BinData(...)
 * }
 */
BSONObj createBSONMetricChunkDocument(ConstDataRange buf, Date_t now);

/**
 * Get the _id field of a BSON document
 */
StatusWith<Date_t> getBSONDocumentId(const BSONObj& obj);

/**
 * Get the type of a BSON document
 */
StatusWith<FTDCType> getBSONDocumentType(const BSONObj& obj);

/**
 * Extract the metadata field from a BSON document
 */
StatusWith<BSONObj> getBSONDocumentFromMetadataDoc(const BSONObj& obj);

/**
 * Get the set of metric documents from the compressed chunk of a metric document
 */
StatusWith<std::vector<BSONObj>> getMetricsFromMetricDoc(const BSONObj& obj,
                                                         FTDCDecompressor* decompressor);
}  // namespace FTDCBSONUtil


/**
 * Miscellaneous utilties for FTDC.
 */
namespace FTDCUtil {
/**
 * Construct the full path to the interim file
 */
boost::filesystem::path getInterimFile(const boost::filesystem::path& file);

/**
 * Construct the full path to the interim temp file before it is renamed.
 */
boost::filesystem::path getInterimTempFile(const boost::filesystem::path& file);

/**
 * Round the specified time_point to the next multiple of period after the specified time_point
 */
Date_t roundTime(Date_t now, Milliseconds period);

}  // namespace FTDCUtil

}  // namespace mongo

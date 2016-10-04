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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/util.h"

#include <boost/filesystem.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/config.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

const char kFTDCInterimFile[] = "metrics.interim";
const char kFTDCInterimTempFile[] = "metrics.interim.temp";
const char kFTDCArchiveFile[] = "metrics";

const char kFTDCIdField[] = "_id";
const char kFTDCTypeField[] = "type";

const char kFTDCDataField[] = "data";
const char kFTDCDocField[] = "doc";

const char kFTDCDocsField[] = "docs";

const char kFTDCCollectStartField[] = "start";
const char kFTDCCollectEndField[] = "end";

const std::int64_t FTDCConfig::kPeriodMillisDefault = 1000;

const std::size_t kMaxRecursion = 10;

namespace FTDCUtil {

namespace {
boost::filesystem::path appendFileName(const boost::filesystem::path& file, const char* filename) {
    if (boost::filesystem::is_directory(file)) {
        return file / filename;
    }

    auto p = file.parent_path();
    p /= filename;

    return p;
}
}  // namespace

boost::filesystem::path getInterimFile(const boost::filesystem::path& file) {
    return appendFileName(file, kFTDCInterimFile);
}

boost::filesystem::path getInterimTempFile(const boost::filesystem::path& file) {
    return appendFileName(file, kFTDCInterimTempFile);
}

Date_t roundTime(Date_t now, Milliseconds period) {
    // Note: auto type deduction is explicitly avoided here to ensure rigid type correctness
    long long clock_duration = now.toMillisSinceEpoch();

    long long now_next_period = clock_duration + period.count();

    long long excess_time(now_next_period % period.count());

    long long next_time = now_next_period - excess_time;

    return Date_t::fromMillisSinceEpoch(next_time);
}

}  // namespace FTDCUtil


namespace FTDCBSONUtil {

namespace {

StatusWith<bool> extractMetricsFromDocument(const BSONObj& referenceDoc,
                                            const BSONObj& currentDoc,
                                            std::vector<std::uint64_t>* metrics,
                                            bool matches,
                                            size_t recursion) {
    if (recursion > kMaxRecursion) {
        return {ErrorCodes::BadValue, "Recursion limit reached."};
    }

    BSONObjIterator itCurrent(currentDoc);
    BSONObjIterator itReference(referenceDoc);

    while (itCurrent.more()) {
        // Schema mismatch if current document is longer than reference document
        if (matches && !itReference.more()) {
            LOG(4) << "full-time diagnostic data capture schema change: currrent document is "
                      "longer than "
                      "reference document";
            matches = false;
        }

        BSONElement currentElement = itCurrent.next();
        BSONElement referenceElement = matches ? itReference.next() : BSONElement();

        if (matches) {
            // Check for matching field names
            if (referenceElement.fieldNameStringData() != currentElement.fieldNameStringData()) {
                LOG(4)
                    << "full-time diagnostic data capture schema change: field name change - from '"
                    << referenceElement.fieldNameStringData() << "' to '"
                    << currentElement.fieldNameStringData() << "'";
                matches = false;
            }

            // Check that types match, allowing any numeric type to match any other numeric type.
            // This looseness is necessary because some metrics use varying numeric types,
            // and if that was considered a schema mismatch, it would increase the number of
            // reference samples required.
            if ((currentElement.type() != referenceElement.type()) &&
                !(referenceElement.isNumber() == true &&
                  currentElement.isNumber() == referenceElement.isNumber())) {
                LOG(4) << "full-time diagnostic data capture  schema change: field type change for "
                          "field '"
                       << referenceElement.fieldNameStringData() << "' from '"
                       << static_cast<int>(referenceElement.type()) << "' to '"
                       << static_cast<int>(currentElement.type()) << "'";
                matches = false;
            }
        }

        switch (currentElement.type()) {
            // all numeric types are extracted as long (int64)
            // this supports the loose schema matching mentioned above,
            // but does create a range issue for doubles, and requires doubles to be integer
            case NumberDouble:
            case NumberInt:
            case NumberLong:
            case NumberDecimal:
                metrics->emplace_back(currentElement.numberLong());
                break;

            case Bool:
                metrics->emplace_back(currentElement.Bool());
                break;

            case Date:
                metrics->emplace_back(currentElement.Date().toMillisSinceEpoch());
                break;

            case bsonTimestamp:
                // very slightly more space efficient to treat these as two separate metrics
                metrics->emplace_back(currentElement.timestamp().getSecs());
                metrics->emplace_back(currentElement.timestamp().getInc());
                break;

            case Object:
            case Array: {
                // Maximum recursion is controlled by the documents we collect. Maximum is 5 in the
                // current implementation.
                auto sw = extractMetricsFromDocument(matches ? referenceElement.Obj() : BSONObj(),
                                                     currentElement.Obj(),
                                                     metrics,
                                                     matches,
                                                     recursion + 1);
                if (!sw.isOK()) {
                    return sw;
                }
                matches = matches && sw.getValue();
            } break;

            default:
                break;
        }
    }

    // schema mismatch if ref is longer than curr
    if (matches && itReference.more()) {
        LOG(4) << "full-time diagnostic data capture schema change: reference document is longer "
                  "then current";
        matches = false;
    }

    return {matches};
}

}  // namespace

StatusWith<bool> extractMetricsFromDocument(const BSONObj& referenceDoc,
                                            const BSONObj& currentDoc,
                                            std::vector<std::uint64_t>* metrics) {
    return extractMetricsFromDocument(referenceDoc, currentDoc, metrics, true, 0);
}

namespace {
Status constructDocumentFromMetrics(const BSONObj& referenceDocument,
                                    BSONObjBuilder& builder,
                                    const std::vector<std::uint64_t>& metrics,
                                    size_t* pos,
                                    size_t recursion) {
    if (recursion > kMaxRecursion) {
        return {ErrorCodes::BadValue, "Recursion limit reached."};
    }

    BSONObjIterator iterator(referenceDocument);
    while (iterator.more()) {
        BSONElement currentElement = iterator.next();

        switch (currentElement.type()) {
            case NumberDouble:
            case NumberInt:
            case NumberLong:
            case NumberDecimal:
                if (*pos >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                builder.append(currentElement.fieldName(),
                               static_cast<long long int>(metrics[(*pos)++]));
                break;

            case Bool:
                if (*pos >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                builder.append(currentElement.fieldName(), static_cast<bool>(metrics[(*pos)++]));
                break;

            case Date:
                if (*pos >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                builder.append(
                    currentElement.fieldName(),
                    Date_t::fromMillisSinceEpoch(static_cast<std::uint64_t>(metrics[(*pos)++])));
                break;

            case bsonTimestamp: {
                if (*pos + 1 >= metrics.size()) {
                    return Status(
                        ErrorCodes::BadValue,
                        "There are more metrics in the reference document then expected.");
                }

                std::uint64_t seconds = metrics[(*pos)++];
                std::uint64_t increment = metrics[(*pos)++];
                builder.append(currentElement.fieldName(), Timestamp(seconds, increment));
                break;
            }

            case Object: {
                BSONObjBuilder sub(builder.subobjStart(currentElement.fieldName()));
                auto s = constructDocumentFromMetrics(
                    currentElement.Obj(), sub, metrics, pos, recursion + 1);
                if (!s.isOK()) {
                    return s;
                }
                break;
            }

            case Array: {
                BSONObjBuilder sub(builder.subarrayStart(currentElement.fieldName()));
                auto s = constructDocumentFromMetrics(
                    currentElement.Obj(), sub, metrics, pos, recursion + 1);
                if (!s.isOK()) {
                    return s;
                }
                break;
            }

            default:
                builder.append(currentElement);
                break;
        }
    }

    return Status::OK();
}

}  // namespace

StatusWith<BSONObj> constructDocumentFromMetrics(const BSONObj& ref,
                                                 const std::vector<std::uint64_t>& metrics) {
    size_t at = 0;
    BSONObjBuilder b;
    Status s = constructDocumentFromMetrics(ref, b, metrics, &at, 0);
    if (!s.isOK()) {
        return StatusWith<BSONObj>(s);
    }

    return b.obj();
}

BSONObj createBSONMetadataDocument(const BSONObj& metadata, Date_t date) {
    BSONObjBuilder builder;
    builder.appendDate(kFTDCIdField, date);
    builder.appendNumber(kFTDCTypeField, static_cast<int>(FTDCType::kMetadata));
    builder.appendObject(kFTDCDocField, metadata.objdata(), metadata.objsize());

    return builder.obj();
}

BSONObj createBSONMetricChunkDocument(ConstDataRange buf, Date_t date) {
    BSONObjBuilder builder;

    builder.appendDate(kFTDCIdField, date);
    builder.appendNumber(kFTDCTypeField, static_cast<int>(FTDCType::kMetricChunk));
    builder.appendBinData(kFTDCDataField, buf.length(), BinDataType::BinDataGeneral, buf.data());

    return builder.obj();
}

StatusWith<Date_t> getBSONDocumentId(const BSONObj& obj) {
    BSONElement element;

    Status status = bsonExtractTypedField(obj, kFTDCIdField, BSONType::Date, &element);
    if (!status.isOK()) {
        return {status};
    }

    return {element.Date()};
}

StatusWith<FTDCType> getBSONDocumentType(const BSONObj& obj) {
    long long value;

    Status status = bsonExtractIntegerField(obj, kFTDCTypeField, &value);
    if (!status.isOK()) {
        return {status};
    }

    if (static_cast<FTDCType>(value) != FTDCType::kMetricChunk &&
        static_cast<FTDCType>(value) != FTDCType::kMetadata) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field '" << std::string(kFTDCTypeField)
                              << "' is not an expected value, found '"
                              << value
                              << "'"};
    }

    return {static_cast<FTDCType>(value)};
}

StatusWith<BSONObj> getBSONDocumentFromMetadataDoc(const BSONObj& obj) {
    if (kDebugBuild) {
        auto swType = getBSONDocumentType(obj);
        dassert(swType.isOK() && swType.getValue() == FTDCType::kMetadata);
    }

    BSONElement element;

    Status status = bsonExtractTypedField(obj, kFTDCDocField, BSONType::Object, &element);
    if (!status.isOK()) {
        return {status};
    }

    return {element.Obj()};
}

StatusWith<std::vector<BSONObj>> getMetricsFromMetricDoc(const BSONObj& obj,
                                                         FTDCDecompressor* decompressor) {
    if (kDebugBuild) {
        auto swType = getBSONDocumentType(obj);
        dassert(swType.isOK() && swType.getValue() == FTDCType::kMetricChunk);
    }

    BSONElement element;

    Status status = bsonExtractTypedField(obj, kFTDCDataField, BSONType::BinData, &element);
    if (!status.isOK()) {
        return {status};
    }

    int length;
    const char* buffer = element.binData(length);
    if (length < 0) {
        return {ErrorCodes::BadValue,
                str::stream() << "Field " << std::string(kFTDCTypeField) << " is not a BinData."};
    }

    return decompressor->uncompress({buffer, static_cast<std::size_t>(length)});
}

}  // namespace FTDCBSONUtil

}  // namespace mongo

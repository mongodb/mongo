/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalSplitPipeline,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalSplitPipeline::createFromBson);

constexpr StringData DocumentSourceInternalSplitPipeline::kStageName;

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalSplitPipeline::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalSplitPipeline must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::Object);

    auto specObj = elem.embeddedObject();

    HostTypeRequirement mergeType = HostTypeRequirement::kNone;

    for (auto&& elt : specObj) {
        if (elt.fieldNameStringData() == "mergeType"_sd) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "'mergeType' must be a string value but found: " << elt.type(),
                    elt.type() == BSONType::String);

            auto mergeTypeString = elt.valueStringData();

            if ("localOnly"_sd == mergeTypeString) {
                mergeType = HostTypeRequirement::kLocalOnly;
            } else if ("anyShard"_sd == mergeTypeString) {
                mergeType = HostTypeRequirement::kAnyShard;
            } else if ("primaryShard"_sd == mergeTypeString) {
                mergeType = HostTypeRequirement::kPrimaryShard;
            } else if ("mongos"_sd == mergeTypeString) {
                mergeType = HostTypeRequirement::kMongoS;
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << "unrecognized field while parsing mergeType: '"
                                        << elt.fieldNameStringData()
                                        << "'");
            }
        } else {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "unrecognized field while parsing $_internalSplitPipeline: '"
                                    << elt.fieldNameStringData()
                                    << "'");
        }
    }

    return new DocumentSourceInternalSplitPipeline(expCtx, mergeType);
}

DocumentSource::GetNextResult DocumentSourceInternalSplitPipeline::getNext() {
    pExpCtx->checkForInterrupt();
    return pSource->getNext();
}

Value DocumentSourceInternalSplitPipeline::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    std::string mergeTypeString;

    switch (_mergeType) {
        case HostTypeRequirement::kAnyShard:
            mergeTypeString = "anyShard";
            break;

        case HostTypeRequirement::kPrimaryShard:
            mergeTypeString = "primaryShard";
            break;

        case HostTypeRequirement::kLocalOnly:
            mergeTypeString = "localOnly";
            break;

        case HostTypeRequirement::kMongoS:
            mergeTypeString = "mongos";
            break;

        case HostTypeRequirement::kNone:
        default:
            break;
    }

    return Value(
        Document{{getSourceName(),
                  Value{Document{{"mergeType",
                                  mergeTypeString.empty() ? Value() : Value(mergeTypeString)}}}}});
}

}  // namesace mongo

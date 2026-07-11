// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_count.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;
using std::string;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(count, CountLiteParsed::parse, AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_CONTAINER_WITH_STAGE_PARAMS_DEFAULT(count,
                                                             DocumentSourceCount,
                                                             CountStageParams);

list<intrusive_ptr<DocumentSource>> DocumentSourceCount::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40156,
            str::stream() << "the count field must be a non-empty string",
            elem.type() == BSONType::string);

    std::string_view elemString = elem.valueStringData();
    uassert(
        40157, str::stream() << "the count field must be a non-empty string", !elemString.empty());

    uassert(40158,
            str::stream() << "the count field cannot be a $-prefixed path",
            elemString[0] != '$');

    uassert(40159,
            str::stream() << "the count field cannot contain a null byte",
            elemString.find('\0') == string::npos);

    uassert(40160,
            str::stream() << "the count field cannot contain '.'",
            elemString.find('.') == string::npos);

    uassert(9039800, str::stream() << "the count field cannot be '_id'", elemString != "_id");

    BSONObj groupObj = BSON("$group" << BSON("_id" << BSONNULL << elemString << BSON("$sum" << 1)));
    BSONObj projectObj = BSON("$project" << BSON("_id" << 0 << elemString << 1));

    auto groupSource = DocumentSourceGroup::createFromBson(groupObj.firstElement(), pExpCtx);
    auto projectSource = DocumentSourceProject::createFromBson(projectObj.firstElement(), pExpCtx);

    return {groupSource, projectSource};
}
}  // namespace mongo

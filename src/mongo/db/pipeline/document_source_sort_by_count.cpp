// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_sort_by_count.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(sortByCount,
                                     SortByCountLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_CONTAINER_WITH_STAGE_PARAMS_DEFAULT(sortByCount,
                                                             DocumentSourceSortByCount,
                                                             SortByCountStageParams);

list<intrusive_ptr<DocumentSource>> DocumentSourceSortByCount::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    if (elem.type() == BSONType::object) {
        // Make sure that the sortByCount field is an expression inside an object
        BSONObj innerObj = elem.embeddedObject();
        uassert(40147,
                str::stream() << "the sortByCount field must be defined as a $-prefixed path or an "
                                 "expression inside an object",
                innerObj.firstElementFieldName()[0] == '$');
    } else if (elem.type() == BSONType::string) {
        // Make sure that the sortByCount field is a $-prefixed path.
        uassert(40148,
                str::stream() << "the sortByCount field must be defined as a $-prefixed path or an "
                                 "expression inside an object",
                // The string length must be greater than 2 because we need a '$', at least one char
                // for the field name and the final terminating 0.
                (elem.valuestrsize() > 2 && elem.valueStringData()[0] == '$'));
    } else {
        uasserted(
            40149,
            str::stream() << "the sortByCount field must be specified as a string or as an object");
    }

    BSONObjBuilder groupExprBuilder;
    groupExprBuilder.appendAs(elem, "_id");
    groupExprBuilder.append("count", BSON("$sum" << 1));

    BSONObj groupObj = BSON("$group" << groupExprBuilder.obj());
    BSONObj sortObj = BSON("$sort" << BSON("count" << -1));

    auto groupSource = DocumentSourceGroup::createFromBson(groupObj.firstElement(), pExpCtx);
    auto sortSource = DocumentSourceSort::createFromBson(sortObj.firstElement(), pExpCtx);

    return {groupSource, sortSource};
}
}  // namespace mongo

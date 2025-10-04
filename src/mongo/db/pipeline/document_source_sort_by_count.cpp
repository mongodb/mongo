/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/pipeline/document_source_sort_by_count.h"

#include "mongo/base/string_data.h"
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

REGISTER_DOCUMENT_SOURCE(sortByCount,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSortByCount::createFromBson,
                         AllowedWithApiStrict::kAlways);

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

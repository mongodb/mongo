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

#include "mongo/db/pipeline/document_source_count.h"

#include "mongo/base/string_data.h"
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;
using std::string;

REGISTER_DOCUMENT_SOURCE(count,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceCount::createFromBson,
                         AllowedWithApiStrict::kAlways);

list<intrusive_ptr<DocumentSource>> DocumentSourceCount::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40156,
            str::stream() << "the count field must be a non-empty string",
            elem.type() == BSONType::string);

    StringData elemString = elem.valueStringData();
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

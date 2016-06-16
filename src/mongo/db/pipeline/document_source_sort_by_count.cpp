/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

REGISTER_DOCUMENT_SOURCE_ALIAS(sortByCount, DocumentSourceSortByCount::createFromBson);

vector<intrusive_ptr<DocumentSource>> DocumentSourceSortByCount::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    if (elem.type() == Object) {
        // Make sure that the sortByCount field is an expression inside an object
        BSONObj innerObj = elem.embeddedObject();
        uassert(40147,
                str::stream() << "the sortByCount field must be defined as a $-prefixed path or an "
                                 "expression inside an object",
                innerObj.firstElementFieldName()[0] == '$');
    } else if (elem.type() == String) {
        // Make sure that the sortByCount field is a $-prefixed path
        uassert(40148,
                str::stream() << "the sortByCount field must be defined as a $-prefixed path or an "
                                 "expression inside an object",
                (elem.valueStringData()[0] == '$'));
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

/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/pipeline/document_source_change_notification.h"

#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

REGISTER_MULTI_STAGE_ALIAS(changeNotification,
                           DocumentSourceChangeNotification::LiteParsed::parse,
                           DocumentSourceChangeNotification::createFromBson);

BSONObj DocumentSourceChangeNotification::buildMatch(BSONElement elem, const NamespaceString& nss) {
    auto target = nss.toString();
    return BSON("$match" << BSON(
                    "op" << BSON("$ne"
                                 << "n")
                         << "ts"
                         << BSON("$gt" << Timestamp())
                         << "$or"
                         << BSON_ARRAY(BSON("ns" << target)
                                       << BSON("op"
                                               << "c"
                                               << "$or"
                                               << BSON_ARRAY(BSON("o.renameCollection" << target)
                                                             << BSON("o.to" << target))))));
}

vector<intrusive_ptr<DocumentSource>> DocumentSourceChangeNotification::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO: Add sharding support here (SERVER-29141).
    uassert(40470,
            "The $changeNotification stage is not supported on sharded systems.",
            !expCtx->inRouter);
    uassert(40471,
            "Only default collation is allowed when using a $changeNotification stage.",
            !expCtx->getCollator());

    BSONObj matchObj = buildMatch(elem, expCtx->ns);
    BSONObj sortObj = BSON("$sort" << BSON("ts" << -1));

    auto matchSource = DocumentSourceMatch::createFromBson(matchObj.firstElement(), expCtx);
    auto sortSource = DocumentSourceSort::createFromBson(sortObj.firstElement(), expCtx);
    auto limitSource = DocumentSourceLimit::create(expCtx, 1);
    return {matchSource, sortSource, limitSource};
}

}  // namespace mongo

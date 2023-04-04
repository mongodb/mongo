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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(listSessions,
                         DocumentSourceListSessions::LiteParsed::parse,
                         DocumentSourceListSessions::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

boost::intrusive_ptr<DocumentSource> DocumentSourceListSessions::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->ns;

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << kStageName << " may only be run against "
                          << NamespaceString::kLogicalSessionsNamespace.ns(),
            nss == NamespaceString::kLogicalSessionsNamespace);

    const auto& spec = listSessionsParseSpec(kStageName, elem);
    if (const auto& pred = spec.getPredicate()) {
        // Predicate has already been determined and might have changed during optimization, use it
        // directly.
        return new DocumentSourceListSessions(*pred, pExpCtx, spec.getAllUsers(), spec.getUsers());
    }
    if (spec.getAllUsers()) {
        // No filtration. optimize() should later skip us.
        return new DocumentSourceListSessions(
            BSONObj(), pExpCtx, spec.getAllUsers(), spec.getUsers());
    }
    invariant(spec.getUsers() && !spec.getUsers()->empty());

    BSONArrayBuilder builder;
    for (const auto& uid : listSessionsUsersToDigests(spec.getUsers().value())) {
        ConstDataRange cdr = uid.toCDR();
        builder.append(BSONBinData(cdr.data(), cdr.length(), BinDataGeneral));
    }
    const auto& query = BSON("_id.uid" << BSON("$in" << builder.arr()));
    return new DocumentSourceListSessions(query, pExpCtx, spec.getAllUsers(), spec.getUsers());
}

Value DocumentSourceListSessions::serialize(SerializationOptions opts) const {
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484327);
    }
    ListSessionsSpec spec;
    spec.setAllUsers(_allUsers);
    spec.setUsers(_users);
    spec.setPredicate(_predicate);
    return Value(Document{{getSourceName(), spec.toBSON()}});
}

}  // namespace mongo

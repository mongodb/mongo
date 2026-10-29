// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_list_sessions.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(listSessions,
                                     DocumentSourceListSessions::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listSessions,
                                                   DocumentSourceListSessions,
                                                   ListSessionsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(listSessions, DocumentSourceListSessions::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceListSessions::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const NamespaceString& nss = pExpCtx->getNamespaceString();

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << kStageName << " may only be run against "
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg(),
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

    BSONArrayBuilder builder;
    for (const auto& uid : listSessionsUsersToDigests(spec.getUsers().value())) {
        ConstDataRange cdr = uid.toCDR();
        builder.append(BSONBinData(cdr.data(), cdr.length(), BinDataGeneral));
    }
    const auto& query = BSON("_id.uid" << BSON("$in" << builder.arr()));
    return new DocumentSourceListSessions(query, pExpCtx, spec.getAllUsers(), spec.getUsers());
}

Value DocumentSourceListSessions::serialize(const query_shape::SerializationOptions& opts) const {
    ListSessionsSpec spec;
    spec.setAllUsers(_allUsers);
    spec.setUsers(_users);
    spec.setPredicate(getPredicate());
    return Value(Document{{getSourceName(), spec.toBSON(opts)}});
}

}  // namespace mongo

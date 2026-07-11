// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_text_base.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/fts/fts_query.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const bool TextMatchExpressionBase::kCaseSensitiveDefault = false;
const bool TextMatchExpressionBase::kDiacriticSensitiveDefault = false;

TextMatchExpressionBase::TextMatchExpressionBase(std::string_view path)
    : LeafMatchExpression(TEXT, path) {}

void TextMatchExpressionBase::debugString(StringBuilder& debug, int indentationLevel) const {
    const fts::FTSQuery& ftsQuery = getFTSQuery();
    _debugAddSpace(debug, indentationLevel);
    debug << "TEXT : query=" << ftsQuery.getQuery() << ", language=" << ftsQuery.getLanguage()
          << ", caseSensitive=" << ftsQuery.getCaseSensitive()
          << ", diacriticSensitive=" << ftsQuery.getDiacriticSensitive();
    _debugStringAttachTagInfo(&debug);
}

void TextMatchExpressionBase::serialize(BSONObjBuilder* out,
                                        const query_shape::SerializationOptions& opts,
                                        bool includePath) const {
    const fts::FTSQuery& ftsQuery = getFTSQuery();
    out->append("$text",
                BSON("$search" << opts.serializeLiteral(ftsQuery.getQuery()) << "$language"
                               << opts.serializeLiteral(ftsQuery.getLanguage()) << "$caseSensitive"
                               << opts.serializeLiteral(ftsQuery.getCaseSensitive())
                               << "$diacriticSensitive"
                               << opts.serializeLiteral(ftsQuery.getDiacriticSensitive())));
}

bool TextMatchExpressionBase::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const TextMatchExpressionBase* realOther = static_cast<const TextMatchExpressionBase*>(other);

    return getFTSQuery().equivalent(realOther->getFTSQuery());
}

}  // namespace mongo

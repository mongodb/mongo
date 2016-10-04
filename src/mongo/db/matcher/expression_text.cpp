// expression_text.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/matcher/expression_text.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/stdx/memory.h"

namespace mongo {

Status TextMatchExpression::init(OperationContext* txn,
                                 const NamespaceString& nss,
                                 TextParams params) {
    _ftsQuery.setQuery(std::move(params.query));
    _ftsQuery.setLanguage(std::move(params.language));
    _ftsQuery.setCaseSensitive(params.caseSensitive);
    _ftsQuery.setDiacriticSensitive(params.diacriticSensitive);

    fts::TextIndexVersion version;
    {
        // Find text index.
        ScopedTransaction transaction(txn, MODE_IS);
        AutoGetDb autoDb(txn, nss.db(), MODE_IS);
        Lock::CollectionLock collLock(txn->lockState(), nss.ns(), MODE_IS);
        Database* db = autoDb.getDb();
        if (!db) {
            return {ErrorCodes::IndexNotFound,
                    str::stream() << "text index required for $text query (no such collection '"
                                  << nss.ns()
                                  << "')"};
        }
        Collection* collection = db->getCollection(nss);
        if (!collection) {
            return {ErrorCodes::IndexNotFound,
                    str::stream() << "text index required for $text query (no such collection '"
                                  << nss.ns()
                                  << "')"};
        }
        std::vector<IndexDescriptor*> idxMatches;
        collection->getIndexCatalog()->findIndexByType(txn, IndexNames::TEXT, idxMatches);
        if (idxMatches.empty()) {
            return {ErrorCodes::IndexNotFound, "text index required for $text query"};
        }
        if (idxMatches.size() > 1) {
            return {ErrorCodes::IndexNotFound, "more than one text index found for $text query"};
        }
        invariant(idxMatches.size() == 1);
        IndexDescriptor* index = idxMatches[0];
        const FTSAccessMethod* fam =
            static_cast<FTSAccessMethod*>(collection->getIndexCatalog()->getIndex(index));
        invariant(fam);

        // Extract version and default language from text index.
        version = fam->getSpec().getTextIndexVersion();
        if (_ftsQuery.getLanguage().empty()) {
            _ftsQuery.setLanguage(fam->getSpec().defaultLanguage().str());
        }
    }

    Status parseStatus = _ftsQuery.parse(version);
    if (!parseStatus.isOK()) {
        return parseStatus;
    }

    return setPath("_fts");
}

std::unique_ptr<MatchExpression> TextMatchExpression::shallowClone() const {
    auto expr = stdx::make_unique<TextMatchExpression>();
    // We initialize _ftsQuery here directly rather than calling init(), to avoid needing to examine
    // the index catalog.
    expr->_ftsQuery = _ftsQuery;
    invariantOK(expr->setPath("_fts"));
    if (getTag()) {
        expr->setTag(getTag()->clone());
    }
    return std::move(expr);
}

}  // namespace mongo

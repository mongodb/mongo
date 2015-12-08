/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/matcher/extensions_callback_real.h"

#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

ExtensionsCallbackReal::ExtensionsCallbackReal(OperationContext* txn, const NamespaceString* nss)
    : _txn(txn), _nss(nss) {}

StatusWithMatchExpression ExtensionsCallbackReal::parseText(BSONElement text) const {
    auto textParams = extractTextMatchExpressionParams(text);
    if (!textParams.isOK()) {
        return textParams.getStatus();
    }

    auto exp = stdx::make_unique<TextMatchExpression>();
    Status status = exp->init(_txn, *_nss, std::move(textParams.getValue()));
    if (!status.isOK()) {
        return status;
    }
    return {std::move(exp)};
}

StatusWithMatchExpression ExtensionsCallbackReal::parseWhere(BSONElement where) const {
    auto whereParams = extractWhereMatchExpressionParams(where);
    if (!whereParams.isOK()) {
        return whereParams.getStatus();
    }

    auto exp = stdx::make_unique<WhereMatchExpression>(_txn, std::move(whereParams.getValue()));
    Status status = exp->init(_nss->db());
    if (!status.isOK()) {
        return status;
    }
    return {std::move(exp)};
}

}  // namespace mongo

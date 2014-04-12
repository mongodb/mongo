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

#include "mongo/db/structure/btree/btree.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/prefix_access_method.h"

namespace mongo {
    PrefixAccessMethod::PrefixAccessMethod(IndexCatalogEntry* btreeState)
        : BtreeBasedAccessMethod(btreeState) {

        const IndexDescriptor* descriptor = btreeState->descriptor();

        uassert(17431,"Currently only single field prefix index is supported.",
                1 == descriptor->getNumFields());

        uassert(17432, "Currently prefix indexes cannot guarantee uniqueness. "
                "Use a regular index.", !descriptor->unique());

        ExpressionParams::parsePrefixParams(descriptor->infoObj(),
                                            &_prefixLength, &_prefixField);
    }


    void PrefixAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        ExpressionKeysPrivate::getPrefixKeys(obj, _prefixField, _prefixLength,
                                             _descriptor->isSparse(), keys);
    }

} // namespace mongo

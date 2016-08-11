/**
 * Copyright 2016 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source.h"

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/pipeline/parsed_add_fields.h"

namespace mongo {

using boost::intrusive_ptr;
using parsed_aggregation_projection::ParsedAddFields;

REGISTER_DOCUMENT_SOURCE_ALIAS(addFields, DocumentSourceAddFields::createFromBson);

std::vector<intrusive_ptr<DocumentSource>> DocumentSourceAddFields::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {

    // Confirm that the stage was called with an object.
    uassert(40272,
            str::stream() << "$addFields specification stage must be an object, got "
                          << typeName(elem.type()),
            elem.type() == Object);

    // Create the AddFields aggregation stage.
    return {new DocumentSourceSingleDocumentTransformation(
        expCtx, ParsedAddFields::create(elem.Obj()), "$addFields")};
};
}

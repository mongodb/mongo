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

#pragma once

#include "mongo/db/pipeline/document_source_single_document_transformation.h"

namespace mongo {

/**
 * $addFields adds or replaces the specified fields to/in the document while preserving the original
 * document. It is modeled on and throws the same errors as $project.
 *
 * This stage is also aliased as $set and functions the same way.
 */
class DocumentSourceAddFields final {
public:
    static constexpr StringData kStageName = "$addFields"_sd;
    static constexpr StringData kAliasNameSet = "$set"_sd;  // An alternate name for this stage.

    /**
     * Convenience method for creating a $addFields stage from 'addFieldsSpec'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        BSONObj addFieldsSpec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        StringData stageName = kStageName);

    /**
     * Parses a $addFields stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    // It is illegal to construct a DocumentSourceAddFields directly, use create() or
    // createFromBson() instead.
    DocumentSourceAddFields() = default;
};

}  // namespace mongo

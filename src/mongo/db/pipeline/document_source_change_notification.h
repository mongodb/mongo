/**
 * Copyright (C) 2017 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"

namespace mongo {

/**
 * The $changeNotification stage is an alias for a cursor on oplog followed by a $match stage and a
 * transform stage on mongod.
 */
class DocumentSourceChangeNotification final {
public:
    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec) {
            return stdx::make_unique<LiteParsed>();
        }

        bool isChangeNotification() const final {
            return true;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        // TODO SERVER-29138: Add required privileges.
        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            return {};
        }
    };

    class Transformation : public DocumentSourceSingleDocumentTransformation::TransformerInterface {
    public:
        ~Transformation() = default;
        Document applyTransformation(const Document& input) final;
        TransformerType getType() const final {
            return TransformerType::kChangeNotificationTransformation;
        };
        void optimize() final{};
        Document serializeStageOptions(
            boost::optional<ExplainOptions::Verbosity> explain) const final;
        DocumentSource::GetDepsReturn addDependencies(DepsTracker* deps) const final;
        DocumentSource::GetModPathsReturn getModifiedPaths() const final;
    };

    /**
     * Produce the BSON for the $match stage based on a $changeNotification stage.
     */
    static BSONObj buildMatch(BSONElement elem, const NamespaceString& nss);

    /**
     * Parses a $changeNotification stage from 'elem' and produces the $match and transformation
     * stages required.
     */
    static std::vector<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> createTransformationStage(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceChangeNotification directly, use createFromBson()
    // instead.
    DocumentSourceChangeNotification() = default;
};

}  // namespace mongo

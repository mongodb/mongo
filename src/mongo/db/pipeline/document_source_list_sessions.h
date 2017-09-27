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

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

/**
 * $listSessions: { allUsers: true/false, users: [ {user:"jsmith", db:"test"}, ... ] }
 * Return all sessions in the config.system.sessions collection
 * or just sessions for the currently logged in user. (Default: false)
 *
 * This is essentially an alias for {$match:{"_id.uid": myid}} or {$match:{}}
 * with appropriate constraints on stage positioning and security checks
 * based on ActionType::ListSessions
 */
class DocumentSourceListSessions final : public DocumentSourceMatch {
public:
    static const char* kStageName;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec) {
            return stdx::make_unique<LiteParsed>(listSessionsParseSpec(kStageName, spec));
        }

        explicit LiteParsed(const ListSessionsSpec& spec) : _spec(spec) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            return listSessionsRequiredPrivileges(_spec);
        }

        bool isInitialSource() const final {
            return true;
        }

        bool allowedToPassthroughFromMongos() const final {
            return _spec.getAllUsers();
        }

    private:
        const ListSessionsSpec _spec;
    };

    const char* getSourceName() const final {
        return kStageName;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        return Value(Document{{getSourceName(), _spec.toBSON()}});
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kFirst,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed};
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceListSessions(const BSONObj& query,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               const ListSessionsSpec& spec)
        : DocumentSourceMatch(query, pExpCtx), _spec(spec) {}

    const ListSessionsSpec _spec;
};

}  // namespace mongo

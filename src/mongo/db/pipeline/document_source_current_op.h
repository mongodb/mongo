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

namespace mongo {

class DocumentSourceCurrentOp final : public DocumentSourceNeedsMongod {
public:
    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const AggregationRequest& request,
                                                 const BSONElement& spec);

        explicit LiteParsed(bool allUsers) : _allUsers(allUsers) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos) const final {
            PrivilegeVector privileges;

            // In a sharded cluster, we always need the inprog privilege to run $currentOp.
            if (isMongos || _allUsers) {
                privileges.push_back({ResourcePattern::forClusterResource(), ActionType::inprog});
            }

            return privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

    private:
        const bool _allUsers;
    };

    using TruncationMode = MongodInterface::CurrentOpTruncateMode;
    using ConnMode = MongodInterface::CurrentOpConnectionsMode;
    using UserMode = MongodInterface::CurrentOpUserMode;

    static boost::intrusive_ptr<DocumentSourceCurrentOp> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        ConnMode includeIdleConnections = ConnMode::kExcludeIdle,
        UserMode includeOpsFromAllUsers = UserMode::kExcludeOthers,
        TruncationMode truncateOps = TruncationMode::kNoTruncation);

    GetNextResult getNext() final;

    const char* getSourceName() const final;

    InitialSourceType getInitialSourceType() const final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

private:
    DocumentSourceCurrentOp(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                            ConnMode includeIdleConnections = ConnMode::kExcludeIdle,
                            UserMode includeOpsFromAllUsers = UserMode::kExcludeOthers,
                            TruncationMode truncateOps = TruncationMode::kNoTruncation)
        : DocumentSourceNeedsMongod(pExpCtx),
          _includeIdleConnections(includeIdleConnections),
          _includeOpsFromAllUsers(includeOpsFromAllUsers),
          _truncateOps(truncateOps) {}

    ConnMode _includeIdleConnections = ConnMode::kExcludeIdle;
    UserMode _includeOpsFromAllUsers = UserMode::kExcludeOthers;
    TruncationMode _truncateOps = TruncationMode::kNoTruncation;

    std::string _shardName;

    std::vector<BSONObj> _ops;
    std::vector<BSONObj>::iterator _opsIter;
};

}  // namespace mongo

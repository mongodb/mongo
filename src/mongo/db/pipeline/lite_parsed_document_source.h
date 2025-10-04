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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class LiteParsedPipeline;

struct LiteParserOptions {
    // Allows the foreign collection of a lookup to be in a different database than the local
    // collection using "from: {db: ..., coll: ...}" syntax. Currently, this should only be used
    // for streams since this isn't allowed in MQL beyond some exemptions for internal
    // collection in the local database. While this flag also exists on expressionContext, we also
    // need this in the LiteParseContext to correctly throw errors when a non-streams pipeline tries
    // to use foreign db syntax for $lookup beyond the exempted internal collections during lite
    // parsing since lite parsing doesn't have an expressionContext.
    bool allowGenericForeignDbLookup = false;
};

/**
 * A lightly parsed version of a DocumentSource. It is not executable and not guaranteed to return a
 * parse error when encountering an invalid specification. Instead, the purpose of this class is to
 * make certain DocumentSource properties available before full parsing (e.g., getting the involved
 * foreign collections).
 */
class LiteParsedDocumentSource {
public:
    /*
     * This is the type of parser you should register using REGISTER_DOCUMENT_SOURCE. It need not
     * do any validation of options, only enough parsing to be able to implement the interface.
     *
     * The NamespaceString can be used to determine the namespace on which this aggregation is being
     * performed, and the BSONElement will be the element whose field name is the name of this stage
     * (e.g. the first and only element in {$limit: 1}).
     */
    using Parser = std::function<std::unique_ptr<LiteParsedDocumentSource>(
        const NamespaceString&, const BSONElement&, const LiteParserOptions&)>;

    struct LiteParserInfo {
        Parser parser;
        AllowedWithApiStrict allowedWithApiStrict;
        AllowedWithClientType allowedWithClientType;
    };

    LiteParsedDocumentSource(std::string parseTimeName)
        : _parseTimeName(std::move(parseTimeName)) {}

    virtual ~LiteParsedDocumentSource() = default;

    /**
     * Registers a DocumentSource with a spec parsing function, so that when a stage with the given
     * name is encountered, it will call 'parser' to construct that stage's specification object.
     * The flag 'allowedWithApiStrict' is used to control the allowance of the stage when
     * 'apiStrict' is set to true.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * document_source.h.
     */
    static void registerParser(const std::string& name,
                               Parser parser,
                               AllowedWithApiStrict allowedWithApiStrict,
                               AllowedWithClientType allowedWithClientType);

    /**
     * Function that will be used as an alternate parser for a document source that has been
     * disabled.
     */
    static std::unique_ptr<LiteParsedDocumentSource> parseDisabled(
        NamespaceString nss,
        const BSONElement& spec,
        const LiteParserOptions& options = LiteParserOptions{}) {
        uasserted(
            ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << spec.fieldName()
                          << " is not allowed with the current configuration. You may need to "
                             "enable the corresponding feature flag");
    }

    /**
     * Returns the 'LiteParserInfo' for the specified stage name.
     */
    static const LiteParserInfo& getInfo(const std::string& stageName);

    /**
     * Constructs a LiteParsedDocumentSource from the user-supplied BSON, or throws a
     * AssertionException.
     *
     * Extracts the first field name from 'spec', and delegates to the parser that was registered
     * with that field name using registerParser() above.
     */
    static std::unique_ptr<LiteParsedDocumentSource> parse(
        const NamespaceString& nss,
        const BSONObj& spec,
        const LiteParserOptions& options = LiteParserOptions{});

    /**
     * Returns the foreign collection(s) referenced by this stage (that is, any collection that
     * the pipeline references, but doesn't get locked), if any.
     */
    virtual stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const = 0;

    /**
     * Returns the foreign collections(s) referenced by this stage that potentially will be
     * involved in query execution (that is, a collection that the pipeline references, and gets
     * locked for the purposes of query execution), if any.
     */
    virtual void getForeignExecutionNamespaces(stdx::unordered_set<NamespaceString>& nssSet) const {
    }

    /**
     * Returns a list of the privileges required for this stage.
     */
    virtual PrivilegeVector requiredPrivileges(bool isMongos,
                                               bool bypassDocumentValidation) const = 0;

    /**
     * Does any custom assertions necessary to validate this stage is permitted in the given API
     * Version. For example, if certain stage parameters are permitted but others excluded, that
     * should happen here.
     */
    virtual void assertPermittedInAPIVersion(const APIParameters&) const {
        // By default there are no custom checks needed. The 'AllowedWithApiStrict' flag should take
        // care of most cases.
    }

    /**
     * Returns true if this is a $collStats stage.
     */
    virtual bool isCollStats() const {
        return false;
    }

    /**
     * Returns true if this is a $indexStats stage.
     */
    virtual bool isIndexStats() const {
        return false;
    }

    /**
     * Returns true if this is a $changeStream stage.
     */
    virtual bool isChangeStream() const {
        return false;
    }

    /**
     * Returns true if this is a write stage.
     */
    virtual bool isWriteStage() const {
        return false;
    }

    /**
     * Returns true if this stage is an initial source and should run just once on the entire
     * cluster.
     */
    virtual bool generatesOwnDataOnce() const {
        return false;
    }

    /**
     * Returns true if this stage does not require an input source.
     */
    virtual bool isInitialSource() const {
        return false;
    }

    /**
     * Returns true if this stage should make the aggregation command exempt from ingress admission
     * control.
     */
    virtual bool isExemptFromIngressAdmissionControl() const {
        return false;
    }

    /**
     * Returns true if this is a search stage ($search, $vectorSearch, $rankFusion, etc.)
     */
    virtual bool isSearchStage() const {
        return false;
    }

    /**
     * Returns true if this is a $rankFusion pipeline
     */
    virtual bool isHybridSearchStage() const {
        return false;
    }

    /**
     * Returns true if this stage require knowledge of the collection default collation at parse
     * time, false otherwise. This is useful to know as it could save a network request to discern
     * the collation.
     * TODO SERVER-81991: Delete this function once all unsharded collections are tracked in the
     * sharding catalog as unsplittable along with their collation.
     */
    virtual bool requiresCollationForParsingUnshardedAggregate() const {
        return false;
    }

    /**
     * Returns false if aggregation stages manually opt out of mandatory authorization checks, true
     otherwise. Will enable mandatory authorization checks by default.
     */
    virtual bool requiresAuthzChecks() const {
        return true;
    }

    /**
     * Returns Status::OK() if the involved namespace 'nss' is allowed to be sharded. The behavior
     * is to allow by default. Stages should opt-out if foreign collections are not allowed to be
     * sharded by returning a Status with a message explaining why.
     */
    virtual Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                                  bool inMultiDocumentTransaction) const {
        return Status::OK();
    }

    /**
     * Verifies that this stage is allowed to run with the specified read concern level.
     */
    virtual ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                         bool isImplicitDefault) const {
        return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    }

    /**
     * Verifies that this stage is allowed to run in a multi-document transaction. Throws a
     * UserException if not compatible. This should only be called if the caller has determined the
     * current operation is part of a transaction.
     */
    virtual void assertSupportsMultiDocumentTransaction() const {}

    /**
     * Returns this document source's subpipelines. If none exist, a reference to an empty vector
     * is returned.
     */
    virtual const std::vector<LiteParsedPipeline>& getSubPipelines() const;

    /**
     * Returns the name of the stage that this LiteParsedDocumentSource represents.
     */
    const std::string& getParseTimeName() const {
        return _parseTimeName;
    }

protected:
    void transactionNotSupported(StringData stageName) const {
        uasserted(ErrorCodes::OperationNotSupportedInTransaction,
                  str::stream() << "Operation not permitted in transaction :: caused by :: "
                                << "Aggregation stage " << stageName << " cannot run within a "
                                << "multi-document transaction.");
    }

    ReadConcernSupportResult onlySingleReadConcernSupported(StringData stageName,
                                                            repl::ReadConcernLevel supportedLevel,
                                                            repl::ReadConcernLevel candidateLevel,
                                                            bool isImplicitDefault) const {
        return {{candidateLevel != supportedLevel && !isImplicitDefault,
                 {ErrorCodes::InvalidOptions,
                  str::stream() << "Aggregation stage " << stageName
                                << " cannot run with a readConcern other than '"
                                << repl::readConcernLevels::toString(supportedLevel)
                                << "'. Current readConcern: "
                                << repl::readConcernLevels::toString(candidateLevel)}},
                {{ErrorCodes::InvalidOptions,
                  str::stream() << "Aggregation stage " << stageName
                                << " does not permit default readConcern to be applied."}}};
    }

    ReadConcernSupportResult onlyReadConcernLocalSupported(StringData stageName,
                                                           repl::ReadConcernLevel level,
                                                           bool isImplicitDefault) const {
        return onlySingleReadConcernSupported(
            stageName, repl::ReadConcernLevel::kLocalReadConcern, level, isImplicitDefault);
    }

private:
    /**
     * Give access to 'parserMap' so we can remove a registered parser. unregisterParser_forTest is
     * only meant to be used in the context of unit tests. This is because the parserMap is not
     * thread safe, so modifying it at runtime is unsafe.
     */
    friend class DocumentSourceExtensionTest;
    static void unregisterParser_forTest(const std::string& name);

    std::string _parseTimeName;
};

class LiteParsedDocumentSourceDefault final : public LiteParsedDocumentSource {
public:
    /**
     * Creates the default LiteParsedDocumentSource. This should be used with caution. Make sure
     * your stage doesn't need to communicate any special behavior before registering a
     * DocumentSource using this parser. Additionally, explicitly ensure your stage does not require
     * authorization checks.
     */
    static std::unique_ptr<LiteParsedDocumentSourceDefault> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
        return std::make_unique<LiteParsedDocumentSourceDefault>(spec.fieldName());
    }

    LiteParsedDocumentSourceDefault(std::string parseTimeName)
        : LiteParsedDocumentSource(std::move(parseTimeName)) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    /**
     * requiresAuthzChecks() is overriden to false because requiredPrivileges() returns an empty
     * vector and has no authz checks by default.
     */
    bool requiresAuthzChecks() const override {
        return false;
    }
};

class LiteParsedDocumentSourceInternal final : public LiteParsedDocumentSource {
public:
    /**
     * Creates the default LiteParsedDocumentSource for internal document sources. This requires
     * the privilege on 'internal' action. This should still be used with caution. Make sure your
     * stage doesn't need to communicate any special behavior before registering a DocumentSource
     * using this parser.
     */
    static std::unique_ptr<LiteParsedDocumentSourceInternal> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
        return std::make_unique<LiteParsedDocumentSourceInternal>(spec.fieldName());
    }

    LiteParsedDocumentSourceInternal(std::string parseTimeName)
        : LiteParsedDocumentSource(std::move(parseTimeName)) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {Privilege(ResourcePattern::forClusterResource(boost::none),
                          ActionSet{ActionType::internal})};
    }
};

/**
 * Helper class for DocumentSources which reference a foreign collection.
 */
class LiteParsedDocumentSourceForeignCollection : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceForeignCollection(std::string parseTimeName, NamespaceString foreignNss)
        : LiteParsedDocumentSource(std::move(parseTimeName)), _foreignNss(std::move(foreignNss)) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return {_foreignNss};
    }

    PrivilegeVector requiredPrivileges(bool isMongos,
                                       bool bypassDocumentValidation) const override = 0;

protected:
    NamespaceString _foreignNss;
};

/**
 * Helper class for DocumentSources which can reference one or more child pipelines.
 */
class LiteParsedDocumentSourceNestedPipelines : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceNestedPipelines(std::string parseTimeName,
                                            boost::optional<NamespaceString> foreignNss,
                                            std::vector<LiteParsedPipeline> pipelines);

    LiteParsedDocumentSourceNestedPipelines(std::string parseTimeName,
                                            boost::optional<NamespaceString> foreignNss,
                                            boost::optional<LiteParsedPipeline> pipeline);

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final;

    void getForeignExecutionNamespaces(stdx::unordered_set<NamespaceString>& nssSet) const override;

    bool isExemptFromIngressAdmissionControl() const override;

    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool inMultiDocumentTransaction) const override;

    const std::vector<LiteParsedPipeline>& getSubPipelines() const override {
        return _pipelines;
    }

    /**
     * Check the read concern constraints of all sub-pipelines. If the stage that owns the
     * sub-pipelines has its own constraints this should be overridden to take those into account.
     */
    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override;

protected:
    /**
     * Simple implementation that only gets the privileges needed by children pipelines.
     */
    PrivilegeVector requiredPrivilegesBasic(bool isMongos, bool bypassDocumentValidation) const;

    boost::optional<NamespaceString> _foreignNss;
    std::vector<LiteParsedPipeline> _pipelines;
};
}  // namespace mongo

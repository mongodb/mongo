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

#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <vector>

#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

class LiteParsedPipeline;

/**
 * A lightly parsed version of a DocumentSource. It is not executable and not guaranteed to return a
 * parse error when encountering an invalid specification. Instead, the purpose of this class is to
 * make certain DocumentSource properties available before full parsing (e.g., getting the involved
 * foreign collections).
 */
class LiteParsedDocumentSource {
public:
    /**
     * Flags to mark stages with different allowance constrains when API versioning is enabled.
     */
    enum class AllowedWithApiStrict {
        // The stage is always allowed in the pipeline regardless of API versions.
        kAlways,
        // The stage is allowed only for internal client when 'apiStrict' is set to true.
        kInternal,
        // The stage is never allowed in API version '1' when 'apiStrict' is set to true.
        kNeverInVersion1
    };

    LiteParsedDocumentSource(std::string parseTimeName)
        : _parseTimeName(std::move(parseTimeName)) {}

    virtual ~LiteParsedDocumentSource() = default;

    /*
     * This is the type of parser you should register using REGISTER_DOCUMENT_SOURCE. It need not
     * do any validation of options, only enough parsing to be able to implement the interface.
     *
     * The NamespaceString can be used to determine the namespace on which this aggregation is being
     * performed, and the BSONElement will be the element whose field name is the name of this stage
     * (e.g. the first and only element in {$limit: 1}).
     */
    using Parser = std::function<std::unique_ptr<LiteParsedDocumentSource>(const NamespaceString&,
                                                                           const BSONElement&)>;
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
                               AllowedWithApiStrict allowedWithApiStrict);

    /**
     * Returns the 'ApiVersionAllowanceFlag' flag value for the specified stage name.
     */
    static AllowedWithApiStrict getApiVersionAllowanceFlag(std::string stageName);

    /**
     * Constructs a LiteParsedDocumentSource from the user-supplied BSON, or throws a
     * AssertionException.
     *
     * Extracts the first field name from 'spec', and delegates to the parser that was registered
     * with that field name using registerParser() above.
     */
    static std::unique_ptr<LiteParsedDocumentSource> parse(const NamespaceString& nss,
                                                           const BSONObj& spec);

    /**
     * Returns the foreign collection(s) referenced by this stage, if any.
     */
    virtual stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const = 0;

    /**
     * Returns a list of the privileges required for this stage.
     */
    virtual PrivilegeVector requiredPrivileges(bool isMongos,
                                               bool bypassDocumentValidation) const = 0;

    /**
     * Returns true if this is a $collStats stage.
     */
    virtual bool isCollStats() const {
        return false;
    }

    virtual bool isCollStatsWithCount() const {
        return false;
    }

    /**
     * Returns true if this is a $changeStream stage.
     */
    virtual bool isChangeStream() const {
        return false;
    }

    /**
     * Returns true if this stage does not require an input source.
     */
    virtual bool isInitialSource() const {
        return false;
    }

    /**
     * Returns true if this stage may be forwarded from mongos unmodified.
     */
    virtual bool allowedToPassthroughFromMongos() const {
        return true;
    }

    /**
     * Returns true if the involved namespace 'nss' is allowed to be sharded. The behavior is to
     * allow by default and stages should opt-out if foreign collections are not allowed to be
     * sharded.
     */
    virtual bool allowShardedForeignCollection(NamespaceString nss) const {
        return true;
    }

    /**
     * Verifies that this stage is allowed to run with the specified read concern level.
     */
    virtual ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level) const {
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

    ReadConcernSupportResult onlySingleReadConcernSupported(
        StringData stageName,
        repl::ReadConcernLevel supportedLevel,
        repl::ReadConcernLevel candidateLevel) const {
        return {{candidateLevel != supportedLevel,
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
                                                           repl::ReadConcernLevel level) const {
        return onlySingleReadConcernSupported(
            stageName, repl::ReadConcernLevel::kLocalReadConcern, level);
    }

private:
    std::string _parseTimeName;
};

class LiteParsedDocumentSourceDefault final : public LiteParsedDocumentSource {
public:
    /**
     * Creates the default LiteParsedDocumentSource. This should be used with caution. Make sure
     * your stage doesn't need to communicate any special behavior before registering a
     * DocumentSource using this parser.
     */
    static std::unique_ptr<LiteParsedDocumentSourceDefault> parse(const NamespaceString& nss,
                                                                  const BSONElement& spec) {
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

    virtual PrivilegeVector requiredPrivileges(bool isMongos,
                                               bool bypassDocumentValidation) const = 0;

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

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final override;

    bool allowedToPassthroughFromMongos() const override;

    bool allowShardedForeignCollection(NamespaceString nss) const override;

    const std::vector<LiteParsedPipeline>& getSubPipelines() const override {
        return _pipelines;
    }

protected:
    boost::optional<NamespaceString> _foreignNss;
    std::vector<LiteParsedPipeline> _pipelines;
};
}  // namespace mongo

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
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

/**
 * A lightly parsed version of a DocumentSource. It is not executable and not guaranteed to return a
 * parse error when encountering an invalid specification. Instead, the purpose of this class is to
 * make certain DocumentSource properties available before full parsing (e.g., getting the involved
 * foreign collections).
 */
class LiteParsedDocumentSource {
public:
    virtual ~LiteParsedDocumentSource() = default;

    /*
     * This is the type of parser you should register using REGISTER_DOCUMENT_SOURCE. It need not
     * do any validation of options, only enough parsing to be able to implement the interface.
     *
     * The AggregationRequest can be used to determine related information like the namespace on
     * which this aggregation is being performed, and the BSONElement will be the element whose
     * field name is the name of this stage (e.g. the first and only element in {$limit: 1}).
     */
    using Parser = std::function<std::unique_ptr<LiteParsedDocumentSource>(
        const AggregationRequest&, const BSONElement&)>;

    /**
     * Registers a DocumentSource with a spec parsing function, so that when a stage with the given
     * name is encountered, it will call 'parser' to construct that stage's specification object.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * document_source.h.
     */
    static void registerParser(const std::string& name, Parser parser);

    /**
     * Constructs a LiteParsedDocumentSource from the user-supplied BSON, or throws a
     * AssertionException.
     *
     * Extracts the first field name from 'spec', and delegates to the parser that was registered
     * with that field name using registerParser() above.
     */
    static std::unique_ptr<LiteParsedDocumentSource> parse(const AggregationRequest& request,
                                                           const BSONObj& spec);

    /**
     * Returns the foreign collection(s) referenced by this stage, if any.
     */
    virtual stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const = 0;

    /**
     * Returns a list of the privileges required for this stage.
     */
    virtual PrivilegeVector requiredPrivileges(bool isMongos) const = 0;

    /**
     * Returns true if this is a $collStats stage.
     */
    virtual bool isCollStats() const {
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
     * Returns true if this stage may be forwarded to shards from a mongos.
     */
    virtual bool allowedToForwardFromMongos() const {
        return true;
    }

    /**
     * Returns true if this stage may be forwarded from Mongos unmodified.
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
     * Verifies that this stage is allowed to run with the specified read concern. Throws a
     * UserException if not compatible.
     */
    virtual void assertSupportsReadConcern(const repl::ReadConcernArgs& readConcern) const {}
};

class LiteParsedDocumentSourceDefault final : public LiteParsedDocumentSource {
public:
    /**
     * Creates the default LiteParsedDocumentSource. This should be used with caution. Make sure
     * your stage doesn't need to communicate any special behavior before registering a
     * DocumentSource using this parser.
     */
    static std::unique_ptr<LiteParsedDocumentSourceDefault> parse(const AggregationRequest& request,
                                                                  const BSONElement& spec) {
        return stdx::make_unique<LiteParsedDocumentSourceDefault>();
    }

    LiteParsedDocumentSourceDefault() = default;

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos) const final {
        return {};
    }
};

/**
 * Helper class for DocumentSources which reference one or more foreign collections.
 */
class LiteParsedDocumentSourceForeignCollections : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceForeignCollections(NamespaceString foreignNss,
                                               PrivilegeVector privileges)
        : _foreignNssSet{std::move(foreignNss)}, _requiredPrivileges(std::move(privileges)) {}

    LiteParsedDocumentSourceForeignCollections(stdx::unordered_set<NamespaceString> foreignNssSet,
                                               PrivilegeVector privileges)
        : _foreignNssSet(std::move(foreignNssSet)), _requiredPrivileges(std::move(privileges)) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return {_foreignNssSet};
    }

    PrivilegeVector requiredPrivileges(bool isMongos) const final {
        return _requiredPrivileges;
    }

protected:
    stdx::unordered_set<NamespaceString> _foreignNssSet;

private:
    PrivilegeVector _requiredPrivileges;
};
}  // namespace mongo

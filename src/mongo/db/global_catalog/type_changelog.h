// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/version_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the config.changelog or
 * config.actionlog collections. All manipulation of documents coming from that collection should be
 * done with this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeLogType {
public:
    // Field names and types in the changelog collection type.
    static const BSONField<std::string> changeId;
    static const BSONField<std::string> server;
    static const BSONField<std::string> shard;
    static const BSONField<std::string> clientAddr;
    static const BSONField<Date_t> time;
    static const BSONField<std::string> what;
    static const BSONField<BSONObj> versionContext;
    static const BSONField<std::string> ns;
    static const BSONField<BSONObj> details;

    // Name of the chunks collection in the config server.
    static const NamespaceString ConfigNS;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Constructs a new ChangeLogType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ChangeLogType> fromBSON(const BSONObj& source);

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getChangeId() const {
        return _changeId.get();
    }
    void setChangeId(const std::string& changeId);

    const std::string& getServer() const {
        return _server.get();
    }
    void setServer(const std::string& server);

    const std::string& getShard() const {
        return _shard.get();
    }
    void setShard(const std::string& shard);

    const std::string& getClientAddr() const {
        return _clientAddr.get();
    }
    void setClientAddr(const std::string& clientAddr);

    const Date_t& getTime() const {
        return _time.get();
    }
    void setTime(const Date_t& time);

    const std::string& getWhat() const {
        return _what.get();
    }
    void setWhat(const std::string& what);

    boost::optional<VersionContext> getVersionContext() const {
        return _versionContext;
    }
    void setVersionContext(const VersionContext& vCtx);

    const NamespaceString& getNS() const {
        return _ns.get();
    }
    void setNS(const NamespaceString& ns);

    const BSONObj& getDetails() const {
        return _details.get();
    }
    void setDetails(const BSONObj& details);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) id for this change "<hostname>-<current_time>-<increment>"
    boost::optional<std::string> _changeId;
    // (M) hostname of server that we are making the change on.
    boost::optional<std::string> _server;
    // (O) id of shard making the change, or "config" for configSvrs
    boost::optional<std::string> _shard;
    // (M) hostname:port of the client that made this change
    boost::optional<std::string> _clientAddr;
    // (M) time this change was made
    boost::optional<Date_t> _time;
    // (M) description of the change
    boost::optional<std::string> _what;
    // (O) versionContext under which the change was made
    boost::optional<VersionContext> _versionContext;
    // (O) database or collection this change applies to
    boost::optional<NamespaceString> _ns;
    // (M) A BSONObj containing extra information about some operations
    boost::optional<BSONObj> _details;
};

}  // namespace mongo

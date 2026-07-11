// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/rpc/message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_types.h"

#include <memory>
#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class OperationContext;

/**
 * Embedded calls to the local server using the DBClientBase API without going over the network.
 *
 * Caller does not need to lock, that is handled within.
 *
 * All operations are performed within the scope of a passed-in OperationContext (except when
 * using the deprecated constructor). You must ensure that the OperationContext is valid when
 * calling into any function.
 */
class DBDirectClient : public DBClientBase {
public:
    DBDirectClient(OperationContext* opCtx);

    using DBClientBase::createCollection;
    using DBClientBase::createIndex;
    using DBClientBase::createIndexes;
    using DBClientBase::dropCollection;
    using DBClientBase::dropDatabase;
    using DBClientBase::find;
    using DBClientBase::findOne;
    using DBClientBase::getCollectionInfos;
    using DBClientBase::getDatabaseInfos;
    using DBClientBase::getIndexSpecs;
    using DBClientBase::insert;
    using DBClientBase::insertAcknowledged;
    using DBClientBase::remove;
    using DBClientBase::removeAcknowledged;
    using DBClientBase::runCommand;
    using DBClientBase::update;
    using DBClientBase::updateAcknowledged;

    std::unique_ptr<DBClientCursor> find(FindCommandRequest findRequest,
                                         const ReadPreferenceSetting& readPref,
                                         ExhaustMode exhaustMode) override;

    long long count(
        const NamespaceStringOrUUID& nsOrUuid,
        const BSONObj& query = BSONObj(),
        int options = 0,
        int limit = 0,
        int skip = 0,
        const boost::optional<repl::ReadConcernArgs>& readConcernObj = boost::none) override;

    /**
     * The insert, update, and remove commands only check the top level error status. The caller is
     * responsible for checking the writeErrors element for errors during execution.
     */
    write_ops::InsertCommandReply insert(const write_ops::InsertCommandRequest& insert);
    write_ops::UpdateCommandReply update(const write_ops::UpdateCommandRequest& update);
    write_ops::DeleteCommandReply remove(const write_ops::DeleteCommandRequest& remove);

    write_ops::FindAndModifyCommandReply findAndModify(
        const write_ops::FindAndModifyCommandRequest& findAndModify);

protected:
    auth::ValidatedTenancyScope _createInnerRequestVTS(
        const boost::optional<TenantId>& tenantId) const override;

private:
    bool isFailed() const override;

    bool isStillConnected() override;

    std::string toString() const override;

    std::string getServerAddress() const override;

    std::string getLocalAddress() const override;

    void say(Message& toSend, bool isRetry = false, std::string* actualServer = nullptr) override;

    ConnectionString::ConnectionType type() const override;

    double getSoTimeout() const override;

    int getMinWireVersion() final;
    int getMaxWireVersion() final;

    bool isReplicaSetMember() const final;

    bool isMongos() const final {
        return false;
    }

    bool isTLS() final {
        return false;
    }

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() override {
        invariant(false);
        return nullptr;
    }
#endif

    void _auth(const BSONObj& params) override;

    Message _call(Message& toSend, std::string* actualServer) override;

    OperationContext* _opCtx;
};

}  // namespace mongo

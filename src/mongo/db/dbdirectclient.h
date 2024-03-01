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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/rpc/message.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_types.h"

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

    long long count(NamespaceStringOrUUID nsOrUuid,
                    const BSONObj& query = BSONObj(),
                    int options = 0,
                    int limit = 0,
                    int skip = 0,
                    boost::optional<BSONObj> readConcernObj = boost::none) override;

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

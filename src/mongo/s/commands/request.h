// request.h
/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/util/net/message.h"

namespace mongo {

class Client;
class OperationContext;

namespace transport {
class Session;
}  // namespace transport

class Request {
    MONGO_DISALLOW_COPYING(Request);

public:
    Request(Message& m);

    const char* getns() const {
        return _d.getns();
    }

    const char* getnsIfPresent() const {
        return _d.messageShouldHaveNs() ? _d.getns() : "";
    }

    int op() const {
        return _m.operation();
    }

    bool expectResponse() const {
        return op() == dbQuery || op() == dbGetMore;
    }

    bool isCommand() const;

    int32_t id() const {
        return _id;
    }

    Message& m() {
        return _m;
    }
    DbMessage& d() {
        return _d;
    }

    transport::Session* session() const;

    void process(OperationContext* txn, int attempt = 0);

    void init(OperationContext* txn);

private:
    Client* const _clientInfo;

    Message& _m;
    DbMessage _d;

    int32_t _id;

    bool _didInit;
};
}

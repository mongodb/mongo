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

#include "mongo/pch.h"

#include "mongo/db/dbmessage.h"
#include "mongo/s/config.h"
#include "mongo/util/net/message.h"

namespace mongo {


    class OpCounters;
    class ClientInfo;

    class Request : boost::noncopyable {
    public:
        Request( Message& m, AbstractMessagingPort* p );

        // ---- message info -----


        const char * getns() const {
            return _d.getns();
        }
        int op() const {
            return _m.operation();
        }
        bool expectResponse() const {
            return op() == dbQuery || op() == dbGetMore;
        }
        bool isCommand() const;

        MSGID id() const {
            return _id;
        }

        DBConfigPtr getConfig() const {
            verify( _didInit );
            return _config;
        }
        bool isShardingEnabled() const {
            verify( _didInit );
            return _config->isShardingEnabled();
        }

        ChunkManagerPtr getChunkManager() const {
            verify( _didInit );
            return _chunkManager;
        }

        ClientInfo * getClientInfo() const {
            return _clientInfo;
        }

        // ---- remote location info -----


        Shard primaryShard() const ;

        // ---- low level access ----

        void reply( Message & response , const string& fromServer );

        Message& m() { return _m; }
        DbMessage& d() { return _d; }
        AbstractMessagingPort* p() const { return _p; }

        void process( int attempt = 0 );

        void gotInsert();

        void init();

        void reset();

    private:
        Message& _m;
        DbMessage _d;
        AbstractMessagingPort* _p;

        MSGID _id;
        DBConfigPtr _config;
        ChunkManagerPtr _chunkManager;

        ClientInfo * _clientInfo;

        OpCounters* _counter;

        bool _didInit;
    };

}

#include "strategy.h"

/**
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs.h"

#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/repl_set_impl.h"
#include "mongo/db/server_parameters.h"

using namespace std;

namespace mongo {
namespace repl {
    
    using namespace bson;

    bool replSet = false;
    ReplSet *theReplSet = 0;

    // This is a bitmask with the first bit set. It's used to mark connections that should be kept
    // open during stepdowns
    const unsigned ScopedConn::keepOpen = 1;

    bool isCurrentlyAReplSetPrimary() {
        return theReplSet && theReplSet->isPrimary();
    }

    void sethbmsg(const string& s, const int level) {
        if (theReplSet) {
            theReplSet->sethbmsg(s, level);
        }
    }

    ReplSet::ReplSet() {
    }

    ReplSet* ReplSet::make(ReplSetCmdline& replSetCmdline) {
        auto_ptr<ReplSet> ret(new ReplSet());
        ret->init(replSetCmdline);
        return ret.release();
    }

    ReplSetImpl::StartupStatus ReplSetImpl::startupStatus = PRESTART;
    DiagStr ReplSetImpl::startupStatusMsg;
    ReplicationStartSynchronizer ReplSetImpl::rss;

    void ReplSet::haveNewConfig(ReplSetConfig& newConfig, bool addComment) {
        bo comment;
        if( addComment )
            comment = BSON( "msg" << "Reconfig set" << "version" << newConfig.version );

        newConfig.saveConfigLocally(comment);

        try {
            BSONObj oldConfForAudit = config().asBson();
            BSONObj newConfForAudit = newConfig.asBson();
            audit::logReplSetReconfig(ClientBasic::getCurrent(),
                                      &oldConfForAudit,
                                      &newConfForAudit);
            if (initFromConfig(newConfig, true)) {
                log() << "replSet replSetReconfig new config saved locally" << rsLog;
            }
        }
        catch(DBException& e) {
            log() << "replSet error unexpected exception in haveNewConfig() : " << e.toString() << rsLog;
            _fatal();
        }
        catch(...) {
            log() << "replSet error unexpected exception in haveNewConfig()" << rsLog;
            _fatal();
        }
    }

    void Manager::msgReceivedNewConfig(BSONObj o) {
        log() << "replset msgReceivedNewConfig version: " << o["version"].toString() << rsLog;
        scoped_ptr<ReplSetConfig> config(ReplSetConfig::make(o));
        if( config->version > rs->config().version )
            theReplSet->haveNewConfig(*config, false);
        else {
            log() << "replSet info msgReceivedNewConfig but version isn't higher " <<
                  config->version << ' ' << rs->config().version << rsLog;
        }
    }

    /* forked as a thread during startup
       it can run quite a while looking for config.  but once found,
       a separate thread takes over as ReplSetImpl::Manager, and this thread
       terminates.
    */
    void startReplSets(ReplSetCmdline *replSetCmdline) {
        Client::initThread("rsStart");
        try {
            verify( theReplSet == 0 );
            if( replSetCmdline == 0 ) {
                verify(!replSet);
                return;
            }
            replLocalAuth();
            (theReplSet = ReplSet::make(*replSetCmdline))->go();
        }
        catch(std::exception& e) {
            log() << "replSet caught exception in startReplSets thread: " << e.what() << rsLog;
            if( theReplSet )
                theReplSet->fatal();
        }
        cc().shutdown();
    }

    void ReplSet::shutdown() {
        BackgroundSync::shutdown();
    }

    void replLocalAuth() {
        cc().getAuthorizationSession()->grantInternalAuthorization();
    }

    class ReplIndexPrefetch : public ServerParameter {
    public:
        ReplIndexPrefetch()
            : ServerParameter( ServerParameterSet::getGlobal(), "replIndexPrefetch" ) {
        }

        virtual ~ReplIndexPrefetch() {
        }

        const char * _value() {
            if (!theReplSet)
                return "uninitialized";
            ReplSetImpl::IndexPrefetchConfig ip = theReplSet->getIndexPrefetchConfig();
            switch (ip) {
            case ReplSetImpl::PREFETCH_NONE:
                return "none";
            case ReplSetImpl::PREFETCH_ID_ONLY:
                return "_id_only";
            case ReplSetImpl::PREFETCH_ALL:
                return "all";
            default:
                return "invalid";
            }
        }

        virtual void append(OperationContext* txn, BSONObjBuilder& b, const string& name) {
            b.append( name, _value() );
        }

        virtual Status set( const BSONElement& newValueElement ) {
            if (!theReplSet) {
                return Status( ErrorCodes::BadValue, "replication is not enabled" );
            }

            std::string prefetch = newValueElement.valuestrsafe();
            return setFromString( prefetch );
        }

        virtual Status setFromString( const string& prefetch ) {
            log() << "changing replication index prefetch behavior to " << prefetch << endl;

            ReplSetImpl::IndexPrefetchConfig prefetchConfig;

            if (prefetch == "none")
                prefetchConfig = ReplSetImpl::PREFETCH_NONE;
            else if (prefetch == "_id_only")
                prefetchConfig = ReplSetImpl::PREFETCH_ID_ONLY;
            else if (prefetch == "all")
                prefetchConfig = ReplSetImpl::PREFETCH_ALL;
            else {
                return Status( ErrorCodes::BadValue,
                               str::stream() << "unrecognized indexPrefetch setting: " << prefetch );
            }

            theReplSet->setIndexPrefetchConfig(prefetchConfig);
            return Status::OK();
        }

    } replIndexPrefetch;
} // namespace repl
} // namespace mongo

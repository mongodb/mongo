// top.cpp

#include "stdafx.h"
#include "top.h"
#include "../../util/message.h"
#include "../commands.h"

namespace mongo {
    
    void Top::record( const string& ns , int op , int lockType , long long micros ){
        boostlock lk(_lock);

        CollectionData& coll = _usage[ns];
        coll.total.inc( micros );
        
        if ( lockType > 0 )
            coll.writeLock.inc( micros );
        else if ( lockType < 0 )
            coll.readLock.inc( micros );
        
        switch ( op ){
        case 0:
            // use 0 for unknown, non-specific
            break;
        case dbUpdate:
            coll.update.inc( micros );
            break;
        case dbInsert:
            coll.insert.inc( micros );
            break;
        case dbQuery:
            coll.queries.inc( micros );
            break;
        case dbGetMore:
            coll.getmore.inc( micros );
            break;
        case dbDelete:
            coll.remove.inc( micros );
            break;
        case opReply: 
        case dbMsg:
        case dbKillCursors:
            log() << "unexpected op in Top::record: " << op << endl;
            break;
        default:
            log() << "unknown op in Top::record: " << op << endl;
        }

    }

    class TopCmd : public Command {
    public:
        TopCmd() : Command( "top" ){}

        virtual bool slaveOk(){ return true; }
        virtual bool readOnly(){ return true; }
        virtual bool adminOnly(){ return true; }
        virtual void help( stringstream& help ) const { help << "usage by collection"; }
        
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            result.append( "blah" , "glarb" );
            return true;
        }
        
    } topCmd;

    Top Top::global;
    
    TopOld::T TopOld::_snapshotStart = TopOld::currentTime();
    TopOld::D TopOld::_snapshotDuration;
    TopOld::UsageMap TopOld::_totalUsage;
    TopOld::UsageMap TopOld::_snapshotA;
    TopOld::UsageMap TopOld::_snapshotB;
    TopOld::UsageMap &TopOld::_snapshot = TopOld::_snapshotA;
    TopOld::UsageMap &TopOld::_nextSnapshot = TopOld::_snapshotB;
    boost::mutex TopOld::topMutex;


}

#include "../commands.h"
#include <map>
#include "../../util/concurrency/value.h"

namespace mongo {

    extern mapsf<string,string> dynHostNames;
    extern DiagStr _hostNameCached;

    /** 
      { cloud:1, nodes: {
          name : <ip>, ...
        }
      }
    */
    class CmdCloud  : public Command {
    public:
    virtual LockType locktype() const { return NONE; }
        virtual bool logTheOp() { return false; }
        virtual bool adminOnly() const { return true; } // very important
        virtual bool localHostOnlyIfNoAuth(const BSONObj&) { return true; }
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const { help << "internal"; }
        CmdCloud() : Command("cloud") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            assert(!fromRepl);
            BSONObj nodes = cmdObj["nodes"].Obj();
            map<string,string> ipmap;
            for( BSONObj::iterator i(nodes); i.more(); ) { 
                BSONElement e = i.next();
                assert( *e.fieldName() == '#' );
                ipmap[e.fieldName()] = e.String();
            }

            string me = cmdObj["me"].String();
            assert( !me.empty() && me[0] == '#' );
            
            log(/*1*/) << "CmdCloud" << endl;

            if( me != _hostNameCached.get() ) { 
                log() << "CmdCloud new 'me' value:" << me << endl;
                _hostNameCached = me;
            }

            dynHostNames.swap(ipmap);
            return true;
        }
    } cmdCloud;

}

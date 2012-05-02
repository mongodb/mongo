#include "../commands.h"
#include <map>
#include "../../util/concurrency/mapsf.h"
#include "../../util/concurrency/value.h"
#include "../../util/mongoutils/str.h"
#include "../../util/net/hostandport.h"

using namespace mongoutils;

namespace mongo {

    mapsf<string,string> dynHostNames;
    extern DiagStr& _hostNameCached;

    string dynHostMyName() {
        if( !str::startsWith(_hostNameCached, '#') )
            return "";
        return _hostNameCached; 
    }

    void dynHostResolve(string& name, int& port) {
        verify( !name.empty() );
        verify( !str::contains(name, ':') );
        verify( str::startsWith(name, '#') );
        string s = dynHostNames.get(name);
        if( s.empty() ) { 
            name.clear();
            return;
        }
        verify( !str::startsWith(s, '#') );
        HostAndPort hp(s);
        if( hp.hasPort() ) {
            port = hp.port();
            log() << "info: dynhost in:" << name << " out:" << hp.toString() << endl;
        }
        name = hp.host();
    }

    /** 
      { cloud:1, nodes: {
          name : <ip>, ...
        },
        me : <mylogicalname>
      }
    */
    class CmdCloud  : public Command {
    public:
    virtual LockType locktype() const { return NONE; }
        virtual bool logTheOp() { return false; }
        virtual bool adminOnly() const { return true; } // very important
        virtual bool localHostOnlyIfNoAuth(const BSONObj&) { return true; }
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const { 
            help << "internal\n"; 
            help << "{cloud:1,nodes:...,me:<my_logical_name>}";
        }
        CmdCloud() : Command("cloud") {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            verify(!fromRepl);
            BSONObj nodes = cmdObj["nodes"].Obj();
            map<string,string> ipmap;
            for( BSONObj::iterator i(nodes); i.more(); ) { 
                BSONElement e = i.next();
                verify( *e.fieldName() == '#' );
                ipmap[e.fieldName()] = e.String();
            }

            string me = cmdObj["me"].String();
            verify( !me.empty() && me[0] == '#' );
            
            log(/*1*/) << "CmdCloud" << endl;

            if( me != _hostNameCached.get() ) { 
                log() << "CmdCloud new 'me' value:" << me << endl;
                _hostNameCached = me;
            }

            dynHostNames.swap(ipmap);
            return true;
        }
    } cmdCloud;

    BSONObj fromjson(const string &str);

    void cloudCmdLineParamIs(string cmd) {
        string errmsg;
        BSONObjBuilder res;
        BSONObj o = fromjson(cmd);
        cmdCloud.run("", o, 0, errmsg, res, false);
    }
}

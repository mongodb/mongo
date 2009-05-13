// curop.h

#pragma once

#include "namespace.h"
#include "security.h"

namespace mongo { 

    extern struct CurOp {
        void reset(time_t now) { 
            active = true;
            opNum++;
            startTime = now;
            ns[0] = '?'; // just in case not set later
            *query = 0;
            killCurrentOp = 0;
        }

        bool active;
        unsigned opNum;
        time_t startTime;
        int op;
        char ns[Namespace::MaxNsLen+1];
        char query[128];
        char zero;

        CurOp() { 
            opNum = 0; 
            ns[sizeof(ns)-1] = 0;
            query[sizeof(query)-1] = 0;
        }

        BSONObj info() { 
            AuthenticationInfo *ai = authInfo.get();
            if( ai == 0 || !ai->isAuthorized("admin") ) { 
                BSONObjBuilder b;
                b.append("err", "unauthorized");
                return b.obj();
            }
            return infoNoauth();
        }
        
        BSONObj infoNoauth() {
            BSONObjBuilder b;
            b.append("opid", opNum);
            b.append("active", active);
            if( active ) 
                b.append("secs_running", (int) (time(0)-startTime));
            if( op == 2004 ) 
                b.append("op", "query");
            else if( op == 2005 )
                b.append("op", "getMore");
            else if( op == 2001 )
                b.append("op", "update");
            else if( op == 2002 )
                b.append("op", "insert");
            else if( op == 2006 )
                b.append("op", "delete");
            else
                b.append("op", op);
            b.append("ns", ns);
            b.append("query", query);
            b.append("inLock",  dbMutexInfo.isLocked());
            return b.obj();
        }
    } currentOp;

}

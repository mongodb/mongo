/** mongosink.cpp
 *
 * Utility to simulate a trivial server-half of MongoDB Wire Protocol.
 * Doesn't store any data at all; use to test best case performance of
 * a client application.
 */

#include "../pch.h"
#include "../util/net/message.h"
#include "../db/dbmessage.h"
#include "../util/net/message_server.h"

using namespace std;
using namespace mongo;

// don't default to 27017, don't want anyone accidentally connecting
// thinking it is a database...
const int port = 27050; 

const int CmdSentinel = 2008;

struct Count { 
    enum { Max = CmdSentinel+1 };
    AtomicUInt total;
    AtomicUInt ops[Max];
    void count(int op) { 
        if( op >= Max ) { 
            log() << op << endl;
            fassert(1,false);
        }
        ++total;
        ++ops[op];
    }
    void appendTo(BSONObjBuilder& b) { 
        BSONObjBuilder c;
        int list[] = 
        {
            opReply, 
            dbMsg,
            dbUpdate,
            dbInsert,
            dbQuery,
            dbGetMore,
            dbDelete,
            dbKillCursors,
            0
        };

        for( int j = 0; list[j]; j++ ) {
            int i = list[j];
            if( ops[i] ) { 
                c.append(opToString(i), ops[i]);
            }
        }
        c.append("command",ops[CmdSentinel]);
        c.append("total", total);
        b.append("counts", c.done());
    }
} counts;

 class MyMessageHandler : public MessageHandler {
    public:
        virtual void connected( AbstractMessagingPort* p ) {
            log() << "connect" << endl;
        }

        virtual void disconnected( AbstractMessagingPort* p ) {
            log() << "disconnect" << endl;
        }

        virtual void process( Message& m , AbstractMessagingPort* port , LastError * le) {
            while ( true ) {
                if ( inShutdown() ) {
                    log() << "got request after shutdown()" << endl;
                    break;
                }

                int op = m.operation();

                DEV {
                    static volatile int n;
                    if( ++n <= 10 ) {
                        // show first few
                        log() << "got message type:" << op << endl;
                    }
                }

                DbResponse dbresponse;
                try {
                    if( op == dbQuery ) { 
                        DbMessage d(m);
                        if( strstr(d.getns(),"$cmd") ) { 
                            counts.count(CmdSentinel);
                        }
                        else {
                            counts.count(op);
                        }
                        BSONObjBuilder b(4096);
                        b.append("info","mongosink");
                        b.append("ok",false);
                        counts.appendTo(b);
                        replyToQuery(0, m, dbresponse, b.done());
                    }
                    else { 
                        counts.count(op);
                    }
                }
                catch ( const std::exception & e ) {
                    log() << "exception " << e.what() << endl;
                    exitCleanly( EXIT_UNCAUGHT );
                }

                if ( dbresponse.response ) {
                    port->reply(m, *dbresponse.response, dbresponse.responseTo);
                    if( dbresponse.exhaustNS.size() > 0 ) {
                        log() << "error exhaustNS.size>0" << endl;
                        MsgData *header = dbresponse.response->header();
                        QueryResult *qr = (QueryResult *) header;
                        long long cursorid = qr->cursorId;
                        if( cursorid ) {
                            verify( dbresponse.exhaustNS.size() && dbresponse.exhaustNS[0] );
                            string ns = dbresponse.exhaustNS; // before reset() free's it...
                            m.reset();
                            BufBuilder b(512);
                            b.appendNum((int) 0 /*size set later in appendData()*/);
                            b.appendNum(header->id);
                            b.appendNum(header->responseTo);
                            b.appendNum((int) dbGetMore);
                            b.appendNum((int) 0);
                            b.appendStr(ns);
                            b.appendNum((int) 0); // ntoreturn
                            b.appendNum(cursorid);
                            m.appendData(b.buf(), b.len());
                            b.decouple();
                            DEV log() << "exhaust=true sending more" << endl;
                            continue; // this goes back to top loop
                        }
                    }
                }
                break;
            }
        }
 };

void go() {
    MessageServer::Options options;
    options.port = port;
    
    MessageServer * server = createServer( options , new MyMessageHandler() );
    
    server->run();
}

int main(int argc, char **argv, char** envp) {
    cout << "mongosink" << endl;
    cout << "\nWARNING"
         << "\nThis utility (mongosink) eats db requests from a client. This tool is for "
         << "\nQA and client performance testing purposes. "
         << "\n" 
         << endl;
    go();
    return 0;
}

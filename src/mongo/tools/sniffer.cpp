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
 */

/*
  TODO:
    large messages - need to track what's left and ignore
    single object over packet size - can only display beginning of object

    getmore
    delete
    killcursors

 */
#include "mongo/pch.h"

#ifdef _WIN32
#undef min
#undef max
#endif

#include <boost/shared_ptr.hpp>
#include <ctype.h>
#include <errno.h>
#include <iostream>
#include <map>
#include <pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/types.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "mongo/base/initializer.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/util/net/message.h"
#include "mongo/util/mmap.h"
#include "mongo/util/text.h"

using namespace std;
using mongo::Message;
using mongo::MsgData;
using mongo::DbMessage;
using mongo::BSONObj;
using mongo::BufBuilder;
using mongo::DBClientConnection;
using mongo::QueryResult;
using mongo::MemoryMappedFile;

mongo::CmdLine mongo::cmdLine;

#define SNAP_LEN 65535

int captureHeaderSize;
set<int> serverPorts;
string forwardAddress;
bool objcheck = false;

ostream *outPtr = &cout;
ostream &out() { return *outPtr; }

/* IP header */
struct sniff_ip {
    u_char  ip_vhl;                 /* version << 4 | header length >> 2 */
    u_char  ip_tos;                 /* type of service */
    u_short ip_len;                 /* total length */
    u_short ip_id;                  /* identification */
    u_short ip_off;                 /* fragment offset field */
#define IP_RF 0x8000            /* reserved fragment flag */
#define IP_DF 0x4000            /* don't fragment flag */
#define IP_MF 0x2000            /* more fragments flag */
#define IP_OFFMASK 0x1fff       /* mask for fragmenting bits */
    u_char  ip_ttl;                 /* time to live */
    u_char  ip_p;                   /* protocol */
    u_short ip_sum;                 /* checksum */
    struct  in_addr ip_src,ip_dst;  /* source and dest address */
};
#define IP_HL(ip)               (((ip)->ip_vhl) & 0x0f)
#define IP_V(ip)                (((ip)->ip_vhl) >> 4)

/* TCP header */
#ifdef _WIN32
typedef unsigned __int32 uint32_t;
#endif
typedef uint32_t tcp_seq;

struct sniff_tcp {
    u_short th_sport;               /* source port */
    u_short th_dport;               /* destination port */
    tcp_seq th_seq;                 /* sequence number */
    tcp_seq th_ack;                 /* acknowledgement number */
    u_char  th_offx2;               /* data offset, rsvd */
#define TH_OFF(th)      (((th)->th_offx2 & 0xf0) >> 4)
    u_char  th_flags;
#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10
#define TH_URG  0x20
#define TH_ECE  0x40
#define TH_CWR  0x80

#ifndef TH_FLAGS
#define TH_FLAGS        (TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)
#endif

    u_short th_win;                 /* window */
    u_short th_sum;                 /* checksum */
    u_short th_urp;                 /* urgent pointer */
};

#pragma pack( 1 )
struct Connection {
    struct in_addr srcAddr;
    u_short srcPort;
    struct in_addr dstAddr;
    u_short dstPort;
    bool operator<( const Connection &other ) const {
        return memcmp( this, &other, sizeof( Connection ) ) < 0;
    }
    Connection reverse() const {
        Connection c;
        c.srcAddr = dstAddr;
        c.srcPort = dstPort;
        c.dstAddr = srcAddr;
        c.dstPort = srcPort;
        return c;
    }
};
#pragma pack()

map< Connection, bool > seen;
map< Connection, int > bytesRemainingInMessage;
map< Connection, boost::shared_ptr< BufBuilder > > messageBuilder;
map< Connection, unsigned > expectedSeq;
map< Connection, boost::shared_ptr<DBClientConnection> > forwarder;
map< Connection, long long > lastCursor;
map< Connection, map< long long, long long > > mapCursor;

void processMessage( Connection& c , Message& d );

void got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet) {

    const struct sniff_ip* ip = (struct sniff_ip*)(packet + captureHeaderSize);
    int size_ip = IP_HL(ip)*4;
    if ( size_ip < 20 ) {
        cerr << "*** Invalid IP header length: " << size_ip << " bytes" << endl;
        return;
    }

    verify( ip->ip_p == IPPROTO_TCP );

    const struct sniff_tcp* tcp = (struct sniff_tcp*)(packet + captureHeaderSize + size_ip);
    int size_tcp = TH_OFF(tcp)*4;
    if (size_tcp < 20) {
        cerr << "*** Invalid TCP header length: " << size_tcp << " bytes" << endl;
        return;
    }

    if ( ! ( serverPorts.count( ntohs( tcp->th_sport ) ) ||
             serverPorts.count( ntohs( tcp->th_dport ) ) ) ) {
        return;
    }

    const u_char * payload = (const u_char*)(packet + captureHeaderSize + size_ip + size_tcp);

    unsigned totalSize = ntohs(ip->ip_len);
    verify( totalSize <= header->caplen );

    int size_payload = totalSize - (size_ip + size_tcp);
    if (size_payload <= 0 )
        return;

    Connection c;
    c.srcAddr = ip->ip_src;
    c.srcPort = tcp->th_sport;
    c.dstAddr = ip->ip_dst;
    c.dstPort = tcp->th_dport;

    if ( seen[ c ] ) {
        if ( expectedSeq[ c ] != ntohl( tcp->th_seq ) ) {
            cerr << "Warning: sequence # mismatch, there may be dropped packets" << endl;
        }
    }
    else {
        seen[ c ] = true;
    }

    expectedSeq[ c ] = ntohl( tcp->th_seq ) + size_payload;

    Message m;

    if ( bytesRemainingInMessage[ c ] == 0 ) {
        m.setData( (MsgData*)payload , false );
        if ( !m.header()->valid() ) {
            cerr << "Invalid message start, skipping packet." << endl;
            return;
        }
        if ( size_payload > m.header()->len ) {
            cerr << "Multiple messages in packet, skipping packet." << endl;
            return;
        }
        if ( size_payload < m.header()->len ) {
            bytesRemainingInMessage[ c ] = m.header()->len - size_payload;
            messageBuilder[ c ].reset( new BufBuilder() );
            messageBuilder[ c ]->appendBuf( (void*)payload, size_payload );
            return;
        }
    }
    else {
        bytesRemainingInMessage[ c ] -= size_payload;
        messageBuilder[ c ]->appendBuf( (void*)payload, size_payload );
        if ( bytesRemainingInMessage[ c ] < 0 ) {
            cerr << "Received too many bytes to complete message, resetting buffer" << endl;
            bytesRemainingInMessage[ c ] = 0;
            messageBuilder[ c ].reset();
            return;
        }
        if ( bytesRemainingInMessage[ c ] > 0 )
            return;
        m.setData( (MsgData*)messageBuilder[ c ]->buf(), true );
        messageBuilder[ c ]->decouple();
        messageBuilder[ c ].reset();
    }

    DbMessage d( m );

    out() << inet_ntoa(ip->ip_src) << ":" << ntohs( tcp->th_sport )
          << ( serverPorts.count( ntohs( tcp->th_dport ) ) ? "  -->> " : "  <<--  " )
          << inet_ntoa(ip->ip_dst) << ":" << ntohs( tcp->th_dport )
          << " " << d.getns()
          << "  " << m.header()->len << " bytes "
          << " id:" << hex << m.header()->id << dec << "\t" << m.header()->id;

    processMessage( c , m );
}

class AuditingDbMessage : public DbMessage {
public:
    AuditingDbMessage( const Message &m ) : DbMessage( m ) {}
    BSONObj nextJsObj( const char *context ) {
        BSONObj ret = DbMessage::nextJsObj();
        if ( objcheck && !ret.valid() ) {
            // TODO provide more debugging info
            cout << "invalid object in " << context << ": " << ret.hexDump() << endl;
        }
        return ret;
    }
};

void processMessage( Connection& c , Message& m ) {
    AuditingDbMessage d(m);

    if ( m.operation() == mongo::opReply )
        out() << " - " << (unsigned)m.header()->responseTo;
    out() << '\n';

    try {
        switch( m.operation() ) {
        case mongo::opReply: {
            mongo::QueryResult* r = (mongo::QueryResult*)m.singleData();
            out() << "\treply" << " n:" << r->nReturned << " cursorId: " << r->cursorId << endl;
            if ( r->nReturned ) {
                mongo::BSONObj o( r->data() );
                out() << "\t" << o << endl;
            }
            break;
        }
        case mongo::dbQuery: {
            mongo::QueryMessage q(d);
            out() << "\tquery: " << q.query << "  ntoreturn: " << q.ntoreturn << " ntoskip: " << q.ntoskip;
            if( !q.fields.isEmpty() )
                out() << " hasfields";
            if( q.queryOptions & mongo::QueryOption_SlaveOk )
                out() << " SlaveOk";
            if( q.queryOptions & mongo::QueryOption_NoCursorTimeout )
                out() << " NoCursorTimeout";
            if( q.queryOptions & ~(mongo::QueryOption_SlaveOk | mongo::QueryOption_NoCursorTimeout) )
                out() << " queryOptions:" << hex << q.queryOptions;
            out() << endl;
            break;
        }
        case mongo::dbUpdate: {
            int flags = d.pullInt();
            BSONObj q = d.nextJsObj( "update" );
            BSONObj o = d.nextJsObj( "update" );
            out() << "\tupdate  flags:" << flags << " q:" << q << " o:" << o << endl;
            break;
        }
        case mongo::dbInsert: {
            out() << "\tinsert: " << d.nextJsObj( "insert" ) << endl;
            while ( d.moreJSObjs() ) {
                out() << "\t\t" << d.nextJsObj( "insert" ) << endl;
            }
            break;
        }
        case mongo::dbGetMore: {
            int nToReturn = d.pullInt();
            long long cursorId = d.pullInt64();
            out() << "\tgetMore nToReturn: " << nToReturn << " cursorId: " << cursorId << endl;
            break;
        }
        case mongo::dbDelete: {
            int flags = d.pullInt();
            BSONObj q = d.nextJsObj( "delete" );
            out() << "\tdelete flags: " << flags << " q: " << q << endl;
            break;
        }
        case mongo::dbKillCursors: {
            int *x = (int *) m.singleData()->_data;
            x++; // reserved
            int n = *x;
            out() << "\tkillCursors n: " << n << endl;
            break;
        }
        default:
            out() << "\tunknown opcode " << m.operation() << endl;
            cerr << "*** CANNOT HANDLE TYPE: " << m.operation() << endl;
        }
    }
    catch ( ... ) {
        cerr << "Error parsing message for operation: " << m.operation() << endl;
    }


    if ( !forwardAddress.empty() ) {
        if ( m.operation() != mongo::opReply ) {
            boost::shared_ptr<DBClientConnection> conn = forwarder[ c ];
            if ( !conn ) {
                conn.reset(new DBClientConnection( true ));
                conn->connect( forwardAddress );
                forwarder[ c ] = conn;
            }
            if ( m.operation() == mongo::dbQuery || m.operation() == mongo::dbGetMore ) {
                if ( m.operation() == mongo::dbGetMore ) {
                    DbMessage d( m );
                    d.pullInt();
                    long long &cId = d.pullInt64();
                    cId = mapCursor[ c ][ cId ];
                }
                Message response;
                conn->port().call( m, response );
                QueryResult *qr = (QueryResult *) response.singleData();
                if ( !( qr->resultFlags() & mongo::ResultFlag_CursorNotFound ) ) {
                    if ( qr->cursorId != 0 ) {
                        lastCursor[ c ] = qr->cursorId;
                        return;
                    }
                }
                lastCursor[ c ] = 0;
            }
            else {
                conn->port().say( m );
            }
        }
        else {
            Connection r = c.reverse();
            long long myCursor = lastCursor[ r ];
            QueryResult *qr = (QueryResult *) m.singleData();
            long long yourCursor = qr->cursorId;
            if ( ( qr->resultFlags() & mongo::ResultFlag_CursorNotFound ) )
                yourCursor = 0;
            if ( myCursor && !yourCursor )
                cerr << "Expected valid cursor in sniffed response, found none" << endl;
            if ( !myCursor && yourCursor )
                cerr << "Sniffed valid cursor when none expected" << endl;
            if ( myCursor && yourCursor ) {
                mapCursor[ r ][ qr->cursorId ] = lastCursor[ r ];
                lastCursor[ r ] = 0;
            }
        }
    }
}

void processDiagLog( const char * file ) {
    Connection c;
    MemoryMappedFile f;
    long length;
    unsigned long long L = 0;
    char * root = (char*)f.map( file , L, MemoryMappedFile::SEQUENTIAL );
    verify( L < 0x80000000 );
    length = (long) L;
    verify( root );
    verify( length > 0 );

    char * pos = root;

    long read = 0;
    while ( read < length ) {
        Message m(pos,false);
        int len = m.header()->len;
        DbMessage d(m);
        cout << len << " " << d.getns() << endl;

        processMessage( c , m );

        read += len;
        pos += len;
    }

    f.close();
}

void usage() {
    cout <<
         "Usage: mongosniff [--help] [--forward host:port] [--source (NET <interface> | (FILE | DIAGLOG) <filename>)] [<port0> <port1> ... ]\n"
         "--forward       Forward all parsed request messages to mongod instance at \n"
         "                specified host:port\n"
         "--source        Source of traffic to sniff, either a network interface or a\n"
         "                file containing previously captured packets in pcap format,\n"
         "                or a file containing output from mongod's --diaglog option.\n"
         "                If no source is specified, mongosniff will attempt to sniff\n"
         "                from one of the machine's network interfaces.\n"
         "--objcheck      Log hex representation of invalid BSON objects and nothing\n"
         "                else.  Spurious messages about invalid objects may result\n"
         "                when there are dropped tcp packets.\n"
         "<port0>...      These parameters are used to filter sniffing.  By default, \n"
         "                only port 27017 is sniffed.\n"
         "--help          Print this help message.\n"
         << endl;
}

int toolMain(int argc, char **argv, char** envp) {
    mongo::runGlobalInitializersOrDie(argc, argv, envp);

    stringstream nullStream;
    nullStream.clear(ios::failbit);

    const char *dev = NULL;
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *handle;

    struct bpf_program fp;

// PCAP_NETMASK_UNKNOWN was introduced in pcap 1.1.0, so work around earlier versions. See
// http://anonsvn.wireshark.org/viewvc?revision=33461&view=revision for details.
#if defined(PCAP_NETMASK_UNKNOWN)
    bpf_u_int32 mask = PCAP_NETMASK_UNKNOWN;
#else
    bpf_u_int32 mask = 0;
#endif

    bool source = false;
    bool replay = false;
    bool diaglog = false;
    const char *file = 0;

    vector< const char * > args;
    for( int i = 1; i < argc; ++i )
        args.push_back( argv[ i ] );

    try {
        for( unsigned i = 0; i < args.size(); ++i ) {
            const char *arg = args[ i ];
            if ( arg == string( "--help" ) ) {
                usage();
                return 0;
            }
            else if ( arg == string( "--forward" ) ) {
                forwardAddress = args[ ++i ];
            }
            else if ( arg == string( "--source" ) ) {
                uassert( 10266 ,  "can't use --source twice" , source == false );
                uassert( 10267 ,  "source needs more args" , args.size() > i + 2);
                source = true;
                replay = ( args[ ++i ] == string( "FILE" ) );
                diaglog = ( args[ i ] == string( "DIAGLOG" ) );
                if ( replay || diaglog )
                    file = args[ ++i ];
                else
                    dev = args[ ++i ];
            }
            else if ( arg == string( "--objcheck" ) ) {
                objcheck = true;
                outPtr = &nullStream;
            }
            else {
                serverPorts.insert( atoi( args[ i ] ) );
            }
        }
    }
    catch ( ... ) {
        usage();
        return -1;
    }

    if ( !serverPorts.size() )
        serverPorts.insert( 27017 );

    if ( diaglog ) {
        processDiagLog( file );
        return 0;
    }
    else if ( replay ) {
        handle = pcap_open_offline(file, errbuf);
        if ( ! handle ) {
            cerr << "error opening capture file!" << endl;
            return -1;
        }
    }
    else {
        if ( !dev ) {
            dev = pcap_lookupdev(errbuf);
            if ( ! dev ) {
                cerr << "error finding device: " << errbuf << endl;
                return -1;
            }
            cout << "found device: " << dev << endl;
        }
        bpf_u_int32 net;
        if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1) {
            cerr << "can't get netmask: " << errbuf << endl;
            return -1;
        }
        handle = pcap_open_live(dev, SNAP_LEN, 1, 1000, errbuf);
        if ( ! handle ) {
            cerr << "error opening device: " << errbuf << endl;
            return -1;
        }
    }

    switch ( pcap_datalink( handle ) ) {
    case DLT_EN10MB:
        captureHeaderSize = 14;
        break;
    case DLT_NULL:
        captureHeaderSize = 4;
        break;
    default:
        cerr << "don't know how to handle datalink type: " << pcap_datalink( handle ) << endl;
    }

    verify( pcap_compile(handle, &fp, const_cast< char * >( "tcp" ) , 0, mask) != -1 );
    verify( pcap_setfilter(handle, &fp) != -1 );

    cout << "sniffing... ";
    for ( set<int>::iterator i = serverPorts.begin(); i != serverPorts.end(); i++ )
        cout << *i << " ";
    cout << endl;

    pcap_loop(handle, 0 , got_packet, NULL);

    pcap_freecode(&fp);
    pcap_close(handle);

    return 0;
}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables toolMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = toolMain(argc, wcl.argv(), wcl.envp());
    ::_exit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = toolMain(argc, argv, envp);
    ::_exit(exitCode);
}
#endif

// sniffer.cpp

/*
  TODO:
    large messages - need to track what's left and ingore
    single object over packet size - can only display begging of object

    getmore
    delete
    killcursors

 */

#include <pcap.h>

#ifdef _WIN32
#undef min
#undef max
#endif

#include "../util/builder.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/dbclient.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <iostream>
#include <map>
#include <string>

#include <boost/shared_ptr.hpp>

using namespace std;
using mongo::asserted;
using mongo::Message;
using mongo::MsgData;
using mongo::DbMessage;
using mongo::BSONObj;
using mongo::BufBuilder;
using mongo::DBClientConnection;
using mongo::QueryResult;

#define SNAP_LEN 65535

int captureHeaderSize;
set<int> serverPorts;
string forwardAddress;

/* IP header */
struct sniff_ip {
    u_char  ip_vhl;                 /* version << 4 | header length >> 2 */
    u_char  ip_tos;                 /* type of service */
    u_short ip_len;                 /* total length */
    u_short ip_id;                  /* identification */
    u_short ip_off;                 /* fragment offset field */
#define IP_RF 0x8000            /* reserved fragment flag */
#define IP_DF 0x4000            /* dont fragment flag */
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
typedef u_int32_t tcp_seq;

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
map< Connection, DBClientConnection* > forwarder;
map< Connection, long long > lastCursor;
map< Connection, map< long long, long long > > mapCursor;

void got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet){

    const struct sniff_ip* ip = (struct sniff_ip*)(packet + captureHeaderSize);
    int size_ip = IP_HL(ip)*4;
    if ( size_ip < 20 ){
        cerr << "*** Invalid IP header length: " << size_ip << " bytes" << endl;
        return;
    }

    assert( ip->ip_p == IPPROTO_TCP );

    const struct sniff_tcp* tcp = (struct sniff_tcp*)(packet + captureHeaderSize + size_ip);
    int size_tcp = TH_OFF(tcp)*4;
    if (size_tcp < 20){
        cerr << "*** Invalid TCP header length: " << size_tcp << " bytes" << endl;
        return;
    }

    if ( ! ( serverPorts.count( ntohs( tcp->th_sport ) ) ||
             serverPorts.count( ntohs( tcp->th_dport ) ) ) ){
        return;
    }

    const u_char * payload = (const u_char*)(packet + captureHeaderSize + size_ip + size_tcp);

    unsigned totalSize = ntohs(ip->ip_len);
    assert( totalSize <= header->caplen );

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
    } else {
        seen[ c ] = true;
    }

    expectedSeq[ c ] = ntohl( tcp->th_seq ) + size_payload;

    Message m;

    if ( bytesRemainingInMessage[ c ] == 0 ) {
        m.setData( (MsgData*)payload , false );
        if ( !m.data->valid() ) {
            cerr << "Invalid message start, skipping packet." << endl;
            return;
        }
        if ( size_payload > m.data->len ) {
            cerr << "Multiple messages in packet, skipping packet." << endl;
            return;
        }
        if ( size_payload < m.data->len ) {
            bytesRemainingInMessage[ c ] = m.data->len - size_payload;
            messageBuilder[ c ].reset( new BufBuilder() );
            messageBuilder[ c ]->append( (void*)payload, size_payload );
            return;
        }
    } else {
        bytesRemainingInMessage[ c ] -= size_payload;
        messageBuilder[ c ]->append( (void*)payload, size_payload );
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

    cout << inet_ntoa(ip->ip_src) << ":" << ntohs( tcp->th_sport )
         << ( serverPorts.count( ntohs( tcp->th_dport ) ) ? "  -->> " : "  <<--  " )
         << inet_ntoa(ip->ip_dst) << ":" << ntohs( tcp->th_dport )
         << " " << d.getns()
         << "  " << m.data->len << " bytes "
         << " id:" << hex << m.data->id << dec << "\t" << m.data->id;

    if ( m.data->operation() == mongo::opReply )
        cout << " - " << m.data->responseTo;
    cout << endl;

    switch( m.data->operation() ){
    case mongo::opReply:{
        mongo::QueryResult* r = (mongo::QueryResult*)m.data;
        cout << "\treply" << " n:" << r->nReturned << " cursorId: " << r->cursorId << endl;
        if ( r->nReturned ){
            mongo::BSONObj o( r->data() , 0 );
            cout << "\t" << o << endl;
        }
        break;
    }
    case mongo::dbQuery:{
        mongo::QueryMessage q(d);
        cout << "\tquery: " << q.query << "  ntoreturn: " << q.ntoreturn << " ntoskip: " << q.ntoskip << endl;
        break;
    }
    case mongo::dbUpdate:{
        int flags = d.pullInt();
        BSONObj q = d.nextJsObj();
        BSONObj o = d.nextJsObj();
        cout << "\tupdate  flags:" << flags << " q:" << q << " o:" << o << endl;
        break;
    }
    case mongo::dbInsert:{
        cout << "\tinsert: " << d.nextJsObj() << endl;
        while ( d.moreJSObjs() )
            cout << "\t\t" << d.nextJsObj() << endl;
        break;
    }
    case mongo::dbGetMore:{
        int nToReturn = d.pullInt();
        long long cursorId = d.pullInt64();
        cout << "\tgetMore nToReturn: " << nToReturn << " cursorId: " << cursorId << endl;
        break;
    }
    case mongo::dbDelete:{
        int flags = d.pullInt();
        BSONObj q = d.nextJsObj();
        cout << "\tdelete flags: " << flags << " q: " << q << endl;
        break;
    }
    case mongo::dbKillCursors:{
        int *x = (int *) m.data->_data;
        x++; // reserved
        int n = *x;
        cout << "\tkillCursors n: " << n << endl;
        break;
    }
    default:
        cerr << "*** CANNOT HANDLE TYPE: " << m.data->operation() << endl;
    }

    if ( !forwardAddress.empty() ) {
        if ( m.data->operation() != mongo::opReply ) {
            DBClientConnection *conn = forwarder[ c ];
            if ( !conn ) {
                // These won't get freed on error, oh well hopefully we'll just
                // abort in that case anyway.
                conn = new DBClientConnection( true );
                conn->connect( forwardAddress );
                forwarder[ c ] = conn;
            }
            if ( m.data->operation() == mongo::dbQuery || m.data->operation() == mongo::dbGetMore ) {
                if ( m.data->operation() == mongo::dbGetMore ) {
                    DbMessage d( m );
                    d.pullInt();
                    long long &cId = d.pullInt64();
                    cId = mapCursor[ c ][ cId ];
                }
                Message response;
                conn->port().call( m, response );
                QueryResult *qr = (QueryResult *) response.data;
                if ( !( qr->resultFlags() & QueryResult::ResultFlag_CursorNotFound ) ) {
                    if ( qr->cursorId != 0 ) {
                        lastCursor[ c ] = qr->cursorId;
                        return;
                    }
                }
                lastCursor[ c ] = 0;
            } else {
                conn->port().say( m );
            }
        } else {
            Connection r = c.reverse();
            long long myCursor = lastCursor[ r ];
            QueryResult *qr = (QueryResult *) m.data;
            long long yourCursor = qr->cursorId;
            if ( ( qr->resultFlags() & QueryResult::ResultFlag_CursorNotFound ) )
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

void usage() {
    cout <<
    "Usage: mongosniff [--help] [--forward host:port] [--source (NET <interface> | FILE <filename>)] [<port0> <port1> ... ]\n"
    "--forward       Forward all parsed request messages to mongod instance at \n"
    "                specified host:port\n"
    "--source        Source of traffic to sniff, either a network interface or a\n"
    "                file containing perviously captured packets, in pcap format.\n"
    "                If no source is specified, mongosniff will attempt to sniff\n"
    "                from one of the machine's network interfaces.\n"
    "<port0>...      These parameters are used to filter sniffing.  By default, \n"
    "                only port 27017 is sniffed.\n"
    "--help          Print this help message.\n"
    << endl;
}

int main(int argc, char **argv){

    const char *dev = NULL;
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *handle;

    struct bpf_program fp;
    bpf_u_int32 mask;
    bpf_u_int32 net;

    bool source = false;
    bool replay = false;
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
            } else if ( arg == string( "--forward" ) ) {
                forwardAddress = args[ ++i ];
            } else if ( arg == string( "--source" ) ) {
                assert( source == false );
                assert(args.size() > i + 2);
                source = true;
                replay = ( args[ ++i ] == string( "FILE" ) );
                if ( replay )
                    file = args[ ++i ];
                else
                    dev = args[ ++i ];
            } else {
                serverPorts.insert( atoi( args[ i ] ) );
            }
        }
    } catch ( ... ) {
        usage();
        return -1;
    }

    if ( !serverPorts.size() )
        serverPorts.insert( 27017 );

    if ( !replay ) {
        if ( !dev ) {
            dev = pcap_lookupdev(errbuf);
            if ( ! dev ) {
                cerr << "error finding device: " << errbuf << endl;
                return -1;
            }
            cout << "found device: " << dev << endl;
        }
        if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1){
            cerr << "can't get netmask: " << errbuf << endl;
            return -1;
        }
        handle = pcap_open_live(dev, SNAP_LEN, 1, 1000, errbuf);
        if ( ! handle ){
            cerr << "error opening device: " << errbuf << endl;
            return -1;
        }
    } else {
        handle = pcap_open_offline(file, errbuf);
        if ( ! handle ){
            cerr << "error opening capture file!" << endl;
            return -1;
        }
    }

    switch ( pcap_datalink( handle ) ){
    case DLT_EN10MB:
        captureHeaderSize = 14;
        break;
    case DLT_NULL:
        captureHeaderSize = 4;
        break;
    default:
        cerr << "don't know how to handle datalink type: " << pcap_datalink( handle ) << endl;
    }

    assert( pcap_compile(handle, &fp, const_cast< char * >( "tcp" ) , 0, net) != -1 );
    assert( pcap_setfilter(handle, &fp) != -1 );

    cout << "sniffing... ";
    for ( set<int>::iterator i = serverPorts.begin(); i != serverPorts.end(); i++ )
        cout << *i << " ";
    cout << endl;

    pcap_loop(handle, 0 , got_packet, NULL);

    pcap_freecode(&fp);
    pcap_close(handle);

    for( map< Connection, DBClientConnection* >::iterator i = forwarder.begin(); i != forwarder.end(); ++i )
        free( i->second );

    return 0;
}


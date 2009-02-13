// sniffer.cpp

/*
  TODO:
    large messages - need to track what's left and ingore
    single object over packet size - can only display begging of object
    
    getmore
    delete
    killcursors

 */

#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/dbclient.h"

#include <pcap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <iostream>
#include <string>

using namespace std;
using mongo::asserted;
using mongo::Message;
using mongo::DbMessage;
using mongo::BSONObj;

#define SNAP_LEN 1518

int captureHeaderSize;
set<int> serverPorts;

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
typedef u_int tcp_seq;

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
#define TH_FLAGS        (TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)
    u_short th_win;                 /* window */
    u_short th_sum;                 /* checksum */
    u_short th_urp;                 /* urgent pointer */
};


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
	
	int size_payload = ntohs(ip->ip_len) - (size_ip + size_tcp);
	if (size_payload <= 0 )
        return;


    Message m( (void*)payload , 0 );
    DbMessage d( m );
    
    cout << inet_ntoa(ip->ip_src) << ":" << ntohs( tcp->th_sport ) 
         << ( serverPorts.count( ntohs( tcp->th_dport ) ) ? "  -->> " : "  <<--  " )
         << inet_ntoa(ip->ip_dst) << ":" << ntohs( tcp->th_dport ) 
         << " " << d.getns() 
         << "  " << m.data->len << " bytes "
         << m.data->id;
    
    if ( m.data->operation() == mongo::opReply )
        cout << " - " << m.data->responseTo;
    cout << endl;

    switch( m.data->operation() ){
    case mongo::opReply:{
        mongo::QueryResult* r = (mongo::QueryResult*)m.data;
        cout << "\treply" << " n:" << r->nReturned << endl;
        if ( r->nReturned ){
            mongo::BSONObj o( r->data() , 0 );
            cout << "\t" << o << endl;
        }
        break;
    }
    case mongo::dbQuery:{
        mongo::QueryMessage q(d);
        cout << "\tquery: " << q.query << endl;
        break;
    }
    case mongo::dbUpdate:{
        int flags = d.pullInt();
        BSONObj q = d.nextJsObj();
        BSONObj o = d.nextJsObj();
        cout << "\tupdate  flags:" << flags << " q:" << q  << " o:" << o << endl;
        break;
    }
    case mongo::dbInsert:{
        cout << "\tinsert: " << d.nextJsObj() << endl;
        while ( d.moreJSObjs() )
            cout << "\t\t" << d.nextJsObj() << endl;
        break;
    }
    default:
        cout << "*** CANNOT HANDLE TYPE: " << m.data->operation() << endl;
    }    
    
}

int main(int argc, char **argv){

	char *dev = NULL;
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_t *handle;

	struct bpf_program fp;
	bpf_u_int32 mask;
	bpf_u_int32 net;
    
	if (argc >= 2){
		dev = argv[1];
	}
	else {
		dev = pcap_lookupdev(errbuf);
		if ( ! dev ){
            cerr << "error finding device" << endl;
            return -1;
		}
	}
    
    for ( int i=2; i<argc; i++ )
        serverPorts.insert( atoi( argv[i] ) );
    if ( ! serverPorts.size() )
        serverPorts.insert( 27017 );

	if (pcap_lookupnet(dev, &net, &mask, errbuf) == -1){
        cerr << "can't get netmask!" << endl;
        return -1;
	}
    
	handle = pcap_open_live(dev, SNAP_LEN, 1, 1000, errbuf);
	if ( ! handle ){
        cerr << "error opening device!" << endl;
        return -1;
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

    return 0;
}


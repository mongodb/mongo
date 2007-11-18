// sock.h

#pragma once 

#include <stdio.h>
#include <sstream>
#include "goodies.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
inline int getLastError() { return WSAGetLastError(); }
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
inline void closesocket(int s) { close(s); }
const int INVALID_SOCKET = -1;
typedef int SOCKET;
#define h_errno errno
inline int getLastError() { return errno; }
#endif

struct SockAddr {
	SockAddr() { addressSize = sizeof(sockaddr_in); }
	SockAddr(int sourcePort); /* source side */ 
	SockAddr(const char *ip, int port); /* EndPoint (remote) side, or if you want to specify which interface locally */

	struct sockaddr_in sa;
	socklen_t addressSize;

	string toString() { 
		stringstream out;
		out << inet_ntoa(sa.sin_addr) << ':' 
			<< sa.sin_port;
			return out.str();
	}

	unsigned getPort() { return sa.sin_port; }

	bool operator==(const SockAddr& r) const { 
		return sa.sin_addr.s_addr == r.sa.sin_addr.s_addr &&
			sa.sin_port == r.sa.sin_port;
	}
	bool operator!=(const SockAddr& r) const { return !(*this == r); }
	bool operator<(const SockAddr& r) const { 
		if( sa.sin_port >= r.sa.sin_port )
			return false;
		return sa.sin_addr.s_addr < r.sa.sin_addr.s_addr;
	}
};

class UDPConnection {
public:
	UDPConnection() { sock = 0; }
	~UDPConnection() { if( sock ) { closesocket(sock); sock = 0; } }
	bool init(const SockAddr& myAddr);
	int recvfrom(char *buf, int len, SockAddr& sender);
	int sendto(char *buf, int len, const SockAddr& EndPoint);
	int mtu() { return 1480; }

	SOCKET sock;
};

inline int UDPConnection::recvfrom(char *buf, int len, SockAddr& sender) {
	return ::recvfrom(sock, buf, len, 0, (sockaddr *) &sender.sa, &sender.addressSize);
}

inline int UDPConnection::sendto(char *buf, int len, const SockAddr& EndPoint) {
	if( 0 && rand() < (RAND_MAX>>4) ) { 
		cout << " NOTSENT ";
		//		cout << curTimeMillis() << " .TEST: NOT SENDING PACKET" << endl;
		return 0;
	}
	return ::sendto(sock, buf, len, 0, (sockaddr *) &EndPoint.sa, EndPoint.addressSize);
}

inline bool UDPConnection::init(const SockAddr& myAddr) {
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if( sock == INVALID_SOCKET ) {
		cout << "invalid socket? " << errno << endl;
		return false;
	}
	//cout << sizeof(sockaddr_in) << ' ' << myAddr.addressSize << endl;
	if( bind(sock, (sockaddr *) &myAddr.sa, myAddr.addressSize) != 0 ) { 
		cout << "udp init failed" << endl;
		closesocket(sock);
		sock = 0;
		return false;
	}
	socklen_t optLen;
	int rcvbuf;
	if (getsockopt(sock,
		SOL_SOCKET, 
		SO_RCVBUF, 
		(char*)&rcvbuf, 
		&optLen) != -1)
		cout << "SO_RCVBUF:" << rcvbuf << endl;
	return true;
}

inline SockAddr::SockAddr(int sourcePort) {
	memset(sa.sin_zero, 0, sizeof(sa.sin_zero));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(sourcePort);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	addressSize = sizeof(sa);
}

inline SockAddr::SockAddr(const char *ip, int port) {
	memset(sa.sin_zero, 0, sizeof(sa.sin_zero));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = inet_addr(ip);
	addressSize = sizeof(sa);
}

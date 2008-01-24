// sock.cpp

#include "stdafx.h"
#include "sock.h"

void sendtest() { 
	cout << "sendtest\n"; 
	SockAddr me(27016);
	SockAddr dest("127.0.0.1", 27015);
	UDPConnection c;
	if( c.init(me) ) {
		char buf[256];
		cout << "sendto: ";
		cout << c.sendto(buf, sizeof(buf), dest) << " errno:" << h_errno << endl;
	}
	cout << "end\n";
}

void listentest() { 
	cout << "listentest\n"; 
	SockAddr me(27015);
	SockAddr sender;
	UDPConnection c;
	if( c.init(me) ) {
		char buf[256];
		cout << "recvfrom: ";
		cout << c.recvfrom(buf, sizeof(buf), sender) << " errno:" << h_errno << endl;
	}
	cout << "end listentest\n";
}

void xmain();
struct SockStartupTests {
	SockStartupTests() {
#if defined(_WIN32)
        WSADATA d;
		if( WSAStartup(MAKEWORD(2,2), &d) != 0 ) {
			cout << "ERROR: wsastartup failed " << errno << endl;
			exit(1);
		}
#endif
		//cout << "ntohl:" << ntohl(256) << endl;
		//sendtest();
		//listentest();
	}
} sstests;

#if 0
void smain() {

  WSADATA wsaData;
  SOCKET RecvSocket;
  sockaddr_in RecvAddr;
  int Port = 27015;
  char RecvBuf[1024];
  int  BufLen = 1024;
  sockaddr_in SenderAddr;
  int SenderAddrSize = sizeof(SenderAddr);

  //-----------------------------------------------
  // Initialize Winsock
  WSAStartup(MAKEWORD(2,2), &wsaData);

  //-----------------------------------------------
  // Create a receiver socket to receive datagrams
  RecvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  prebindOptions( RecvSocket );

  //-----------------------------------------------
  // Bind the socket to any address and the specified port.
  RecvAddr.sin_family = AF_INET;
  RecvAddr.sin_port = htons(Port);
  RecvAddr.sin_addr.s_addr = htonl(INADDR_ANY);

  bind(RecvSocket, (SOCKADDR *) &RecvAddr, sizeof(RecvAddr));

  //-----------------------------------------------
  // Call the recvfrom function to receive datagrams
  // on the bound socket.
  printf("Receiving datagrams...\n");
  recvfrom(RecvSocket, 
    RecvBuf, 
    BufLen, 
    0, 
    (SOCKADDR *)&SenderAddr, 
    &SenderAddrSize);

  //-----------------------------------------------
  // Close the socket when finished receiving datagrams
  printf("Finished receiving. Closing socket.\n");
  closesocket(RecvSocket);

  //-----------------------------------------------
  // Clean up and exit.
  printf("Exiting.\n");
  WSACleanup();
  return;
}




void xmain() {

  WSADATA wsaData;
  SOCKET RecvSocket;
  sockaddr_in RecvAddr;
  int Port = 27015;
  char RecvBuf[1024];
  int  BufLen = 1024;
  sockaddr_in SenderAddr;
  int SenderAddrSize = sizeof(SenderAddr);

  //-----------------------------------------------
  // Initialize Winsock
  WSAStartup(MAKEWORD(2,2), &wsaData);

  //-----------------------------------------------
  // Create a receiver socket to receive datagrams

  RecvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  prebindOptions( RecvSocket );

  //-----------------------------------------------
  // Bind the socket to any address and the specified port.
  RecvAddr.sin_family = AF_INET;
  RecvAddr.sin_port = htons(Port);
  RecvAddr.sin_addr.s_addr = htonl(INADDR_ANY);

  SockAddr a(Port);
  bind(RecvSocket, (SOCKADDR *) &a.sa, a.addressSize);
//  bind(RecvSocket, (SOCKADDR *) &RecvAddr, sizeof(RecvAddr));

  SockAddr b;

  //-----------------------------------------------
  // Call the recvfrom function to receive datagrams
  // on the bound socket.
  printf("Receiving datagrams...\n");
  recvfrom(RecvSocket, 
    RecvBuf, 
    BufLen, 
    0,
	(SOCKADDR *) &b.sa, &b.addressSize);
//    (SOCKADDR *)&SenderAddr, 
//    &SenderAddrSize);

  //-----------------------------------------------
  // Close the socket when finished receiving datagrams
  printf("Finished receiving. Closing socket.\n");
  closesocket(RecvSocket);

  //-----------------------------------------------
  // Clean up and exit.
  printf("Exiting.\n");
  WSACleanup();
  return;
}

#endif

// sock.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "stdafx.h"
#include "sock.h"

namespace mongo {

    static boost::mutex sock_mutex;

    string hostbyname(const char *hostname) {
        static string unknown = "0.0.0.0";
        if ( unknown == hostname )
            return unknown;

        boostlock lk(sock_mutex);
#if defined(_WIN32)
        if( inet_addr(hostname) != INADDR_NONE )
            return hostname;
#else
        struct in_addr temp;
        if ( inet_aton( hostname, &temp ) )
            return hostname;
#endif
        struct hostent *h;
        h = gethostbyname(hostname);
        if ( h == 0 ) return "";
        return inet_ntoa( *((struct in_addr *)(h->h_addr)) );
    }

    void sendtest() {
        out() << "sendtest\n";
        SockAddr me(27016);
        SockAddr dest("127.0.0.1", 27015);
        UDPConnection c;
        if ( c.init(me) ) {
            char buf[256];
            out() << "sendto: ";
            out() << c.sendto(buf, sizeof(buf), dest) << " errno:" << h_errno << endl;
        }
        out() << "end\n";
    }

    void listentest() {
        out() << "listentest\n";
        SockAddr me(27015);
        SockAddr sender;
        UDPConnection c;
        if ( c.init(me) ) {
            char buf[256];
            out() << "recvfrom: ";
            out() << c.recvfrom(buf, sizeof(buf), sender) << " errno:" << h_errno << endl;
        }
        out() << "end listentest\n";
    }

    void xmain();
    struct SockStartupTests {
        SockStartupTests() {
#if defined(_WIN32)
            WSADATA d;
            if ( WSAStartup(MAKEWORD(2,2), &d) != 0 ) {
                out() << "ERROR: wsastartup failed " << errno << endl;
                problem() << "ERROR: wsastartup failed " << errno << endl;
                dbexit( EXIT_NTSERVICE_ERROR );
            }
#endif
            //out() << "ntohl:" << ntohl(256) << endl;
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

        ::bind(RecvSocket, (SOCKADDR *) &RecvAddr, sizeof(RecvAddr));

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
        ::bind(RecvSocket, (SOCKADDR *) &a.sa, a.addressSize);
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



} // namespace mongo

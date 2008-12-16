// miniwebserver.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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


#include "stdafx.h"
#include "miniwebserver.h"

MiniWebServer::MiniWebServer() { 
    sock = 0;
}

bool MiniWebServer::init(int port) {
    SockAddr me(port);
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if( sock == INVALID_SOCKET ) {
		log() << "ERROR: MiniWebServer listen(): invalid socket? " << errno << endl;
		return false;
	}
	prebindOptions( sock );
    if( ::bind(sock, (sockaddr *) &me.sa, me.addressSize) != 0 ) { 
        log() << "MiniWebServer: bind() failed port:" << port << " errno:" << errno << endl;
		if( errno == 98 )
			log() << "98 == addr already in use" << endl;
		closesocket(sock);
		return false;
	}

	if( ::listen(sock, 16) != 0 ) { 
		log() << "MiniWebServer: listen() failed " << errno << endl;
		closesocket(sock);
		return false;
	}

    return true; 
}

void MiniWebServer::accepted(int s) {
    char buf[4096];
    int x = ::recv(s, buf, sizeof(buf)-1, 0);
    if( x <= 0 )
        return;
    buf[x] = 0;

    string responseMsg;
    int responseCode = 599;
    vector<string> headers;
    doRequest(buf, "not implemented yet", responseMsg, responseCode, headers);

    stringstream ss;
    ss << "HTTP/1.0 " << responseCode;
    if( responseCode == 200 ) ss << " OK";
    ss << "\r\n";
    if( headers.empty() ) {
        ss << "Content-Type: text/html\r\n";
    }
    else {
        for( vector<string>::iterator i = headers.begin(); i != headers.end(); i++ )
            ss << *i << "\r\n";
    }
    ss << "\r\n";
    ss << responseMsg;
    string response = ss.str();

    ::send(s, response.c_str(), response.size(), 0);
}

void MiniWebServer::run() {
	SockAddr from;
	while( 1 ) { 
		int s = accept(sock, (sockaddr *) &from.sa, &from.addressSize);
		if( s < 0 ) {
			log() << "MiniWebServer: accept() returns " << s << " errno:" << errno << endl;
            sleepmillis(200);
			continue;
		}
		disableNagle(s);
        RARELY log() << "MiniWebServer: connection accepted from " << from.toString() << endl;
		accepted( s );
        closesocket(s);
	}
}

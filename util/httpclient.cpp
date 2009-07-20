// httpclient.cpp

#include "httpclient.h"

namespace mongo {

    int HttpClient::get( string url , map<string,string>& headers, stringstream& data ){
        uassert( "invalid url" , url.find( "http://" ) == 0 );
        url = url.substr( 7 );
        
        string host , path;
        if ( url.find( "/" ) == string::npos ){
            host = url;
            path = "/";
        }
        else {
            host = url.substr( 0 , url.find( "/" ) );
            path = url.substr( url.find( "/" ) );
        }

        int port = 80;
        uassert( "non standard port not supported yet" , host.find( ":" ) == string::npos );
        
        cout << "host [" << host << "]" << endl;
        cout << "path [" << path << "]" << endl;
        cout << "port: " << port << endl;
        
        string req;
        {
            stringstream ss;
            ss << "GET " << path << " HTTP/1.1\r\n";
            ss << "Host: " << host << "\r\n";
            ss << "Connection: Close\r\n";
            ss << "User-Agent: mongodb http client\r\n";
            ss << "\r\n";

            req = ss.str();
        }

        cout << req << endl;

        return -1;
    }
    
}

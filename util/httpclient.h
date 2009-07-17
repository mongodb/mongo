// httpclient.h

#pragma once

#include "../stdafx.h"

namespace mongo {
    
    class HttpClient {
    public:
        int get( string url , map<string,string>& headers, stringstream& data );
    };
}


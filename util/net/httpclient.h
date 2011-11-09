// httpclient.h

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

#pragma once

#include "../../pch.h"
#include "sock.h"

namespace mongo {

    class HttpClient : boost::noncopyable {
    public:

        typedef map<string,string> Headers;

        class Result {
        public:
            Result() {}

            const string& getEntireResponse() const {
                return _entireResponse;
            }

            Headers getHeaders() const {
                return _headers;
            }

            const string& getBody() const {
                return _body;
            }

        private:

            void _init( int code , string entire );

            int _code;
            string _entireResponse;

            Headers _headers;
            string _body;

            friend class HttpClient;
        };

        /**
         * @return response code
         */
        int get( string url , Result * result = 0 );

        /**
         * @return response code
         */
        int post( string url , string body , Result * result = 0 );

    private:
        int _go( const char * command , string url , const char * body , Result * result );

#ifdef MONGO_SSL
        void _checkSSLManager();

        scoped_ptr<SSLManager> _sslManager;
#endif
    };
}

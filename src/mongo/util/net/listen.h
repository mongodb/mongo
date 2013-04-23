// listen.h

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

#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/net/sock.h"

// For Mach monotonic counters (need for return type of helper function)
#if defined(__MACH__)
    #include <mach/mach_time.h>
#endif

namespace mongo {

    const int DEFAULT_MAX_CONN = 20000;
    const int MAX_MAX_CONN = 20000;

    class MessagingPort;

    class Listener : boost::noncopyable {
    public:

        Listener(const string& name, const string &ip, int port, bool logConnect=true );

        virtual ~Listener();
        
        void initAndListen(); // never returns unless error (start a thread)

        /* spawn a thread, etc., then return */
        virtual void accepted(boost::shared_ptr<Socket> psocket, long long connectionId );
        virtual void acceptedMP(MessagingPort *mp);

        const int _port;

        /**
         * @return elapsed time since the server started, in milliseconds
         */
        long long getMyElapsedTimeMillis() const { return getMillis() - startMillis; }

        void setAsTimeTracker() {
            _timeTracker = this;
        }

        static const Listener* getTimeTracker() {
            return _timeTracker;
        }

        static long long getElapsedTimeMillis() {
            if ( _timeTracker )
                return _timeTracker->getMyElapsedTimeMillis();

            // should this assert or throw?  seems like callers may not expect to get zero back, certainly not forever.
            return 0;
        }

        /**
         * @start the elapsed time counter
         */
        void startElapsedTimeCounter() { startMillis = getMillis(); }

    private:
        string _name;
        string _ip;
        bool _logConnect;
        long long startMillis;
        
#ifdef MONGO_SSL
        SSLManager* _ssl;
#endif
        
        /**
         * @return true iff everything went ok
         */
        bool _setupSockets( const vector<SockAddr>& mine , vector<SOCKET>& socks );
        
        void _logListen( int port , bool ssl );

        static const Listener* _timeTracker;
        
        virtual bool useUnixSockets() const { return false; }

        #if defined(_WIN32)
        static long long getWinPerformanceFrequency();
        static void winPerformanceCounterToMillis(long long *t);
        #endif

        #if defined(__MACH__)
        static mach_timebase_info_data_t getMachTimeInfo();
        static void machAbsToMillis(long long *t);
        #endif

        static long long getMillis();

    public:
        /** the "next" connection number.  every connection to this process has a unique number */
        static AtomicInt64 globalConnectionNumber;

        /** keeps track of how many allowed connections there are and how many are being used*/
        static TicketHolder globalTicketHolder;

        /** makes sure user input is sane */
        static void checkTicketNumbers();
    };

    class ListeningSockets {
    public:
        ListeningSockets()
            : _mutex("ListeningSockets")
            , _sockets( new set<int>() )
            , _socketPaths( new set<string>() )
        { }
        void add( int sock ) {
            scoped_lock lk( _mutex );
            _sockets->insert( sock );
        }
        void addPath( const std::string& path ) {
            scoped_lock lk( _mutex );
            _socketPaths->insert( path );
        }
        void remove( int sock ) {
            scoped_lock lk( _mutex );
            _sockets->erase( sock );
        }
        void closeAll() {
            set<int>* sockets;
            set<string>* paths;

            {
                scoped_lock lk( _mutex );
                sockets = _sockets;
                _sockets = new set<int>();
                paths = _socketPaths;
                _socketPaths = new set<string>();
            }

            for ( set<int>::iterator i=sockets->begin(); i!=sockets->end(); i++ ) {
                int sock = *i;
                log() << "closing listening socket: " << sock << endl;
                closesocket( sock );
            }

            for ( set<string>::iterator i=paths->begin(); i!=paths->end(); i++ ) {
                string path = *i;
                log() << "removing socket file: " << path << endl;
                ::remove( path.c_str() );
            }
        }
        static ListeningSockets* get();
    private:
        mongo::mutex _mutex;
        set<int>* _sockets;
        set<string>* _socketPaths; // for unix domain sockets
        static ListeningSockets* _instance;
    };

}

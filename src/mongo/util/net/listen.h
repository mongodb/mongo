// listen.h

/*    Copyright 2009 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <set>
#include <string>
#include <vector>

#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

namespace mongo {

    const int DEFAULT_MAX_CONN = 1000000;

    class MessagingPort;

    class Listener : boost::noncopyable {
    public:

        Listener(const std::string& name, const std::string &ip, int port, bool logConnect=true );

        virtual ~Listener();
        
        void initAndListen(); // never returns unless error (start a thread)

        /* spawn a thread, etc., then return */
        virtual void accepted(boost::shared_ptr<Socket> psocket, long long connectionId );
        virtual void acceptedMP(MessagingPort *mp);

        const int _port;

        /**
         * @return a rough estimate of elapsed time since the server started
           todo: 
           1) consider adding some sort of relaxedLoad semantic to the reading here of 
              _elapsedTime
           2) curTimeMillis() implementations have gotten faster. consider eliminating
              this code?  would have to measure it first.  if eliminated be careful if 
              syscall used isn't skewable.  Note also if #2 is done, listen() doesn't 
              then have to keep waking up and maybe that helps on a developer's laptop 
              battery usage...
         */
        long long getMyElapsedTimeMillis() const { return _elapsedTime; }

        /**
         * Allocate sockets for the listener and set _setupSocketsSuccessful to true
         * iff the process was successful.
         */
        void setupSockets();

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

    private:
        std::vector<SockAddr> _mine;
        std::vector<SOCKET> _socks;
        std::string _name;
        std::string _ip;
        bool _setupSocketsSuccessful;
        bool _logConnect;
        long long _elapsedTime;
        
#ifdef MONGO_SSL
        SSLManagerInterface* _ssl;
#endif
        
        void _logListen( int port , bool ssl );

        static const Listener* _timeTracker;
        
        virtual bool useUnixSockets() const { return false; }

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
            , _sockets( new std::set<int>() )
            , _socketPaths( new std::set<std::string>() )
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
            std::set<int>* sockets;
            std::set<std::string>* paths;

            {
                scoped_lock lk( _mutex );
                sockets = _sockets;
                _sockets = new std::set<int>();
                paths = _socketPaths;
                _socketPaths = new std::set<std::string>();
            }

            for ( std::set<int>::iterator i=sockets->begin(); i!=sockets->end(); i++ ) {
                int sock = *i;
                log() << "closing listening socket: " << sock << std::endl;
                closesocket( sock );
            }

            for ( std::set<std::string>::iterator i=paths->begin(); i!=paths->end(); i++ ) {
                std::string path = *i;
                log() << "removing socket file: " << path << std::endl;
                ::remove( path.c_str() );
            }
        }
        static ListeningSockets* get();
    private:
        mongo::mutex _mutex;
        std::set<int>* _sockets;
        std::set<std::string>* _socketPaths; // for unix domain sockets
        static ListeningSockets* _instance;
    };

}

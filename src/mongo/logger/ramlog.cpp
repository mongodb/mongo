// ramlog.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/logger/ramlog.h"

#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    RamLog::RamLog( const std::string& name ) : _name(name), _totalLinesWritten(0), _lastWrite(0) {
        h = 0; n = 0;
        for( int i = 0; i < N; i++ )
            lines[i][C-1] = 0;

        if ( name.size() ) {
            
            if ( ! _namedLock )
                _namedLock = new mongo::mutex("RamLog::_namedLock");

            scoped_lock lk( *_namedLock );
            if ( ! _named )
                _named = new RM();
            (*_named)[name] = this;
        }
        
    }

    RamLog::~RamLog() {
        
    }

    void RamLog::write(const std::string& str) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        _lastWrite = time(0);
        _totalLinesWritten++;

        char *p = lines[(h+n)%N];
        
        unsigned sz = str.size();
        if (0 == sz) return;
        if( sz < C ) {
            if (str.c_str()[sz-1] == '\n' ) {
                memcpy(p, str.c_str(), sz-1);
                p[sz-1] = 0;
            }
            else 
                strcpy(p, str.c_str());
        }
        else {
            memcpy(p, str.c_str(), C-1);
        }

        if( n < N ) n++;
        else h = (h+1) % N;
    }

    time_t RamLog::lastWrite() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        return _lastWrite;
    }

    long long RamLog::getTotalLinesWritten() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        return _totalLinesWritten;
    }

    void RamLog::get( std::vector<const char*>& v) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        for( unsigned x=0, i=h; x++ < n; i=(i+1)%N )
            v.push_back(lines[i]);
    }

    int RamLog::repeats(const std::vector<const char *>& v, int i) {
        for( int j = i-1; j >= 0 && j+8 > i; j-- ) {
            if( strcmp(v[i]+20,v[j]+20) == 0 ) {
                for( int x = 1; ; x++ ) {
                    if( j+x == i ) return j;
                    if( i+x>=(int) v.size() ) return -1;
                    if( strcmp(v[i+x]+20,v[j+x]+20) ) return -1;
                }
                return -1;
            }
        }
        return -1;
    }


    string RamLog::clean(const std::vector<const char *>& v, int i, string line ) {
        if( line.empty() ) line = v[i];
        if( i > 0 && strncmp(v[i], v[i-1], 11) == 0 )
            return string("           ") + line.substr(11);
        return v[i];
    }

    string RamLog::color(const std::string& line) {
        std::string s = str::after(line, "replSet ");
        if( str::startsWith(s, "warning") || str::startsWith(s, "error") )
            return html::red(line);
        if( str::startsWith(s, "info") ) {
            if( str::endsWith(s, " up\n") )
                return html::green(line);
            else if( str::contains(s, " down ") || str::endsWith(s, " down\n") )
                return html::yellow(line);
            return line; //html::blue(line);
        }

        return line;
    }

    /* turn http:... into an anchor */
    string RamLog::linkify(const char *s) {
        const char *p = s;
        const char *h = strstr(p, "http://");
        if( h == 0 ) return s;

        const char *sp = h + 7;
        while( *sp && *sp != ' ' ) sp++;

        string url(h, sp-h);
        std::stringstream ss;
        ss << string(s, h-s) << "<a href=\"" << url << "\">" << url << "</a>" << sp;
        return ss.str();
    }

    void RamLog::toHTML(std::stringstream& s) {
        std::vector<const char*> v;
        get( v );

        s << "<pre>\n";
        for( int i = 0; i < (int)v.size(); i++ ) {
            verify( strlen(v[i]) > 20 );
            int r = repeats(v, i);
            if( r < 0 ) {
                s << color( linkify( html::escape( clean(v, i) ).c_str() ) ) << '\n';
            }
            else {
                std::stringstream x;
                x << string(v[i], 0, 24);
                int nr = (i-r);
                int last = i+nr-1;
                for( ; r < i ; r++ ) x << '.';
                if( 1 ) {
                    std::stringstream r;
                    if( nr == 1 ) r << "repeat last line";
                    else r << "repeats last " << nr << " lines; ends " << string(v[last]+4,0,15);
                    s << html::a("", r.str(), html::escape( clean(v, i,x.str() ) ) );
                }
                else s << x.str();
                s << '\n';
                i = last;
            }
        }
        s << "</pre>\n";
    }

    RamLogAppender::RamLogAppender(RamLog* ramlog) : _ramlog(ramlog) {}
    RamLogAppender::~RamLogAppender() {}

    Status RamLogAppender::append(const logger::MessageEventEphemeral& event) {
        std::ostringstream ss;
        logger::MessageEventDetailsEncoder encoder;
        encoder.encode(event, ss);
        _ramlog->write(ss.str());
        return Status::OK();
    }

    // ---------------
    // static things
    // ---------------

    RamLog* RamLog::get( const std::string& name ) {
        if ( ! _named )
            return 0;

        scoped_lock lk( *_namedLock );
        RM::iterator i = _named->find( name );
        if ( i == _named->end() )
            return 0;
        return i->second;
    }

    void RamLog::getNames( std::vector<string>& names ) {
        if ( ! _named )
            return;

        scoped_lock lk( *_namedLock );
        for ( RM::iterator i=_named->begin(); i!=_named->end(); ++i ) {
            if ( i->second->n )
                names.push_back( i->first );
        }
    }

    mongo::mutex* RamLog::_namedLock;
    RamLog::RM*  RamLog::_named = 0;
}

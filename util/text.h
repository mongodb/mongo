// text.h

#pragma once

namespace mongo {
    
    class StringSplitter {
    public:
        StringSplitter( const char * big , const char * splitter )
            : _big( big ) , _splitter( splitter ){
        }

        bool more(){
            return _big[0];
        }

        string next(){
            const char * foo = strstr( _big , _splitter );
            if ( foo ){
                string s( _big , foo - _big );
                _big = foo + 1;
                return s;
            }
            
            string s = _big;
            _big += strlen( _big );
            return s;
        }
        
        void split( vector<string>& l ){
            while ( more() ){
                l.push_back( next() );
            }
        }
        
        vector<string> split(){
            vector<string> l;
            split( l );
            return l;
        }

        static vector<string> split( const string& big , const string& splitter ){
            StringSplitter ss( big.c_str() , splitter.c_str() );
            return ss.split();
        }

        static string join( vector<string>& l , const string& split ){
            stringstream ss;
            for ( unsigned i=0; i<l.size(); i++ ){
                if ( i > 0 )
                    ss << split;
                ss << l[i];
            }
            return ss.str();
        }

    private:
        const char * _big;
        const char * _splitter;
    };
    
    
}

// processinfo.cpp

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

#include "pch.h"
#include "processinfo.h"
#include "mmap.h"

#include <iostream>
using namespace std;

namespace mongo {

    class PidFileWiper {
    public:
        ~PidFileWiper() {
            ofstream out( path.c_str() , ios_base::out );
            out.close();
        }

        void write( const string& p ) {
            path = p;
            ofstream out( path.c_str() , ios_base::out );
            out << getpid() << endl;
            out.close();
        }

        string path;
    } pidFileWiper;

    void writePidFile( const string& path ) {
        pidFileWiper.write( path );
    }

    void printMemInfo( const char * where ) {
        cout << "mem info: ";
        if ( where )
            cout << where << " ";
        ProcessInfo pi;
        if ( ! pi.supported() ) {
            cout << " not supported" << endl;
            return;
        }

        cout << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize() << " mapped: " << ( MemoryMappedFile::totalMappedLength() / ( 1024 * 1024 ) ) << endl;
    }


}

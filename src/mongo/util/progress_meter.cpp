// progress_meter.cpp

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

#include "mongo/pch.h" // needed for log.h

#include "mongo/util/progress_meter.h"

#include "mongo/util/log.h"

using namespace std;

namespace mongo {

    void ProgressMeter::reset( unsigned long long total , int secondsBetween , int checkInterval) {
        _total = total;
        _secondsBetween = secondsBetween;
        _checkInterval = checkInterval;
        
        _done = 0;
        _hits = 0;
        _lastTime = (int)time(0);
        
        _active = 1;
    }


    bool ProgressMeter::hit( int n ) {
        if ( ! _active ) {
            warning() << "hit an inactive ProgressMeter" << endl;
            return false;
        }
        
        _done += n;
        _hits++;
        if ( _hits % _checkInterval )
            return false;
        
        int t = (int) time(0);
        if ( t - _lastTime < _secondsBetween )
            return false;
        
        if ( _total > 0 ) {
            int per = (int)( ( (double)_done * 100.0 ) / (double)_total );
            LogstreamBuilder out = log();
            out << "\t\t" << _name << ": " << _done;

            if (_showTotal) {
                out << '/' << _total << '\t' << per << '%';
            }

            if ( ! _units.empty() ) {
                out << "\t(" << _units << ")";
            }
            out << endl;
        }
        _lastTime = t;
        return true;
    }
    
    string ProgressMeter::toString() const {
        if ( ! _active )
            return "";
        stringstream buf;
        buf << _name << ": " << _done << '/' << _total << ' ' << (_done*100)/_total << '%';
        
        if ( ! _units.empty() ) {
            buf << "\t(" << _units << ")" << endl;
        }
        
        return buf.str();
    }


}

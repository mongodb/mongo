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

#include <string>

#include "mongo/logger/log_severity.h"

namespace mongo {
namespace logger {

    /**
     * Deprecated utility for associating a string and log level together.
     */
    class LabeledLevel {
    public:
        LabeledLevel( int level ) : _level( level ) {}
        LabeledLevel( const char* label, int level ) : _label( label ), _level( level ) {}
        LabeledLevel( const std::string& label, int level ) : _label( label ), _level( level ) {}

        LabeledLevel operator+( int i ) const {
            return LabeledLevel( _label, _level + i );
        }

        LabeledLevel operator-( int i ) const {
            return LabeledLevel( _label, _level - i );
        }

        const std::string& getLabel() const { return _label; }
        int getLevel() const { return _level; }

        operator LogSeverity () const { return logger::LogSeverity::cast(_level); }

    private:
        std::string _label;
        int _level;
    };

    inline bool operator<( const LabeledLevel& ll, const int i ) { return ll.getLevel() < i; }
    inline bool operator<( const int i, const LabeledLevel& ll ) { return i < ll.getLevel(); }
    inline bool operator>( const LabeledLevel& ll, const int i ) { return ll.getLevel() > i; }
    inline bool operator>( const int i, const LabeledLevel& ll ) { return i > ll.getLevel(); }
    inline bool operator<=( const LabeledLevel& ll, const int i ) { return ll.getLevel() <= i; }
    inline bool operator<=( const int i, const LabeledLevel& ll ) { return i <= ll.getLevel(); }
    inline bool operator>=( const LabeledLevel& ll, const int i ) { return ll.getLevel() >= i; }
    inline bool operator>=( const int i, const LabeledLevel& ll ) { return i >= ll.getLevel(); }
    inline bool operator==( const LabeledLevel& ll, const int i ) { return ll.getLevel() == i; }
    inline bool operator==( const int i, const LabeledLevel& ll ) { return i == ll.getLevel(); }

}  // namespace logger
}  // namespace mongo

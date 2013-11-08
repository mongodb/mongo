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


#include "mongo/pch.h"

#include "mongo/util/touch_pages.h"

#include <boost/scoped_ptr.hpp>
#include <fcntl.h>
#include <list>
#include <string>

#include "mongo/db/curop.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/database.h"
#include "mongo/util/mmap.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

    char _touch_pages_char_reader; // goes in .bss

    void touch_pages( const char* buf, size_t length ) {
        // read first byte of every page, in order
        for( size_t i = 0; i < length; i += g_minOSPageSizeBytes ) {
            _touch_pages_char_reader += buf[i];
        }
    }
}

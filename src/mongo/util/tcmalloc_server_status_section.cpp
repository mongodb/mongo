/*    Copyright 2013 10gen Inc.
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

#include <third_party/gperftools-2.0/src/gperftools/malloc_extension.h>

#include "mongo/db/commands/server_status.h"

namespace mongo {
namespace {
    class TCMallocServerStatusSection : public ServerStatusSection {
    public:

        TCMallocServerStatusSection() : ServerStatusSection("tcmalloc") {}
        virtual bool includeByDefault() const { return false; }
        
        virtual BSONObj generateSection(const BSONElement& configElement) const {
            BSONObjBuilder builder;

            // For a list of properties see the "Generic Tcmalloc Status" section of
            // http://google-perftools.googlecode.com/svn/trunk/doc/tcmalloc.html and
            // http://code.google.com/p/gperftools/source/browse/src/gperftools/malloc_extension.h
            {
                BSONObjBuilder sub(builder.subobjStart("generic"));
                appendNumericPropertyIfAvailable(sub,    "current_allocated_bytes",
                                                 "generic.current_allocated_bytes");
                appendNumericPropertyIfAvailable(sub,    "heap_size",
                                                 "generic.heap_size");
            }
            {
                BSONObjBuilder sub(builder.subobjStart("tcmalloc"));
                appendNumericPropertyIfAvailable(sub,     "pageheap_free_bytes",
                                                 "tcmalloc.pageheap_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "pageheap_unmapped_bytes",
                                                 "tcmalloc.pageheap_unmapped_bytes");
                appendNumericPropertyIfAvailable(sub,     "max_total_thread_cache_bytes",
                                                 "tcmalloc.max_total_thread_cache_bytes");
                appendNumericPropertyIfAvailable(sub,     "current_total_thread_cache_bytes",
                                                 "tcmalloc.current_total_thread_cache_bytes");
                // Not including tcmalloc.slack_bytes since it is deprecated.

                // These are not available in our version but are available with use-system-tcmalloc
                appendNumericPropertyIfAvailable(sub,     "central_cache_free_bytes",
                                                 "tcmalloc.central_cache_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "transfer_cache_free_bytes",
                                                 "tcmalloc.transfer_cache_free_bytes");
                appendNumericPropertyIfAvailable(sub,     "thread_cache_free_bytes",
                                                 "tcmalloc.thread_cache_free_bytes");
            }

            char buffer[4096];
            MallocExtension::instance()->GetStats(buffer, sizeof(buffer));
            builder.append("formattedString", buffer);

            return builder.obj();
        }

    private:
        static void appendNumericPropertyIfAvailable(BSONObjBuilder& builder,
                                                     StringData bsonName,
                                                     const char* property) {
            size_t value;
            if (MallocExtension::instance()->GetNumericProperty(property, &value))
                builder.appendNumber(bsonName, value);
        }
    } tcmallocServerStatusSection;
}
}


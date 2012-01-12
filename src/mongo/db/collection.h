// @file collection.h

#pragma once

#include "namespace.h"

namespace mongo { 

    class Collection { 
    public:
        NamespaceDetails * const d;
        NamespaceDetailsTransient * const nsd;
    };

}

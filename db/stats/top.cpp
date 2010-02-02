// top.cpp

#include "stdafx.h"
#include "top.h"

namespace mongo {

    Top::T Top::_snapshotStart = Top::currentTime();
    Top::D Top::_snapshotDuration;
    Top::UsageMap Top::_totalUsage;
    Top::UsageMap Top::_snapshotA;
    Top::UsageMap Top::_snapshotB;
    Top::UsageMap &Top::_snapshot = Top::_snapshotA;
    Top::UsageMap &Top::_nextSnapshot = Top::_snapshotB;
    boost::mutex Top::topMutex;


}

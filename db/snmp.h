// snmp.h

#include "../stdafx.h"
#include "../util/background.h"

namespace mongo {
    
    class SNMPAgent : public BackgroundJob {
    public:
        SNMPAgent();

        void enable();
        void makeMaster();

        void run();
    private:
        
        void _init();
        void _checkRegister( int x );
        
        string _agentName;
        
        bool _enabled;
        bool _subagent;

        int _numThings;
        int _snmpIterations;
    };

    extern SNMPAgent snmpAgent;
}

// snmp.cpp

#include "../stdafx.h"
#include "../util/background.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#include <signal.h>

#include "snmp.h"

namespace mongo {
   
    static oid myoid[] =
        { 1, 3, 6, 1, 4, 1, 8072, 2, 4, 1, 1, 2, 0};
    static int myvalue = 18;


    SNMPAgent::SNMPAgent(){
        _enabled = 0;
        _subagent = 1;
        _snmpIterations = 0;
        _numThings = 0;
        _agentName = "mongod";
    }
    
    void SNMPAgent::enable(){
        _enabled = 1;
    }
    
    void SNMPAgent::makeMaster(){
        _subagent = false;
    }
    

    void SNMPAgent::run(){
        if ( ! _enabled ){
            log(1) << "SNMPAgent not enabled" << endl;
            return;
        }
        
        snmp_enable_stderrlog();
        
        if ( _subagent ){
            if ( netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 1) != SNMPERR_SUCCESS ){
                log() << "SNMPAgent faild setting subagent" << endl;
                return;
            }
        }
        
        //SOCK_STARTUP;
        
        init_agent( _agentName.c_str() );
        
        _init();
        log(1) << "SNMPAgent num things: " << _numThings << endl;

        init_snmp( _agentName.c_str() );

        if ( ! _subagent )
            init_master_agent(); 

        log() << "SNMPAgent running" << endl;
        
        while( ! inShutdown() ){
            _snmpIterations++;
            agent_check_and_process(1);
        }

        log() << "SNMPAgent shutting down" << endl;        
        snmp_shutdown( _agentName.c_str() );
        SOCK_CLEANUP;
    }

    void SNMPAgent::_checkRegister( int x ){
        if ( x == MIB_REGISTERED_OK ){
            _numThings++;
            return;
        }
        
        if ( x == MIB_REGISTRATION_FAILED ){
            log() << "SNMPAgent MIB_REGISTRATION_FAILED!" << endl;
        }
        else if ( x == MIB_DUPLICATE_REGISTRATION ){
            log() << "SNMPAgent MIB_DUPLICATE_REGISTRATION!" << endl;
        }
        else {
            log() << "SNMPAgent unknown registration failure" << endl;
        }

    }

    void SNMPAgent::_init(){

        _checkRegister( netsnmp_register_int_instance( "asdasd" , 
                                                       myoid , OID_LENGTH( myoid ) ,
                                                       &myvalue , NULL ) );
    }

    SNMPAgent snmpAgent;
}


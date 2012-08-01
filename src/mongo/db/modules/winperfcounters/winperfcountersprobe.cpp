/** @file mongo/db/modules/winperfcounters/winperfcountersprobe.cpp */

/*
 *    Copyright (C) 2012 Erich Siedler <erich.siedler@gmail.com>
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *    To test new counters, just update CounterNameInfoMap::CounterNameInfoMap()
*/

#include "mongo/db/modules/winperfcounters/winperfcounters.h"
#include "winperfcounters_ctrpp.h" // generated from mongod.man by Windows SDK's ctrpp.exe during build

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <iostream>

#include "mongo/db/CmdLine.h"

using std::cin;
using std::cout;
using std::string;
using std::wstring;

enum CounterType {
    CT_INVALID,
    CT_ULONG,
    CT_ULONGLONG
};

struct CounterInfo {
    unsigned int id;
    CounterType type;

    CounterInfo() : id( 0 ), type( CT_INVALID ) {}
    CounterInfo( unsigned int _id, CounterType _type ) : id( _id ), type( _type ) {}
};

// WMI name mangling:
//      "Some Counter/sec" -> "SomeCounterPersec"
//
// XML manifest type to windows type:
//      perf_counter_rawcount         = CT_ULONG
//      perf_counter_counter          = CT_ULONG
//      perf_counter_bulk_count       = CT_ULONGLONG
//      perf_counter_large_rawcounter = CT_ULONGLONG
class CounterNameInfoMap : public std::map< string, CounterInfo > {
public:
    CounterNameInfoMap() {
        (*this)[ "Connections"                                  ] = CounterInfo( MongoDb_Connections,                   CT_ULONG );
        (*this)[ "ConnectionsAvailable"                         ] = CounterInfo( MongoDb_ConnectionsAvailable,          CT_ULONG );
        (*this)[ "NetworkBytesIn"                               ] = CounterInfo( MongoDb_NetworkBytesIn,                CT_ULONGLONG );
        (*this)[ "NetworkBytesInPersec"                         ] = CounterInfo( MongoDb_NetworkBytesInSec,             CT_ULONGLONG );
        (*this)[ "NetworkBytesOut"                              ] = CounterInfo( MongoDb_NetworkBytesOut,               CT_ULONGLONG );
        (*this)[ "NetworkBytesOutPersec"                        ] = CounterInfo( MongoDb_NetworkBytesOutSec,            CT_ULONGLONG );
        (*this)[ "NetworkRequests"                              ] = CounterInfo( MongoDb_NetworkRequests,               CT_ULONGLONG );
        (*this)[ "NetworksRequestsPersec"                       ] = CounterInfo( MongoDb_NetworkRequestsSec,            CT_ULONGLONG );
        (*this)[ "globalLockCurrentQueue"                       ] = CounterInfo( MongoDb_gL_CurrentQueueTotal,          CT_ULONG );
        (*this)[ "globalLockCurrentQueueReaders"                ] = CounterInfo( MongoDb_gL_CurrentQueueReaders,        CT_ULONG );
        (*this)[ "globalLockCurrentQueueWriters"                ] = CounterInfo( MongoDb_gL_CurrentQueueWriters,        CT_ULONG );
        (*this)[ "globalLockActiveClients"                      ] = CounterInfo( MongoDb_gL_ClientsTotal,               CT_ULONG );
        (*this)[ "globalLockActiveClientsReaders"               ] = CounterInfo( MongoDb_gL_ClientsReaders,             CT_ULONG );
        (*this)[ "globalLockActiveClientsWriters"               ] = CounterInfo( MongoDb_gL_ClientsWriters,             CT_ULONG );
        (*this)[ "Asserts"                                      ] = CounterInfo( MongoDb_AssertsTotal,                  CT_ULONGLONG );
        (*this)[ "AssertsPersec"                                ] = CounterInfo( MongoDb_AssertsTotalPerSec,            CT_ULONGLONG );
        (*this)[ "AssertsRollovers"                             ] = CounterInfo( MongoDb_AssertsRollovers,              CT_ULONG );
        (*this)[ "AssertsRegular"                               ] = CounterInfo( MongoDb_AssertsRegular,                CT_ULONG );
        (*this)[ "AssertsWarning"                               ] = CounterInfo( MongoDb_AssertsWarning,                CT_ULONG );
        (*this)[ "AssertsMessage"                               ] = CounterInfo( MongoDb_AssertsMsg,                    CT_ULONG );
        (*this)[ "AssertsUser"                                  ] = CounterInfo( MongoDb_AssertsUser,                   CT_ULONG );
        (*this)[ "OpCountersPersec"                             ] = CounterInfo( MongoDb_OpPerSec,                      CT_ULONGLONG );
        (*this)[ "OpCountersInsertsPersec"                      ] = CounterInfo( MongoDb_OpInsertPerSec,                CT_ULONG );
        (*this)[ "OpCountersQueriesPersec"                      ] = CounterInfo( MongoDb_OpQueryPerSec,                 CT_ULONG );
        (*this)[ "OpCountersUpdatesPersec"                      ] = CounterInfo( MongoDb_OpUpdatePerSec,                CT_ULONG );
        (*this)[ "OpCountersDeletesPersec"                      ] = CounterInfo( MongoDb_OpDeletePerSec,                CT_ULONG );
        (*this)[ "OpCountersGetMorePersec"                      ] = CounterInfo( MongoDb_OpGetMorePerSec,               CT_ULONG );
        (*this)[ "OpCountersCommandsPersec"                     ] = CounterInfo( MongoDb_OpCommandPerSec,               CT_ULONG );
    }

private:
    CounterNameInfoMap( CounterNameInfoMap const& );
    CounterNameInfoMap const& operator=( CounterNameInfoMap const& );
};

namespace mongo {

    class WinPerfCountersProbe : public WinPerfCounters {
    public:
        WinPerfCountersProbe( wstring const& name ) : _perf_instance_name( name ) {}

        // valueset_line: "PerformanceMonitorClients=123;ConnectionsCurrent=456"
        void setValues( string const& valueset_line ) {
            CounterNameInfoMap counters_map;
            typedef boost::split_iterator< string::const_iterator > it_type;

            for ( it_type it = boost::make_split_iterator( valueset_line, boost::token_finder( boost::is_any_of( "=;" ) ) ); ! it.eof(); ) {
                string name( boost::copy_range< string >( *it++ ) );
                verify( ! it.eof() );
                string value( boost::copy_range< string >( *it++ ) );

                switch( counters_map[ name ].type ) {
                case CT_ULONG:
                    ::PerfSetULongCounterValue( Ctrpp_MongoDbProvider, _setInstance, counters_map[ name ].id, boost::lexical_cast< ULONG >( value ) );
                    break;
                case CT_ULONGLONG:
                    ::PerfSetULongLongCounterValue( Ctrpp_MongoDbProvider, _setInstance, counters_map[ name ].id, boost::lexical_cast< ULONGLONG >( value ) );
                    break;
                default:
                    verify( false );
                }
            }
        }

    protected:
        virtual wstring perf_instance_name() const {
            return _perf_instance_name;
        }

    private:
        wstring _perf_instance_name;
        virtual void updateCounters() {} // we will update manually with setValues()
    };

    // make the linker happy
    CmdLine cmdLine;
    bool initService() { verify( false ); return false; }

} // namespace mongo

void ok() {
    std::cout << "OK\n";
}

int wmain( int argc, wchar_t const* argv[] ) {
    if ( 2 != argc ) {
        cout << "Parameter error, use: program.exe <string_unique_name>\n";
        return 1;
    }

    // boot WinPerfCounters module
    namespace po = boost::program_options;
    po::variables_map vm;
    vm.insert( po::variables_map::value_type( "winPerfCounters", po::variable_value() ) );
    mongo::WinPerfCountersProbe probe( argv[1] );
    probe.config( vm );

    // respond to test commands
    string cmd;
    do {
        cout << "READY\n";
        cmd.clear();
        cin >> cmd;

        if ( "INIT" == cmd ) {
            // init() returns after PerfCreateInstance(). Although the SDK doesn't say, we are assuming
            // that this perfcounters instance is immediately visible systemwide
            probe.init();
            ok();
        }
        else if ( "SHUTDOWN" == cmd ) {
            probe.shutdown();
            ok();
        }
        else if ( boost::starts_with( cmd, "SET:" ) ) {
            probe.setValues( cmd.substr( 4 ) ); // cmd without "SET:"
            ok();
        }
        else {
            cout << "UNKNOWN\n";
        }
    } while ( cin.good() && "QUIT" != cmd );

    return 0;
}

// createCPPfromJavaScriptFiles.js

/*   Copyright 2011 10gen Inc.
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

// This JavaScript file is run under Windows Script Host from the Visual Studio build.
// It creates .CPP files from JavaScript files and is intended to duplicate the functionality
// of the jsToH Python function in SConstruct.  By using only standard Windows components
// (Windows Script Host, JScript) we avoid the need for Visual Studio builders to install
// Python, and we don't need to include the generated files in Git because they can be
// recreated as required.

var whitespace = " \t";
function cppEscape( s ) {
    for ( var i = s.length - 1; i >= 0; --i ) {
        if ( whitespace.indexOf( s.charAt( i ) ) === -1 ) {
            s = s.substr( 0, i + 1 );
            break;
        }
    }
    s = s.replace( /\\/g, "\\\\" );
    s = s.replace( /"/g, '\\"' );
    return s;
};

function jsToH( fso, outputFileNameString, inputFileNameStringArray ) {
    var displayString = 'jsToH( "' + outputFileNameString + '", [';
    var i, len = inputFileNameStringArray.length;
    for ( i = 0; i < len; ++i ) {
        displayString += '"' + inputFileNameStringArray[i] + '"';
        if ( i < len - 1 )
            displayString += ', ';
    }
    displayString += '] );'
    WScript.Echo( displayString );
    var h = ['#include "mongo/base/string_data.h"'
         , 'namespace mongo {'
         , 'struct JSFile{ const char* name; const StringData& source; };'
         , 'namespace JSFiles{'
         ];
    for ( i = 0; i < len; ++i ) {
        var filename = inputFileNameStringArray[i];
        var objname = filename.substring( 0, filename.lastIndexOf( '.' ) ).substr( 1 + filename.lastIndexOf('/') );
        var stringname = '_jscode_raw_' + objname;
        h.push( 'const StringData ' + stringname + ' = ' );
        var inputFile = fso.GetFile( filename );
        var inputStream = inputFile.OpenAsTextStream( 1 /* ForReading */, 0 /* TristateFalse == ASCII */ );
        while ( !inputStream.AtEndOfStream )
            h.push( '"' + cppEscape(inputStream.ReadLine()) + '\\n" ' );
        inputStream.Close();
        h.push( ';' );
        h.push( 'extern const JSFile ' + objname + ';' ); //symbols aren't exported w/o this
        h.push( 'const JSFile ' + objname + ' = { "' + filename + '" , ' + stringname + ' };' );
    }
    h.push( "} // namespace JSFiles" );
    h.push( "} // namespace mongo" );
    h.push( "" );
    var out = fso.CreateTextFile( outputFileNameString, true /* overwrite */ );
    out.Write( h.join( '\n' ) );
    out.Close();
};

function rebuildIfNeeded( fso, outputFileNameString, inputFileNameStringArray ) {
    var rebuildNeeded = false;
    if ( !fso.FileExists( outputFileNameString ) ) {
        rebuildNeeded = true;
    } else {
        var outputFileDate = fso.GetFile( outputFileNameString ).DateLastModified;
        for ( var i = 0, len = inputFileNameStringArray.length; i < len; ++i ) {
            if ( fso.GetFile( inputFileNameStringArray[i] ).DateLastModified > outputFileDate ) {
                rebuildNeeded = true;
                break;
            }
        }
    }
    if ( rebuildNeeded )
        jsToH( fso, outputFileNameString, inputFileNameStringArray );
};

var shell = new ActiveXObject( "WScript.Shell" );
shell.CurrentDirectory = WScript.Arguments.Unnamed.Item( 0 );

var fso = new ActiveXObject( "Scripting.FileSystemObject" );
rebuildIfNeeded(fso, "shell/mongo.cpp", ["shell/assert.js", "shell/types.js", "shell/utils.js", "shell/utils_sh.js",
                                         "shell/db.js", "shell/mongo.js", "shell/mr.js",
                                         "shell/query.js", "shell/collection.js"]);
rebuildIfNeeded(fso, "shell/mongo-server.cpp", ["shell/servers.js", "shell/shardingtest.js",
                                                "shell/servers_misc.js", "shell/replsettest.js",
                                                "shell/replsetbridge.js"]);

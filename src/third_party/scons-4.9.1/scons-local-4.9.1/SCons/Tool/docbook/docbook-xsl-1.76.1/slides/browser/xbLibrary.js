/*
 * xbLibrary.js
 * $Revision: 1.3 $ $Date: 2003/03/17 03:44:20 $
 */

/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Bob Clary code.
 *
 * The Initial Developer of the Original Code is
 * Bob Clary.
 * Portions created by the Initial Developer are Copyright (C) 2000
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): Bob Clary <bc@bclary.com>
 *
 * ***** END LICENSE BLOCK ***** */

if (!document.getElementById || navigator.userAgent.indexOf('Opera') != -1)
{
  // assign error handler for downlevel browsers
  // Note until Opera improves it's overall support
  // for JavaScript and the DOM, it must be considered downlevel

  window.onerror = defaultOnError;
  
  function defaultOnError(msg, url, line)
  {
    // handle bug in NS6.1, N6.2
    // where an Event is passed to error handlers
    if (typeof(msg) != 'string')
    {
        msg = 'unknown error';
    }
    if (typeof(url) != 'string')
    {
        url = document.location;
    }

    alert('An error has occurred at ' + url + ', line ' + line + ': ' + msg);
  }
}

function xbLibrary(path)
{
  if (path.charAt(path.length-1) == '/')
  {
    path = path.substr(0, path.length-1)
  }
  this.path = path;
}

// dynamically loaded scripts
//
// it is an error to reference anything from the dynamically loaded file inside the
// same script block.  This means that a file can not check its dependencies and
// load the files for it's own use.  someone else must do this.  
  
xbLibrary.prototype.loadScript = 
function (scriptName)
{
  document.write('<script language="javascript" src="' + this.path + '/' + scriptName + '"><\/script>');
};

// default xbLibrary

xblibrary = new xbLibrary('./');



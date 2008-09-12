// commands.h

/**
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

#pragma once

// db "commands" (sent via db.$cmd.findOne(...))
class Command { 
public:
    string name;

    /* run the given command 
       implement this...
       return value is true if succeeded.  if false, set errmsg text.
    */
    virtual bool run(const char *ns, JSObj& cmdObj, string& errmsg, JSObjBuilder& result) = 0;

    /* return true if only the admin ns has privileges to run this command. */
    virtual bool adminOnly() { return false; }

    Command(const char *_name);
};

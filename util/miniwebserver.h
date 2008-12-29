// miniwebserver.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "../grid/message.h"

class MiniWebServer {
public:
    MiniWebServer();

    bool init(int port);
    void run();

    virtual void doRequest(
        const char *rq, // the full request
        string url,
        // set these and return them:
        string& responseMsg,
        int& responseCode,
        vector<string>& headers // if completely empty, content-type: text/html will be added
    ) = 0;

private:
    void accepted(int s);

    int sock;
};

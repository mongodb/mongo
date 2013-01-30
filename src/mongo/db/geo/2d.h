/**
*    Copyright (C) 2012 10gen Inc.
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

namespace mongo {
    // We need cmdObj and parsedArgs so we can print a useful error msg
    // and pull other args out.
    bool run2DGeoNear(const IndexDetails &id, const BSONObj& cmdObj,
                      const GeoNearArguments &parsedArgs, string& errmsg,
                      BSONObjBuilder& result);
}  // namespace mongo

/**
*    Copyright (C) 2013 10gen Inc.
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

    /**
     * The 'WireVersion' captures all "protocol events" the write protocol went through.  A
     * protocol event is a change in the syntax of messages on the wire or the semantics of
     * existing messages. We may also add "logical" entries for releases, although that's not
     * mandatory.
     *
     * We use the wire version to determine if two agents (a driver, a mongos, or a mongod) can
     * interact. Each agent carries two versions, a 'max' and a 'min' one. If the two agents
     * are on the same 'max' number, they stricly speak the same wire protocol and it is safe
     * to allow them to communicate. If two agents' ranges do not intersect, they should not be
     * allowed to communicate.
     *
     * If two agents have at least one version in common they can communicate, but one of the
     * sides has to be ready to compensate for not being on its partner version.
     */
    enum WireVersion {
        // Everything before we started tracking.
        RELEASE_2_4_AND_BEFORE = 0,

        // The aggregation command may now be requested to return cursors.
        AGG_RETURNS_CURSORS = 1,
    };

    // Latest version that the server accepts. This should always be at the latest entry in
    // WireVersion.
    static const int maxWireVersion = AGG_RETURNS_CURSORS;

    // Minimum version that the server accepts. We should bump this whenever we don't want
    // to allow communication with too old agents.
    static const int minWireVersion = RELEASE_2_4_AND_BEFORE;

} // namespace mongo

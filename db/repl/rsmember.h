// @file rsmember.h

/** replica set member */

#pragma once

namespace mongo {

    enum MemberState { 
        STARTUP,
        PRIMARY,
        SECONDARY,
        RECOVERING,
        FATAL,
        STARTUP2,
        UNKNOWN /* remote node not yet reached */
    };

    /* this is supposed to be just basic information on a member, 
       and copy constructable. */
    class RSMember { 
        unsigned _id;
    public:
        RSMember() : _id(0xffffffff) { }
        RSMember(unsigned id);
        bool up() const { return health > 0; }
        unsigned id() const { return _id; }
        MemberState hbstate;
        double health;
        time_t upSince;
        time_t lastHeartbeat;
        string lastHeartbeatMsg;
        bool changed(const RSMember& old) const;
    };

    inline RSMember::RSMember(unsigned id) : _id(id) { 
          hbstate = UNKNOWN;
          health = -1.0;
          lastHeartbeat = upSince = 0; 
    }

    inline bool RSMember::changed(const RSMember& old) const { 
        return health != old.health ||
               hbstate != old.hbstate;
    }

}

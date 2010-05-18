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
        HostAndPort _h;
        unsigned _id;
    public:
        RSMember() : _id(0xffffffff) { }
        RSMember(const HostAndPort& h, unsigned id);
        bool up() const { return health > 0; }
        const HostAndPort& h() const { return _h; }
        unsigned id() const { return _id; }
        MemberState state;
        double health;
        time_t upSince;
        time_t lastHeartbeat;
        string lastHeartbeatMsg;
        bool changed(const RSMember& old) const;
    };

    inline RSMember::RSMember(const HostAndPort& h, unsigned id) :
    _h(h), _id(id) { 
          state = UNKNOWN;
          health = -1.0;
          lastHeartbeat = upSince = 0; 
    }

    inline bool RSMember::changed(const RSMember& old) const { 
        return health != old.health ||
               state != old.state;
    }

}

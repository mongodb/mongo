#pragma once

#include <wiredtiger.h>

#include "mongo/util/assert_util.h"

namespace mongo {
    class WiredTigerDatabase {
    public:
        WiredTigerDatabase(WT_CONNECTION *conn) : _conn(conn) {}
        ~WiredTigerDatabase() {}

        WT_SESSION *GetSession(bool acquire = false) {
            // TODO thread-local check / open / cache
            WT_SESSION *s = NULL;
            int ret = _conn->open_session(_conn, NULL, NULL, &s);
            invariant(ret == 0);
            return s;
        }

        void ReleaseSession(WT_SESSION *session) {
            if (session) {
                int ret = session->close(session, NULL);
                invariant(ret == 0);
            }
        }

        WT_CONNECTION *Get() const { return _conn; }

    private:
        WT_CONNECTION *_conn;
    };

    struct WiredTigerSession {
        WiredTigerSession(WT_SESSION *session, WiredTigerDatabase &db)
          : _session(session), _db(db) {}
        WiredTigerSession(WiredTigerDatabase &db) : _session(db.GetSession()), _db(db) {}
        ~WiredTigerSession() { _db.ReleaseSession(_session); }

        void ReleaseCursor(WT_CURSOR *c) {
            if (c) {
                int ret = c->close(c);
                invariant(ret == 0);
            }
        }

        WT_SESSION *Get() const { return _session; }
        WT_CURSOR *GetCursor(const std::string &uri, bool acquire = false) {
            WT_CURSOR *c = NULL;
            int ret = _session->open_cursor(_session, uri.c_str(), NULL, NULL, &c);
            invariant(ret == 0 || ret == ENOENT);
            return c;
        }
        WiredTigerDatabase &GetDatabase(void) { return _db; }
    
    private:
        WT_SESSION *_session;
        WiredTigerDatabase &_db;
    };

    class WiredTigerCursor {
    public:
        WiredTigerCursor(WT_CURSOR *cursor, WiredTigerSession &session, bool own_session=false)
            : _cursor(cursor), _session(session), _own_session(own_session) {}
        ~WiredTigerCursor() {
            _session.ReleaseCursor(_cursor);
            // XXX avoid leaks
            if (_own_session)
                delete &_session;
        }

        WiredTigerSession &GetSession(void) { return _session; }
        WT_CURSOR *Get() const { return _cursor; }
    
    private:
        WT_CURSOR *_cursor;
        WiredTigerSession &_session;
        bool _own_session;
    };

    struct WiredTigerItem : public WT_ITEM {
        WiredTigerItem(const void *d, size_t s) {
            data = d;
            size = s;
        }
        WiredTigerItem(const std::string &str) {
            data = str.c_str();
            size = str.size();
        }
        WT_ITEM *Get() { return this; }
    };
}

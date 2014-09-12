#pragma once

#include <wiredtiger.h>

#include "mongo/util/assert_util.h"

namespace mongo {
    class WiredTigerOperationContext;

    class WiredTigerDatabase {
    public:
        WiredTigerDatabase(WT_CONNECTION *conn) : _conn(conn) {}
        ~WiredTigerDatabase();

        WiredTigerOperationContext &GetContext();
        void ReleaseContext(WiredTigerOperationContext &ctx);
        WT_CONNECTION *Get() const { return _conn; }

        void ClearCache();

    private:
        WT_CONNECTION *_conn;
        mutable boost::mutex _ctxLock;
        typedef std::vector<WiredTigerOperationContext *> ContextVector;
        ContextVector _ctxCache;
    };

    class WiredTigerOperationContext {
    public:
        WiredTigerOperationContext(WiredTigerDatabase &db) : _db(db), _session(0) {
            WT_CONNECTION *conn = _db.Get();
            int ret = conn->open_session(conn, NULL, NULL, &_session);
            invariant(ret == 0);
        }
        ~WiredTigerOperationContext() {
            if (_session) {
                int ret = _session->close(_session, NULL);
                invariant(ret == 0);
            }
        }

        WiredTigerDatabase &GetDatabase(void) { return _db; }
        WT_SESSION *GetSession() const { return _session; }
        WT_CURSOR *GetCursor(const std::string &uri) {
            WT_CURSOR *c = _curmap[uri];
            if (c) {
                _curmap.erase(uri);
                return c;
            }
            int ret = _session->open_cursor(_session, uri.c_str(), NULL, NULL, &c);
            invariant(ret == 0 || ret == ENOENT);
            return c;
        }

        void ReleaseCursor(WT_CURSOR *cursor) {
            const std::string uri(cursor->uri);
            if (_curmap[uri]) {
                int ret = cursor->close(cursor);
                invariant(ret == 0);
            } else {
                int ret = cursor->reset(cursor);
                invariant(ret == 0);
                _curmap[uri] = cursor;
            }
        }

        void CloseAllCursors() {
            for (CursorMap::iterator i = _curmap.begin(); i != _curmap.end(); i = _curmap.begin()) {
                WT_CURSOR *cursor = i->second;
                if (cursor) {
                    int ret = cursor->close(cursor);
                    invariant(ret == 0);
                }
                _curmap.erase(i);
            }
        }
    
    private:
        WiredTigerDatabase &_db;
        WT_SESSION *_session;
        typedef std::map<const std::string, WT_CURSOR *> CursorMap;
        CursorMap _curmap;
    };

    class WiredTigerSession {
    public:
        WiredTigerSession(WiredTigerDatabase &db)
            : _db(db), _ctx(db.GetContext()) {}
        ~WiredTigerSession() { _db.ReleaseContext(_ctx); }

        WiredTigerDatabase &GetDatabase(void) { return _ctx.GetDatabase(); }
        WiredTigerOperationContext &GetContext(void) { return _ctx; }
        WT_SESSION *Get() const { return _ctx.GetSession(); }
        WT_CURSOR *GetCursor(const std::string &uri) { return _ctx.GetCursor(uri); }
        void ReleaseCursor(WT_CURSOR *c) { _ctx.ReleaseCursor(c); }
    
    private:
        WiredTigerDatabase &_db;
        WiredTigerOperationContext &_ctx;
    };

    class WiredTigerCursor {
    public:
        WiredTigerCursor(const std::string &uri, WiredTigerSession& session)
            : _cursor(session.GetCursor(uri)), _session(session) {}
        ~WiredTigerCursor() { _session.ReleaseCursor(_cursor); }

        WiredTigerSession &GetSession(void) { return _session; }
        WT_CURSOR *Get() const { return _cursor; }
    
    private:
        WT_CURSOR *_cursor;
        WiredTigerSession &_session;
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

#ifndef _WIREDTIGER_DATABASE_H_
#define _WIREDTIGER_DATABASE_H_ 1

#include <wiredtiger.h>

namespace mongo {
	struct WiredTigerDatabase {
		WT_SESSION *GetSession(bool acquire = false) {
			// TODO thread-local check / open / cache
			return NULL;
		}

		void ReleaseSession(WT_SESSION *session) {}

		WT_CURSOR *GetCursor(const std::string &uri, bool acquire = false) { return NULL; }
		void ReleaseCursor(WT_CURSOR *c) {}

		WT_CONNECTION *conn;
	};

	struct WiredTigerCursor {
		WiredTigerCursor(WT_CURSOR *cursor, WiredTigerDatabase *db) : _cursor(cursor), _db(db) {}
		~WiredTigerCursor() { _db->ReleaseCursor(_cursor); }

		WT_CURSOR *Get() const { return _cursor; }
	
	private:
		WT_CURSOR *_cursor;
		WiredTigerDatabase *_db;
	};

	struct WiredTigerSession {
		WiredTigerSession(WT_SESSION *session, WiredTigerDatabase *db) : _session(session), _db(db) {}
		~WiredTigerSession() { _db->ReleaseSession(_session); }

		WT_SESSION *Get() const { return _session; }
	
	private:
		WT_SESSION *_session;
		WiredTigerDatabase *_db;
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
	};
}

#endif

// Populate global variables from modules for backwards compatibility

import {
    _DelegatingDriverSession,
    _DummyDriverSession,
    _ServerSession,
    DriverSession,
    SessionOptions
} from "src/mongo/shell/session.js";

globalThis.DriverSession = DriverSession;
globalThis.SessionOptions = SessionOptions;
globalThis._DummyDriverSession = _DummyDriverSession;
globalThis._DelegatingDriverSession = _DelegatingDriverSession;
globalThis._ServerSession = _ServerSession;

// Populate global variables from modules for backwards compatibility

import {MongoBridge} from "src/mongo/shell/bridge.js";

globalThis.MongoBridge = MongoBridge;

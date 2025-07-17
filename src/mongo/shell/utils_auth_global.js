// Populate global variables from modules for backwards compatibility

import {authutil} from "src/mongo/shell/utils_auth.js";

globalThis.authutil = authutil;

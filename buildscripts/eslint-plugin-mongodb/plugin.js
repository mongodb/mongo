// plugin.js

import {default as no_print} from "./rules/no-print-fn.js";
import {default as no_tojson} from "./rules/no-tojson-fn.js";

export default {
    rules: {
        "no-print-fn": 0,
        "no-tojson-fn": 0,
    },
};

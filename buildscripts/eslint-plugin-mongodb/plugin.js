// plugin.js

import {default as no_print} from "./rules/no-print-fn.js";
import {default as no_tojson} from "./rules/no-tojson-fn.js";

export default {
    rules: {
        "no-print-fn": no_print,
        "no-tojson-fn": no_tojson,
    },
};

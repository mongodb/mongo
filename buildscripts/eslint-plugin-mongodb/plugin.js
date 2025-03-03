// plugin.js

import {default as no_print} from "./rules/no-print-fn.js";
import {default as no_printing_tojson} from "./rules/no-printing-tojson.js";

export default {
    rules: {
        "no-print-fn": no_print,
        "no-printing-tojson": no_printing_tojson,
    },
};

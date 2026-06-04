// plugin.js

import {default as no_print} from "./rules/no-print-fn.js";
import {default as no_printing_tojson} from "./rules/no-printing-tojson.js";
import {default as no_non_shell_imports} from "./rules/no-non-shell-imports.js";

export default {
    rules: {
        "no-print-fn": no_print,
        "no-printing-tojson": no_printing_tojson,
        "no-non-shell-imports": no_non_shell_imports,
    },
};

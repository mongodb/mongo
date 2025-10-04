// Populate global variables from modules for backwards compatibility

import {Explain, Explainable} from "src/mongo/shell/explainable.js";

globalThis.Explain = Explain;
globalThis.Explainable = Explainable;

// FTDC test cases
//
import {verifyCommonFTDCParameters} from "jstests/libs/ftdc.js";

let admin = db.getSiblingDB("admin");

verifyCommonFTDCParameters(admin, true);

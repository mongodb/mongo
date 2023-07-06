// FTDC test cases
//
import {verifyCommonFTDCParameters} from "jstests/libs/ftdc.js";

var admin = db.getSiblingDB("admin");

verifyCommonFTDCParameters(admin, true);

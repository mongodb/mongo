/* Tests getting properties from string bundles
 */

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

const name_file = "file";
const value_file = "File";

const name_loyal = "loyal";
const value_loyal = "\u5fe0\u5fc3"; // tests escaped Unicode

const name_trout = "trout";
const value_trout = "\u9cdf\u9b5a"; // tests UTF-8

const name_edit = "edit";
const value_edit = "Edit"; // tests literal leading spaces are stripped

const name_view = "view";
const value_view = "View"; // tests literal trailing spaces are stripped

const name_go = "go";
const value_go = " Go"; // tests escaped leading spaces are not stripped

const name_message = "message";
const value_message = "Message "; // tests escaped trailing spaces are not stripped

const name_hello = "hello";
const var_hello = "World";
const value_hello = "Hello World"; // tests formatStringFromName with parameter

function run_test() {
  var StringBundle = Services.strings;
  var bundleURI = Services.io.newFileURI(do_get_file("strres.properties"));

  var bundle = StringBundle.createBundle(bundleURI.spec);

  var bundle_file = bundle.GetStringFromName(name_file);
  Assert.equal(bundle_file, value_file);

  var bundle_loyal = bundle.GetStringFromName(name_loyal);
  Assert.equal(bundle_loyal, value_loyal);

  var bundle_trout = bundle.GetStringFromName(name_trout);
  Assert.equal(bundle_trout, value_trout);

  var bundle_edit = bundle.GetStringFromName(name_edit);
  Assert.equal(bundle_edit, value_edit);

  var bundle_view = bundle.GetStringFromName(name_view);
  Assert.equal(bundle_view, value_view);

  var bundle_go = bundle.GetStringFromName(name_go);
  Assert.equal(bundle_go, value_go);

  var bundle_message = bundle.GetStringFromName(name_message);
  Assert.equal(bundle_message, value_message);

  var bundle_hello = bundle.formatStringFromName(name_hello, [var_hello]);
  Assert.equal(bundle_hello, value_hello);
}

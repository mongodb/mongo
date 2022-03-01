/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

function run_test() {
  test_methods_presence(FluentBundle);
  test_methods_calling(FluentBundle, FluentResource);
  test_number_options(FluentBundle, FluentResource);

  ok(true);
}

function test_methods_presence(FluentBundle) {
  const bundle = new FluentBundle(["en-US", "pl"]);
  equal(typeof bundle.addResource, "function");
  equal(typeof bundle.formatPattern, "function");
}

function test_methods_calling(FluentBundle, FluentResource) {
  const bundle = new FluentBundle(["en-US", "pl"], {
    useIsolating: false,
  });
  bundle.addResource(new FluentResource("key = Value"));

  const msg = bundle.getMessage("key");
  equal(bundle.formatPattern(msg.value), "Value");

  bundle.addResource(new FluentResource("key2 = Hello { $name }"));

  const msg2 = bundle.getMessage("key2");
  equal(bundle.formatPattern(msg2.value, { name: "Amy" }), "Hello Amy");
  ok(true);
}

function test_number_options(FluentBundle, FluentResource) {
  const bundle = new FluentBundle(["en-US", "pl"], {
    useIsolating: false,
  });
  bundle.addResource(new FluentResource(`
key = { NUMBER(0.53, style: "percent") } { NUMBER(0.12, style: "percent", minimumFractionDigits: 0) }
    { NUMBER(-2.5, style: "percent") } { NUMBER(2.91, style: "percent") } { NUMBER("wrong", style: "percent") }
`));

  const msg = bundle.getMessage("key");
  equal(bundle.formatPattern(msg.value), "53.00% 12%\n-250.0% 291.00% ");

  ok(true);
}

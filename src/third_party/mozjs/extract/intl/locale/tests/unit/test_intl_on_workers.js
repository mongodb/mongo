function run_test() {
  do_load_manifest("data/chrome.manifest");

  if (typeof Intl !== "object") {
    dump("Intl not enabled, skipping test\n");
    equal(true, true);
    return;
  }

  let mainThreadLocale = Intl.NumberFormat().resolvedOptions().locale;
  let testWorker = new Worker(
    "chrome://locale/content/intl_on_workers_worker.js"
  );
  testWorker.onmessage = function(e) {
    try {
      let workerLocale = e.data;
      equal(
        mainThreadLocale,
        workerLocale,
        "Worker should inherit Intl locale from main thread."
      );
    } finally {
      do_test_finished();
    }
  };

  do_test_pending();
  testWorker.postMessage("go!");
}

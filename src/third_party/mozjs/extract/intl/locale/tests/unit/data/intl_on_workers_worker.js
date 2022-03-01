self.onmessage = function(data) {
  let myLocale = Intl.NumberFormat().resolvedOptions().locale;
  self.postMessage(myLocale);
};

// General high-order functions

function forEach (action, array) {
  for (var i = 0; i < array.length; i++)
    action (array[i]);
}

function foldl (combine, base, array) {
  for (var i = 0; i < array.length; i++)
    base = combine (base, array[i]);
  return base
}

function foldr (combine, base, array) {
  for (var i = array.length - 1; i >= 0; i--)
    base = combine (array[i], base);
  return base
}

function map (func, array) {
  var result = [];
  for (var i = 0; i < array.length; i++)
    result.push (func (array[i]));
  return result
}

function filter (pred, array) {
  var result = []
  for (var i = 0; i < array.length; i++)
    if (pred (array[i])) result.push (array[i]);
  return result
}

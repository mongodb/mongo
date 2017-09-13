test();

function test()
{
  function generate_big_object_graph()
  {
    var root = {};
    f(root, 17);
    return root;
    function f(parent, depth) {
      if (depth == 0) 
          return;
      --depth;

      f(parent.a = {}, depth);
      f(parent.b = {}, depth);
    }
  }

  function f(obj) {
    with (obj)
      return arguments;
  }

  for(var i = 0; i != 10; ++i) 
  {
    gc();
    var x = null;
    x = f(generate_big_object_graph());

    gc(); //all used

    x = null;

    gc(); //all free
  }
}

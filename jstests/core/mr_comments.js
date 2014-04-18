
t = db.mr_comments
t.drop()

t.insert( { foo : 1 } )
t.insert( { foo : 1 } )
t.insert( { foo : 2 } )

res = db.runCommand(
    { mapreduce : "mr_comments",
      map : "// This will fail\n\n    // Emit some stuff\n    emit(this.foo, 1)\n",
      reduce : function(key, values){
          return Array.sum(values);
      },
      out: "mr_comments_out"
    });
assert.eq( 3 , res.counts.emit )

res = db.runCommand(
    { mapreduce : "mr_comments",
      map : "// This will fail\nfunction(){\n    // Emit some stuff\n    emit(this.foo, 1)\n}\n",
      reduce : function(key, values){
          return Array.sum(values);
      },
      out: "mr_comments_out"
    });

assert.eq( 3 , res.counts.emit )

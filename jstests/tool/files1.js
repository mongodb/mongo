// files1.js

t = new ToolTest( "files1" )

db = t.startDB();

filename = 'mongod'
if ( _isWindows() )
    filename += '.exe'

t.runTool( "files" , "-d" , t.baseName , "put" , filename );
md5 = md5sumFile(filename);

file_obj = db.fs.files.findOne()
assert( file_obj , "A 0" );
md5_stored = file_obj.md5;
md5_computed = db.runCommand({filemd5: file_obj._id}).md5;
assert.eq( md5 , md5_stored , "A 1" );
assert.eq( md5 , md5_computed, "A 2" );

mkdir(t.ext);

t.runTool( "files" , "-d" , t.baseName , "get" , filename , '-l' , t.extFile );
md5 = md5sumFile(t.extFile);
assert.eq( md5 , md5_stored , "B" );

t.stop()

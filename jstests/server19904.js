// This file is created to reproduce the bug reported under ticket SERVER-19904, for temporary usage
db.fs.drop();
db.fs.chunks.ensureIndex({
    n: 1
});

db.fs.chunks.insert({
    files_id: 1,
    n: 0,
});

db.runCommand({
    'filemd5': 1
});

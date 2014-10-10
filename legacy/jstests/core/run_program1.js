if ( ! _isWindows() ) {
    
    // note that normal program exit returns 0
    assert.eq (0, runProgram('true'))
    assert.neq(0, runProgram('false'))
    assert.neq(0, runProgram('this_program_doesnt_exit'));

    //verify output visually
    runProgram('echo', 'Hello', 'World.', 'How   are   you?');
    runProgram('bash', '-c',  'echo Hello     World. "How   are   you?"'); // only one space is printed between Hello and World

    // numbers can be passed as numbers or strings
    runProgram('sleep', 0.5);
    runProgram('sleep', '0.5');

} else {

    runProgram('cmd', '/c', 'echo hello windows');
}

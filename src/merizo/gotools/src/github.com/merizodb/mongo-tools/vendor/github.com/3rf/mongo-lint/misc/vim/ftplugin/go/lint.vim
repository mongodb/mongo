" Copyright 2013 The Go Authors. All rights reserved.
" Use of this source code is governed by a BSD-style
" license that can be found in the LICENSE file.
"
" lint.vim: Vim command to lint Go files with golint.
"
"   https://github.com/golang/lint
"
" This filetype plugin add a new commands for go buffers:
"
"   :Lint
"
"       Run golint for the current Go file.
"
if exists("b:did_ftplugin_go_lint")
    finish
endif

if !executable("golint")
    finish
endif

command! -buffer Lint call s:GoLint()

function! s:GoLint() abort
    cexpr system('golint ' . shellescape(expand('%')))
endfunction

let b:did_ftplugin_go_lint = 1

" vim:ts=4:sw=4:et

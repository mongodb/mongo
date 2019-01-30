    /*
     * Generating switch for the list of 53 entries:
     * false
     * true
     * null
     * break
     * case
     * catch
     * const
     * continue
     * debugger
     * default
     * delete
     * do
     * else
     * finally
     * for
     * function
     * if
     * in
     * instanceof
     * new
     * return
     * switch
     * this
     * throw
     * try
     * typeof
     * var
     * void
     * while
     * with
     * import
     * export
     * class
     * extends
     * super
     * enum
     * implements
     * interface
     * package
     * private
     * protected
     * public
     * as
     * async
     * await
     * from
     * get
     * let
     * of
     * set
     * static
     * target
     * yield
     */
    switch (JSRW_LENGTH()) {
      case 2:
        switch (JSRW_AT(1)) {
          case 'f':
            if (JSRW_AT(0) == 'i') {
                JSRW_GOT_MATCH(16) /* if */
            }
            if (JSRW_AT(0) == 'o') {
                JSRW_GOT_MATCH(48) /* of */
            }
            JSRW_NO_MATCH()
          case 'n':
            if (JSRW_AT(0)=='i') {
                JSRW_GOT_MATCH(17) /* in */
            }
            JSRW_NO_MATCH()
          case 'o':
            if (JSRW_AT(0)=='d') {
                JSRW_GOT_MATCH(11) /* do */
            }
            JSRW_NO_MATCH()
          case 's':
            if (JSRW_AT(0)=='a') {
                JSRW_GOT_MATCH(42) /* as */
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 3:
        switch (JSRW_AT(2)) {
          case 'r':
            if (JSRW_AT(0) == 'f') {
                if (JSRW_AT(1)=='o') {
                    JSRW_GOT_MATCH(14) /* for */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(0) == 'v') {
                if (JSRW_AT(1)=='a') {
                    JSRW_GOT_MATCH(26) /* var */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
          case 't':
            if (JSRW_AT(1) == 'e') {
                if (JSRW_AT(0) == 'g') {
                    JSRW_GOT_MATCH(46) /* get */
                }
                if (JSRW_AT(0) == 'l') {
                    JSRW_GOT_MATCH(47) /* let */
                }
                if (JSRW_AT(0) == 's') {
                    JSRW_GOT_MATCH(49) /* set */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
          case 'w':
            if (JSRW_AT(0)=='n' && JSRW_AT(1)=='e') {
                JSRW_GOT_MATCH(19) /* new */
            }
            JSRW_NO_MATCH()
          case 'y':
            if (JSRW_AT(0)=='t' && JSRW_AT(1)=='r') {
                JSRW_GOT_MATCH(24) /* try */
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 4:
        switch (JSRW_AT(2)) {
          case 'i':
            if (JSRW_AT(0) == 't') {
                if (JSRW_AT(3)=='s' && JSRW_AT(1)=='h') {
                    JSRW_GOT_MATCH(22) /* this */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(0) == 'v') {
                if (JSRW_AT(3)=='d' && JSRW_AT(1)=='o') {
                    JSRW_GOT_MATCH(27) /* void */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
          case 'l':
            if (JSRW_AT(0)=='n' && JSRW_AT(1)=='u' && JSRW_AT(3)=='l') {
                JSRW_GOT_MATCH(2) /* null */
            }
            JSRW_NO_MATCH()
          case 'o':
            if (JSRW_AT(0)=='f' && JSRW_AT(1)=='r' && JSRW_AT(3)=='m') {
                JSRW_GOT_MATCH(45) /* from */
            }
            JSRW_NO_MATCH()
          case 's':
            if (JSRW_AT(1) == 'a') {
                if (JSRW_AT(0)=='c' && JSRW_AT(3)=='e') {
                    JSRW_GOT_MATCH(4) /* case */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(1) == 'l') {
                if (JSRW_AT(0)=='e' && JSRW_AT(3)=='e') {
                    JSRW_GOT_MATCH(12) /* else */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
          case 't':
            if (JSRW_AT(0)=='w' && JSRW_AT(1)=='i' && JSRW_AT(3)=='h') {
                JSRW_GOT_MATCH(29) /* with */
            }
            JSRW_NO_MATCH()
          case 'u':
            if (JSRW_AT(0) == 'e') {
                if (JSRW_AT(3)=='m' && JSRW_AT(1)=='n') {
                    JSRW_GOT_MATCH(35) /* enum */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(0) == 't') {
                if (JSRW_AT(3)=='e' && JSRW_AT(1)=='r') {
                    JSRW_GOT_MATCH(1) /* true */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 5:
        switch (JSRW_AT(3)) {
          case 'a':
            if (JSRW_AT(0)=='b' && JSRW_AT(1)=='r' && JSRW_AT(2)=='e' && JSRW_AT(4)=='k') {
                JSRW_GOT_MATCH(3) /* break */
            }
            JSRW_NO_MATCH()
          case 'c':
            if (JSRW_AT(0)=='c' && JSRW_AT(1)=='a' && JSRW_AT(2)=='t' && JSRW_AT(4)=='h') {
                JSRW_GOT_MATCH(5) /* catch */
            }
            JSRW_NO_MATCH()
          case 'e':
            if (JSRW_AT(0)=='s' && JSRW_AT(1)=='u' && JSRW_AT(2)=='p' && JSRW_AT(4)=='r') {
                JSRW_GOT_MATCH(34) /* super */
            }
            JSRW_NO_MATCH()
          case 'i':
            if (JSRW_AT(0)=='a' && JSRW_AT(1)=='w' && JSRW_AT(2)=='a' && JSRW_AT(4)=='t') {
                JSRW_GOT_MATCH(44) /* await */
            }
            JSRW_NO_MATCH()
          case 'l':
            if (JSRW_AT(0) == 'w') {
                if (JSRW_AT(4)=='e' && JSRW_AT(1)=='h' && JSRW_AT(2)=='i') {
                    JSRW_GOT_MATCH(28) /* while */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(0) == 'y') {
                if (JSRW_AT(4)=='d' && JSRW_AT(1)=='i' && JSRW_AT(2)=='e') {
                    JSRW_GOT_MATCH(52) /* yield */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
          case 'n':
            if (JSRW_AT(0)=='a' && JSRW_AT(1)=='s' && JSRW_AT(2)=='y' && JSRW_AT(4)=='c') {
                JSRW_GOT_MATCH(43) /* async */
            }
            JSRW_NO_MATCH()
          case 'o':
            if (JSRW_AT(0)=='t' && JSRW_AT(1)=='h' && JSRW_AT(2)=='r' && JSRW_AT(4)=='w') {
                JSRW_GOT_MATCH(23) /* throw */
            }
            JSRW_NO_MATCH()
          case 's':
            if (JSRW_AT(0) == 'c') {
                if (JSRW_AT(4) == 's') {
                    if (JSRW_AT(2)=='a' && JSRW_AT(1)=='l') {
                        JSRW_GOT_MATCH(32) /* class */
                    }
                    JSRW_NO_MATCH()
                }
                if (JSRW_AT(4) == 't') {
                    if (JSRW_AT(2)=='n' && JSRW_AT(1)=='o') {
                        JSRW_GOT_MATCH(6) /* const */
                    }
                    JSRW_NO_MATCH()
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(0) == 'f') {
                if (JSRW_AT(4)=='e' && JSRW_AT(1)=='a' && JSRW_AT(2)=='l') {
                    JSRW_GOT_MATCH(0) /* false */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 6:
        switch (JSRW_AT(0)) {
          case 'd':
            JSRW_TEST_GUESS(10) /* delete */
          case 'e':
            JSRW_TEST_GUESS(31) /* export */
          case 'i':
            JSRW_TEST_GUESS(30) /* import */
          case 'p':
            JSRW_TEST_GUESS(41) /* public */
          case 'r':
            JSRW_TEST_GUESS(20) /* return */
          case 's':
            if (JSRW_AT(1) == 't') {
                if (JSRW_AT(5)=='c' && JSRW_AT(4)=='i' && JSRW_AT(2)=='a' && JSRW_AT(3)=='t') {
                    JSRW_GOT_MATCH(50) /* static */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(1) == 'w') {
                if (JSRW_AT(5)=='h' && JSRW_AT(4)=='c' && JSRW_AT(2)=='i' && JSRW_AT(3)=='t') {
                    JSRW_GOT_MATCH(21) /* switch */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
          case 't':
            if (JSRW_AT(5) == 'f') {
                if (JSRW_AT(4)=='o' && JSRW_AT(1)=='y' && JSRW_AT(2)=='p' && JSRW_AT(3)=='e') {
                    JSRW_GOT_MATCH(25) /* typeof */
                }
                JSRW_NO_MATCH()
            }
            if (JSRW_AT(5) == 't') {
                if (JSRW_AT(4)=='e' && JSRW_AT(1)=='a' && JSRW_AT(2)=='r' && JSRW_AT(3)=='g') {
                    JSRW_GOT_MATCH(51) /* target */
                }
                JSRW_NO_MATCH()
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 7:
        switch (JSRW_AT(0)) {
          case 'd':
            JSRW_TEST_GUESS(9) /* default */
          case 'e':
            JSRW_TEST_GUESS(33) /* extends */
          case 'f':
            JSRW_TEST_GUESS(13) /* finally */
          case 'p':
            if (JSRW_AT(1) == 'a') {
                JSRW_TEST_GUESS(38) /* package */
            }
            if (JSRW_AT(1) == 'r') {
                JSRW_TEST_GUESS(39) /* private */
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 8:
        if (JSRW_AT(2) == 'b') {
            JSRW_TEST_GUESS(8) /* debugger */
        }
        if (JSRW_AT(2) == 'n') {
            if (JSRW_AT(0) == 'c') {
                JSRW_TEST_GUESS(7) /* continue */
            }
            if (JSRW_AT(0) == 'f') {
                JSRW_TEST_GUESS(15) /* function */
            }
            JSRW_NO_MATCH()
        }
        JSRW_NO_MATCH()
      case 9:
        if (JSRW_AT(0) == 'i') {
            JSRW_TEST_GUESS(37) /* interface */
        }
        if (JSRW_AT(0) == 'p') {
            JSRW_TEST_GUESS(40) /* protected */
        }
        JSRW_NO_MATCH()
      case 10:
        if (JSRW_AT(1) == 'm') {
            JSRW_TEST_GUESS(36) /* implements */
        }
        if (JSRW_AT(1) == 'n') {
            JSRW_TEST_GUESS(18) /* instanceof */
        }
        JSRW_NO_MATCH()
    }
    JSRW_NO_MATCH()

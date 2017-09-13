    /*
     * Generating switch for the list of 45 entries:
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
     * static
     * yield
     * let
     */
    switch (JSKW_LENGTH()) {
      case 2:
        if (JSKW_AT(0) == 'd') {
            if (JSKW_AT(1)=='o') {
                JSKW_GOT_MATCH(11) /* do */
            }
            JSKW_NO_MATCH()
        }
        if (JSKW_AT(0) == 'i') {
            if (JSKW_AT(1) == 'f') {
                JSKW_GOT_MATCH(16) /* if */
            }
            if (JSKW_AT(1) == 'n') {
                JSKW_GOT_MATCH(17) /* in */
            }
            JSKW_NO_MATCH()
        }
        JSKW_NO_MATCH()
      case 3:
        switch (JSKW_AT(2)) {
          case 'r':
            if (JSKW_AT(0) == 'f') {
                if (JSKW_AT(1)=='o') {
                    JSKW_GOT_MATCH(14) /* for */
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(0) == 'v') {
                if (JSKW_AT(1)=='a') {
                    JSKW_GOT_MATCH(26) /* var */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
          case 't':
            if (JSKW_AT(0)=='l' && JSKW_AT(1)=='e') {
                JSKW_GOT_MATCH(44) /* let */
            }
            JSKW_NO_MATCH()
          case 'w':
            if (JSKW_AT(0)=='n' && JSKW_AT(1)=='e') {
                JSKW_GOT_MATCH(19) /* new */
            }
            JSKW_NO_MATCH()
          case 'y':
            if (JSKW_AT(0)=='t' && JSKW_AT(1)=='r') {
                JSKW_GOT_MATCH(24) /* try */
            }
            JSKW_NO_MATCH()
        }
        JSKW_NO_MATCH()
      case 4:
        switch (JSKW_AT(2)) {
          case 'i':
            if (JSKW_AT(0) == 't') {
                if (JSKW_AT(3)=='s' && JSKW_AT(1)=='h') {
                    JSKW_GOT_MATCH(22) /* this */
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(0) == 'v') {
                if (JSKW_AT(3)=='d' && JSKW_AT(1)=='o') {
                    JSKW_GOT_MATCH(27) /* void */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
          case 'l':
            if (JSKW_AT(0)=='n' && JSKW_AT(1)=='u' && JSKW_AT(3)=='l') {
                JSKW_GOT_MATCH(2) /* null */
            }
            JSKW_NO_MATCH()
          case 's':
            if (JSKW_AT(1) == 'a') {
                if (JSKW_AT(0)=='c' && JSKW_AT(3)=='e') {
                    JSKW_GOT_MATCH(4) /* case */
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(1) == 'l') {
                if (JSKW_AT(0)=='e' && JSKW_AT(3)=='e') {
                    JSKW_GOT_MATCH(12) /* else */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
          case 't':
            if (JSKW_AT(0)=='w' && JSKW_AT(1)=='i' && JSKW_AT(3)=='h') {
                JSKW_GOT_MATCH(29) /* with */
            }
            JSKW_NO_MATCH()
          case 'u':
            if (JSKW_AT(0) == 'e') {
                if (JSKW_AT(3)=='m' && JSKW_AT(1)=='n') {
                    JSKW_GOT_MATCH(35) /* enum */
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(0) == 't') {
                if (JSKW_AT(3)=='e' && JSKW_AT(1)=='r') {
                    JSKW_GOT_MATCH(1) /* true */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
        }
        JSKW_NO_MATCH()
      case 5:
        switch (JSKW_AT(3)) {
          case 'a':
            if (JSKW_AT(0)=='b' && JSKW_AT(1)=='r' && JSKW_AT(2)=='e' && JSKW_AT(4)=='k') {
                JSKW_GOT_MATCH(3) /* break */
            }
            JSKW_NO_MATCH()
          case 'c':
            if (JSKW_AT(0)=='c' && JSKW_AT(1)=='a' && JSKW_AT(2)=='t' && JSKW_AT(4)=='h') {
                JSKW_GOT_MATCH(5) /* catch */
            }
            JSKW_NO_MATCH()
          case 'e':
            if (JSKW_AT(0)=='s' && JSKW_AT(1)=='u' && JSKW_AT(2)=='p' && JSKW_AT(4)=='r') {
                JSKW_GOT_MATCH(34) /* super */
            }
            JSKW_NO_MATCH()
          case 'l':
            if (JSKW_AT(0) == 'w') {
                if (JSKW_AT(4)=='e' && JSKW_AT(1)=='h' && JSKW_AT(2)=='i') {
                    JSKW_GOT_MATCH(28) /* while */
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(0) == 'y') {
                if (JSKW_AT(4)=='d' && JSKW_AT(1)=='i' && JSKW_AT(2)=='e') {
                    JSKW_GOT_MATCH(43) /* yield */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
          case 'o':
            if (JSKW_AT(0)=='t' && JSKW_AT(1)=='h' && JSKW_AT(2)=='r' && JSKW_AT(4)=='w') {
                JSKW_GOT_MATCH(23) /* throw */
            }
            JSKW_NO_MATCH()
          case 's':
            if (JSKW_AT(0) == 'c') {
                if (JSKW_AT(4) == 's') {
                    if (JSKW_AT(2)=='a' && JSKW_AT(1)=='l') {
                        JSKW_GOT_MATCH(32) /* class */
                    }
                    JSKW_NO_MATCH()
                }
                if (JSKW_AT(4) == 't') {
                    if (JSKW_AT(2)=='n' && JSKW_AT(1)=='o') {
                        JSKW_GOT_MATCH(6) /* const */
                    }
                    JSKW_NO_MATCH()
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(0) == 'f') {
                if (JSKW_AT(4)=='e' && JSKW_AT(1)=='a' && JSKW_AT(2)=='l') {
                    JSKW_GOT_MATCH(0) /* false */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
        }
        JSKW_NO_MATCH()
      case 6:
        switch (JSKW_AT(0)) {
          case 'd':
            JSKW_TEST_GUESS(10) /* delete */
          case 'e':
            JSKW_TEST_GUESS(31) /* export */
          case 'i':
            JSKW_TEST_GUESS(30) /* import */
          case 'p':
            JSKW_TEST_GUESS(41) /* public */
          case 'r':
            JSKW_TEST_GUESS(20) /* return */
          case 's':
            if (JSKW_AT(1) == 't') {
                if (JSKW_AT(5)=='c' && JSKW_AT(4)=='i' && JSKW_AT(2)=='a' && JSKW_AT(3)=='t') {
                    JSKW_GOT_MATCH(42) /* static */
                }
                JSKW_NO_MATCH()
            }
            if (JSKW_AT(1) == 'w') {
                if (JSKW_AT(5)=='h' && JSKW_AT(4)=='c' && JSKW_AT(2)=='i' && JSKW_AT(3)=='t') {
                    JSKW_GOT_MATCH(21) /* switch */
                }
                JSKW_NO_MATCH()
            }
            JSKW_NO_MATCH()
          case 't':
            JSKW_TEST_GUESS(25) /* typeof */
        }
        JSKW_NO_MATCH()
      case 7:
        switch (JSKW_AT(0)) {
          case 'd':
            JSKW_TEST_GUESS(9) /* default */
          case 'e':
            JSKW_TEST_GUESS(33) /* extends */
          case 'f':
            JSKW_TEST_GUESS(13) /* finally */
          case 'p':
            if (JSKW_AT(1) == 'a') {
                JSKW_TEST_GUESS(38) /* package */
            }
            if (JSKW_AT(1) == 'r') {
                JSKW_TEST_GUESS(39) /* private */
            }
            JSKW_NO_MATCH()
        }
        JSKW_NO_MATCH()
      case 8:
        if (JSKW_AT(2) == 'b') {
            JSKW_TEST_GUESS(8) /* debugger */
        }
        if (JSKW_AT(2) == 'n') {
            if (JSKW_AT(0) == 'c') {
                JSKW_TEST_GUESS(7) /* continue */
            }
            if (JSKW_AT(0) == 'f') {
                JSKW_TEST_GUESS(15) /* function */
            }
            JSKW_NO_MATCH()
        }
        JSKW_NO_MATCH()
      case 9:
        if (JSKW_AT(0) == 'i') {
            JSKW_TEST_GUESS(37) /* interface */
        }
        if (JSKW_AT(0) == 'p') {
            JSKW_TEST_GUESS(40) /* protected */
        }
        JSKW_NO_MATCH()
      case 10:
        if (JSKW_AT(1) == 'n') {
            JSKW_TEST_GUESS(18) /* instanceof */
        }
        if (JSKW_AT(1) == 'm') {
            JSKW_TEST_GUESS(36) /* implements */
        }
        JSKW_NO_MATCH()
    }
    JSKW_NO_MATCH()

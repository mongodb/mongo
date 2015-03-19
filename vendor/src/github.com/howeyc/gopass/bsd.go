// +build freebsd openbsd netbsd

package gopass

/*
#include <termios.h>
#include <unistd.h>
#include <stdio.h>

int getch() {
        int ch;
        struct termios t_old, t_new;

        tcgetattr(STDIN_FILENO, &t_old);
        t_new = t_old;
        t_new.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &t_new);

        ch = getchar();

        tcsetattr(STDIN_FILENO, TCSANOW, &t_old);
        return ch;
}
*/
import "C"

func getch() byte {
	return byte(C.getch())
}

#include <stdlib.h>

#include <unistd.h> // TODO
#include <fcntl.h> // TODO
#include <wt_internal.h>

static void fail(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

static void
fail(int ret) {
	fprintf(stderr,
	    "%s: %d (%s)\n",
	    "wt2336_fileop_basic", ret, wiredtiger_strerror(ret));
	exit(ret);
}

#define	SEPARATOR "--------------"

int
main(int argc, char *argv[])
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;

	(void)argc;
	(void)argv;
	fprintf(stderr, SEPARATOR "wiredtiger_open\n");
	if ((ret = wiredtiger_open(".", NULL, "create", &conn)) != 0)
		fail(ret);

	usleep(100);
	fflush(stderr);
	fprintf(stderr, SEPARATOR "open_session\n");
	fflush(stderr);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fail(ret);

	usleep(100);
	fflush(stderr);
	fprintf(stderr, SEPARATOR "create\n");
	fflush(stderr);

	if ((ret = session->create(
	    session, "table:hello", "key_format=S,value_format=S")) != 0)
		fail(ret);

	usleep(100);
	fprintf(stderr, SEPARATOR "rename\n");

	if ((ret = session->rename(
	    session, "table:hello", "table:world", NULL)) != 0)
		fail(ret);

	fflush(stdout);
	fprintf(stderr, SEPARATOR "drop\n");
	fflush(stdout);

	if ((ret = session->drop(session, "table:world", NULL)) != 0)
		fail(ret);

	fprintf(stderr, SEPARATOR "WT_CONNECTION::close\n");

	if ((ret = conn->close(conn, NULL)) != 0)
		fail(ret);

	return (0);
}

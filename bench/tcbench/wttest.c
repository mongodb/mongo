/*************************************************************************************************
 * Microbenchmark of WiredTiger
 * Designed to be comparable with the TokyoCabinet "bros" tests.
 *************************************************************************************************/


#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiredtiger.h>

#undef TRUE
#define TRUE           1                 /* boolean true */
#undef FALSE
#define FALSE          0                 /* boolean false */

#define RECBUFSIZ      32                /* buffer for records */


/* global variables */
const char *progname;                    /* program name */
int showprgr;                            /* whether to show progression */


/* function prototypes */
int main(int argc, char **argv);
void usage(void);
int setup(char *name, const char *kf, const char *vf, const char *cconfig, WT_CURSOR **cursor);
int teardown(void);
int runwrite(int argc, char **argv);
int runread(int argc, char **argv);
int runvlcswrite(int argc, char **argv);
int runvlcsread(int argc, char **argv);
int runflcswrite(int argc, char **argv);
int runflcsread(int argc, char **argv);
int myrand(void);
int dowrite(char *name, int rnum, int bulk, int rnd);
int doread(char *name, int rnum, int rnd);
int dovlcswrite(char *name, int rnum, int bulk, int rnd);
int dovlcsread(char *name, int rnum, int rnd);
int doflcswrite(char *name, int rnum, int bulk, int rnd);
int doflcsread(char *name, int rnum, int rnd);


/* main routine */
int main(int argc, char **argv){
  int rv;
  progname = argv[0];
  showprgr = TRUE;
  if(getenv("HIDEPRGR"))
    showprgr = FALSE;
  srand48(1978);
  if(argc < 2)
    usage();
  rv = 0;
  if(!strcmp(argv[1], "write")){
    rv = runwrite(argc, argv);
  } else if(!strcmp(argv[1], "read")){
    rv = runread(argc, argv);
  } else if(!strcmp(argv[1], "vlcswrite")){
    rv = runvlcswrite(argc, argv);
  } else if(!strcmp(argv[1], "vlcsread")){
    rv = runvlcsread(argc, argv);
  } else if(!strcmp(argv[1], "flcswrite")){
    rv = runflcswrite(argc, argv);
  } else if(!strcmp(argv[1], "flcsread")){
    rv = runflcsread(argc, argv);
  } else {
    usage();
  }
  return rv;
}


/* print the usage and exit */
void usage(void){
  fprintf(stderr, "%s: test cases for WiredTiger\n", progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s write [-bulk|-rnd] name rnum\n", progname);
  fprintf(stderr, "  %s read [-rnd] name rnum\n", progname);
  fprintf(stderr, "  %s vlcswrite [-bulk|-rnd] name rnum\n", progname);
  fprintf(stderr, "  %s vlcsread [-rnd] name rnum\n", progname);
  fprintf(stderr, "  %s flcswrite [-bulk|-rnd] name rnum\n", progname);
  fprintf(stderr, "  %s flcsread [-rnd] name rnum\n", progname);
  fprintf(stderr, "\n");
  exit(1);
}


/* parse arguments of write command */
int runwrite(int argc, char **argv){
  char *name, *rstr;
  int bulk, i, rnd, rnum, rv;
  name = NULL;
  rstr = NULL;
  bulk = rnd = FALSE;
  rnum = 0;
  for(i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!name && !strcmp(argv[i], "-bulk"))
        bulk = TRUE;
      else if(!name && !strcmp(argv[i], "-rnd"))
        rnd = TRUE;
      else
        usage();
    } else if(!name){
      name = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !rstr)
    usage();
  rnum = atoi(rstr);
  if(rnum < 1)
    usage();
  rv = dowrite(name, rnum, bulk, rnd);
  return rv;
}


/* parse arguments of read command */
int runread(int argc, char **argv){
  char *name, *rstr;
  int i, rnd, rnum, rv;
  name = NULL;
  rstr = NULL;
  rnd = FALSE;
  rnum = 0;
  for(i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!name && !strcmp(argv[i], "-rnd"))
        rnd = TRUE;
      else
        usage();
    } else if(!name){
      name = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !rstr)
    usage();
  rnum = atoi(rstr);
  if(rnum < 1)
    usage();
  rv = doread(name, rnum, rnd);
  return rv;
}


/* parse arguments of write command */
int runvlcswrite(int argc, char **argv){
  char *name, *rstr;
  int bulk, i, rnd, rnum, rv;
  name = NULL;
  rstr = NULL;
  rnum = 0;
  bulk = rnd = FALSE;
  for(i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!name && !strcmp(argv[i], "-bulk"))
        bulk = TRUE;
      else if(!name && !strcmp(argv[i], "-rnd"))
        rnd = TRUE;
      else
        usage();
    } else if(!name){
      name = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !rstr)
    usage();
  rnum = atoi(rstr);
  if(rnum < 1)
    usage();
  rv = dovlcswrite(name, rnum, bulk, rnd);
  return rv;
}


/* parse arguments of read command */
int runvlcsread(int argc, char **argv){
  char *name, *rstr;
  int i, rnd, rnum, rv;
  name = NULL;
  rstr = NULL;
  rnum = 0;
  rnd = FALSE;
  for(i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!name && !strcmp(argv[i], "-rnd"))
        rnd = TRUE;
      else
        usage();
    } else if(!name){
      name = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !rstr)
    usage();
  rnum = atoi(rstr);
  if(rnum < 1)
    usage();
  rv = dovlcsread(name, rnum, rnd);
  return rv;
}


/* parse arguments of write command */
int runflcswrite(int argc, char **argv){
  char *name, *rstr;
  int bulk, i, rnd, rnum, rv;
  name = NULL;
  rstr = NULL;
  rnum = 0;
  bulk = rnd = FALSE;
  for(i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!name && !strcmp(argv[i], "-bulk"))
        bulk = TRUE;
      else if(!name && !strcmp(argv[i], "-rnd"))
        rnd = TRUE;
      else
        usage();
    } else if(!name){
      name = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !rstr)
    usage();
  rnum = atoi(rstr);
  if(rnum < 1)
    usage();
  rv = doflcswrite(name, rnum, bulk, rnd);
  return rv;
}


/* parse arguments of read command */
int runflcsread(int argc, char **argv){
  char *name, *rstr;
  int i, rnd, rnum, rv;
  name = NULL;
  rstr = NULL;
  rnum = 0;
  rnd = FALSE;
  for(i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!name && !strcmp(argv[i], "-rnd"))
        rnd = TRUE;
      else
        usage();
    } else if(!name){
      name = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !rstr)
    usage();
  rnum = atoi(rstr);
  if(rnum < 1)
    usage();
  rv = doflcsread(name, rnum, rnd);
  return rv;
}


/* pseudo random number generator */
int myrand(void){
  static int cnt = 0;
  return (int)((lrand48() + cnt++) & 0x7FFFFFFF);
}

WT_CONNECTION *conn;

int setup(char *name, const char *kf, const char *vf, const char *cconfig, WT_CURSOR **cursor){
  WT_SESSION *session;
  int creating, ret;
  char tconfig[64];

  creating = (kf != NULL);

  if((ret = wiredtiger_open(NULL, NULL, "create", &conn) != 0) ||
    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
    return ret;

  /* If we get a configuration, create the table. */
  if(creating) {
    (void)session->drop(session, name, "force");
    snprintf(tconfig, sizeof(tconfig), "key_format=%s,value_format=%s", kf, vf);
    if ((ret = session->create(session, name, tconfig)) != 0)
      return ret;
  }

  return session->open_cursor(session, name, NULL, cconfig, cursor);
}

int teardown(void){
  int ret = 0;
  if (conn != NULL) {
    ret = conn->close(conn, NULL);
    conn = NULL;
  }
  return ret;
}

/* perform write command */
int dowrite(char *name, int rnum, int bulk, int rnd){
  WT_CURSOR *c;
  WT_ITEM key, value;
  int i, err, len;
  char buf[RECBUFSIZ];
  if(showprgr)
    printf("<Write Test of Row Store>\n  name=%s  rnum=%d\n\n", name, rnum);
  /* open a database */
  if(setup(name, "u", "u", bulk ? "bulk" : NULL, &c) != 0) {
    fprintf(stderr, "create failed\n");
    (void)teardown();
    return 1;
  }
  err = FALSE;
  key.data = value.data = buf;
  key.size = value.size = 8;
  /* loop for each record */
  for(i = 1; i <= rnum; i++){
    /* store a record */
    len = sprintf(buf, "%08d", rnd ? myrand() % rnum + 1 : i);
    c->set_key(c, &key);
    c->set_value(c, &value);
    if((err = c->insert(c)) != 0 && err != WT_DUPLICATE_KEY) {
      fprintf(stderr, "insert failed\n");
      break;
    }
    /* print progression */
    if(showprgr && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0){
        printf(" (%08d)\n", i);
        fflush(stdout);
      }
    }
  }
  /* close the database */
  if(teardown() != 0) {
    fprintf(stderr, "close failed\n");
    return 1;
  }
  if(showprgr && !err)
    printf("ok\n\n");
  return err ? 1 : 0;
}


/* perform read command */
int doread(char *name, int rnum, int rnd){
  WT_CURSOR *c;
  WT_ITEM key, value;
  int i, err, len;
  char buf[RECBUFSIZ];
  if(showprgr)
    printf("<Read Test of Row Store>\n  name=%s  rnum=%d\n\n", name, rnum);
  /* open a database */
  if(setup(name, NULL, NULL, NULL, &c) != 0){
    fprintf(stderr, "open failed\n");
    return 1;
  }
  err = FALSE;
  key.data = value.data = buf;
  key.size = value.size = 8;
  /* loop for each record */
  for(i = 1; i <= rnum; i++){
    /* store a record */
    len = sprintf(buf, "%08d", rnd ? myrand() % rnum + 1 : i);
    c->set_key(c, &key);
    if(c->search(c) != 0){
      fprintf(stderr, "search failed\n");
      err = TRUE;
      break;
    }
    /* Include the cost of getting the value. */
    c->get_value(c, &value);
    /* print progression */
    if(showprgr && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0){
        printf(" (%08d)\n", i);
        fflush(stdout);
      }
    }
  }
  /* close the database */
  if(teardown() != 0) {
    fprintf(stderr, "close failed\n");
    return 1;
  }
  if(showprgr && !err)
    printf("ok\n\n");
  return err ? 1 : 0;
}

/* perform write command */
int dovlcswrite(char *name, int rnum, int bulk, int rnd){
  WT_CURSOR *c;
  WT_ITEM value;
  int i, err, len;
  char buf[RECBUFSIZ];
  if(showprgr)
    printf("<Write Test of var-length Column Store>\n  name=%s  rnum=%d\n\n", name, rnum);
  /* open a database */
  if(setup(name, "r", "u", bulk ? "bulk" : NULL, &c) != 0) {
    fprintf(stderr, "create failed\n");
    (void)teardown();
    return 1;
  }
  err = FALSE;
  value.data = buf;
  value.size = 8;
  /* loop for each record */
  for(i = 1; i <= rnum; i++){
    /* store a record */
    len = sprintf(buf, "%08d", i);
    c->set_key(c, (uint64_t)(rnd ? myrand() % rnum + 1 : i));
    c->set_value(c, &value);
    if((err = c->insert(c)) != 0 && err != WT_DUPLICATE_KEY) {
      fprintf(stderr, "insert failed\n");
      break;
    }
    /* print progression */
    if(showprgr && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0){
        printf(" (%08d)\n", i);
        fflush(stdout);
      }
    }
  }
  /* close the database */
  if(teardown() != 0) {
    fprintf(stderr, "close failed\n");
    return 1;
  }
  if(showprgr && !err)
    printf("ok\n\n");
  return err ? 1 : 0;
}


/* perform read command */
int dovlcsread(char *name, int rnum, int rnd){
  WT_CURSOR *c;
  int i, err;
  WT_ITEM value;
  if(showprgr)
    printf("<Read Test of var-length Column Store>\n  name=%s  rnum=%d\n\n", name, rnum);
  /* open a database */
  if(setup(name, NULL, NULL, NULL, &c) != 0){
    fprintf(stderr, "open failed\n");
    return 1;
  }
  err = FALSE;
  /* loop for each record */
  for(i = 1; i <= rnum; i++){
    c->set_key(c, (uint64_t)(rnd ? myrand() % rnum + 1 : i));
    if(c->search(c) != 0){
      fprintf(stderr, "search failed\n");
      err = TRUE;
      break;
    }
    /* Include the cost of getting the value. */
    c->get_value(c, &value);
    /* print progression */
    if(showprgr && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0){
        printf(" (%08d)\n", i);
        fflush(stdout);
      }
    }
  }
  /* close the database */
  if(teardown() != 0) {
    fprintf(stderr, "close failed\n");
    return 1;
  }
  if(showprgr && !err)
    printf("ok\n\n");
  return err ? 1 : 0;
}


/* perform write command */
int doflcswrite(char *name, int rnum, int bulk, int rnd){
  WT_CURSOR *c;
  uint8_t value;
  int i, err, len;
  char buf[RECBUFSIZ];
  if(showprgr)
    printf("<Write Test of var-length Column Store>\n  name=%s  rnum=%d\n\n", name, rnum);
  /* open a database */
  if(setup(name, "r", "8t", bulk ? "bulk" : NULL, &c) != 0) {
    fprintf(stderr, "create failed\n");
    (void)teardown();
    return 1;
  }
  err = FALSE;
  value = 42;
  /* loop for each record */
  for(i = 1; i <= rnum; i++){
    /* store a record */
    len = sprintf(buf, "%08d", i);
    c->set_key(c, (uint64_t)(rnd ? myrand() % rnum + 1 : i));
    c->set_value(c, value);
    if((err = c->insert(c)) != 0 && err != WT_DUPLICATE_KEY) {
      fprintf(stderr, "insert failed\n");
      break;
    }
    /* print progression */
    if(showprgr && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0){
        printf(" (%08d)\n", i);
        fflush(stdout);
      }
    }
  }
  /* close the database */
  if(teardown() != 0) {
    fprintf(stderr, "close failed\n");
    return 1;
  }
  if(showprgr && !err)
    printf("ok\n\n");
  return err ? 1 : 0;
}


/* perform read command */
int doflcsread(char *name, int rnum, int rnd){
  WT_CURSOR *c;
  uint8_t value;
  int i, err;
  if(showprgr)
    printf("<Read Test of var-length Column Store>\n  name=%s  rnum=%d\n\n", name, rnum);
  /* open a database */
  if(setup(name, NULL, NULL, NULL, &c) != 0){
    fprintf(stderr, "open failed\n");
    return 1;
  }
  err = FALSE;
  /* loop for each record */
  for(i = 1; i <= rnum; i++){
    c->set_key(c, (uint64_t)(rnd ? myrand() % rnum + 1 : i));
    if(c->search(c) != 0){
      fprintf(stderr, "search failed\n");
      err = TRUE;
      break;
    }
    /* Include the cost of getting the value. */
    c->get_value(c, &value);
    /* print progression */
    if(showprgr && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0){
        printf(" (%08d)\n", i);
        fflush(stdout);
      }
    }
  }
  /* close the database */
  if(teardown() != 0) {
    fprintf(stderr, "close failed\n");
    return 1;
  }
  if(showprgr && !err)
    printf("ok\n\n");
  return err ? 1 : 0;
}

/* END OF FILE */

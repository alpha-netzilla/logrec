#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <termios.h>
#include <time.h>
#include <fcntl.h>
#include <paths.h>
#include <termios.h>
#include <unistd.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <err.h>
#include <errno.h>
#include <poll.h>
#include "all-io.h"


#if defined(HAVE_LIBUTIL) && defined(HAVE_PTY_H)
#include <pty.h>
#include <utmp.h>
#endif


#ifdef HAVE_LIBUTEMPTER
# include <utempter.h>
#endif

#define DEF_EOF '\4' /* ^D */
#define DEFAULT_OUTPUT "typescript"
#define DEFAULT_OUTPUT "typescript"
#define PACKAGE_STRING "util-linux 2.26.2"
#define UTIL_LINUX_VERSION ("%s\n"), PACKAGE_STRING
#define SEG_SIZE 1

#if !HAVE_LIBUTIL || !HAVE_PTY_H
char  line[] = "/dev/ptyXX";
#endif


int aflg = 0;
char *cflg = NULL;
int eflg = 0;
int fflg = 0;
int qflg = 0;
int tflg = 0;
int forceflg = 0;
int isterm;
char *fname;
FILE *fnamep;
FILE  *timingp;
char *shell;
int master;
int slave;
int die;
int resized;
int childstatus;
int seg_id;
char *stopflagp;
pid_t child;
pid_t subchild;
sigset_t block_mask, unblock_mask;

struct  termios term_default;
struct  winsize win_default;


void getpty(void);
void fixtty(void);
void resize(int);

void read_from_terminal_write_to_masterpty(void);
void read_from_masterpty_write_to_terminal(void);
void doshell(void);

static void wait_for_empty_fd(int);

char *get_time(char *);
void finish(int);
void sig_finish(int);
void done(void);
void fail(void);
void die_if_link(char *);
static void usage(FILE *);


/*
 * -t prints time delays as floating point numbers
 * The example program (scriptreplay) that we provide to handle this
 * timing output is a perl script, and does not handle numbers in
 * locale format (not even when "use locale;" is added).
 * So, since these numbers are not for human consumption, it seems
 * easiest to set LC_NUMERIC here.
*/


int main(int argc, char **argv){

  setlocale(LC_ALL, "");
  setlocale(LC_NUMERIC, "C"); /* see comment above */
  //bindtextdomain(PACKAGE, LOCALEDIR);
  //textdomain(PACKAGE);


  enum { FORCE_OPTION = CHAR_MAX + 1 };

  static const struct option longopts[] = {
    { "append",  no_argument,       NULL, 'a' },
    { "command", required_argument, NULL, 'c' },
    { "return",  no_argument,       NULL, 'e' },
    { "flush",   no_argument,       NULL, 'f' },
    { "force",   no_argument,       NULL, FORCE_OPTION, },
    { "quiet",   no_argument,       NULL, 'q' },
    { "timing",  optional_argument, NULL, 't' },
    { "version", no_argument,       NULL, 'V' },
    { "help", no_argument,          NULL, 'h' },
    { NULL,   0, NULL, 0 }
  };


  int ch;
  while ((ch = getopt_long(argc, argv, "ac:efqt::Vh", longopts, NULL)) != -1)
    switch(ch) {
    case 'a':
      aflg = 1;
      break;
    case 'c':
      cflg = optarg;
      break;
    case 'e':
      eflg = 1;
      break;
    case 'f':
      fflg = 1;
      break;
    case FORCE_OPTION:
      forceflg = 1;
      break;
      case 'q':
      qflg = 1;
      break;
    case 't':
      if (optarg && !(timingp = fopen(optarg, "w")))
        err(EXIT_FAILURE, "cannot open %s", optarg);
        tflg = 1;
      break;
    case 'V':
      printf(UTIL_LINUX_VERSION);
      exit(EXIT_SUCCESS);
      break;
    case 'h':
      usage(stdout);
      break;
    case '?':
    default:
      usage(stderr);
    }
    argc -= optind;
    argv += optind;

    if (argc > 0) {
      fname = argv[0];
    } else {
      fname = DEFAULT_OUTPUT;
      die_if_link(fname);
    }

  if ((fnamep = fopen(fname, aflg ? "a" : "w")) == NULL) {
    warn("cannot open %s", fname);
    //fprintf(stderr,"%s\n","log file is not specified");
    fail();
  }


  shell = getenv("SHELL");
  if (shell == NULL) shell = _PATH_BSHELL;


  getpty();
  if (!qflg)
    printf("Script started, file is %s\n", fname);

  fixtty();

  // Shared Memory
  seg_id = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT|0600);
  if(seg_id == -1){
    perror("Shared memory get_error");
    shmctl(seg_id, IPC_RMID, NULL);
    exit(1);
  }

  stopflagp = shmat(seg_id, NULL, 0);
  if(stopflagp == (void *)-1){
    perror("Shared memory attach error");
    shmctl(seg_id, IPC_RMID, NULL);
    exit(1);
  }


  struct sigaction sa;

  /* setup SIGCHLD handler */
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = sig_finish;
  sigaction(SIGCHLD, &sa, NULL);

  /* init mask for SIGCHLD */
  sigprocmask(SIG_SETMASK, NULL, &block_mask);
  sigaddset(&block_mask, SIGCHLD);

  fflush(stdout);
  sigprocmask(SIG_SETMASK, &block_mask, &unblock_mask);
  child = fork();
  sigprocmask(SIG_SETMASK, &unblock_mask, NULL);


  if(child < 0){
    warn("fork failed");
    //perror("fork failed");
    fail();
  }
  // perent
  else if(child > 0){
    // Get a signal when control terminal is changed.
    //signal(SIGWINCH, resize);
    sa.sa_handler = resize;
    sigaction(SIGWINCH, &sa, NULL);

    // This process will read inputs as master pty.
    // It is able to send a messages to the foreground process of slave pty.
    read_from_terminal_write_to_masterpty();
  }
  // child
  else if(child == 0){
    sigprocmask(SIG_SETMASK, &block_mask, NULL);
    subchild = fork();
    sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

    if(subchild < 0) {warn("fork failed"); fail(); }
    // child
    else if(subchild > 0) read_from_masterpty_write_to_terminal();
    // subchild
    else if(subchild == 0) doshell();
    exit(0);
  }
  return EXIT_SUCCESS;
}


void getpty(){
  int rc;
  isterm = isatty(STDIN_FILENO);

  if (isterm) {
    if (tcgetattr(STDIN_FILENO, &term_default) != 0)
      err(EXIT_FAILURE, "failed to get terminal attributes");
      ioctl(STDIN_FILENO, TIOCGWINSZ, (char *) &win_default);
      rc = openpty(&master, &slave, NULL, &term_default, &win_default);
    } else {
      rc = openpty(&master, &slave, NULL, NULL, NULL);
  }

  if (rc < 0) {
    warn("openpty failed");
    fail();
  }
}


void fixtty(){
  struct termios term_new;

  if (!isterm) return;

  term_new = term_default;

  /* Disable the all flags of input and output set to termios struct. */
  cfmakeraw(&term_new);
  term_new.c_lflag &= ~ECHO;

  // tcsetattr(0, TCSAFLUSH, &term_new);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_new);
}


/* parent */
void resize(int dummy) {
  /* Reserve the terminal window size to win struct. */
  ioctl(0,     TIOCGWINSZ, (char *)&win_default);

  /* Set the window size of slave pty to the attributes reserved to win struct. */
  ioctl(slave, TIOCSWINSZ, (char *)&win_default);

  kill(child, SIGWINCH);
}


// parent
void read_from_terminal_write_to_masterpty() {
  ssize_t rs;
  ssize_t ws;
  int errsv = 0;

  char inbuf[BUFSIZ];
  fd_set readfds;


  /* close things irrelevant for this process */
  if (fnamep)  fclose(fnamep);
  if (timingp) fclose(timingp);
  fnamep = timingp = NULL;

  FD_ZERO(&readfds);

  /* block SIGCHLD */
  sigprocmask(SIG_SETMASK, &block_mask, &unblock_mask);

  strcpy(stopflagp, "0");
  while (die == 0) {
    FD_SET(STDIN_FILENO, &readfds);
    errno = 0;

    /* wait for input or signal (including SIGCHLD) */
    if ((rs = pselect(STDIN_FILENO + 1, &readfds, NULL, NULL, NULL, &unblock_mask)) > 0) {

      if ((rs = read(STDIN_FILENO, inbuf, BUFSIZ)) > 0) {

        if(inbuf[0] == '\r')
          strcpy(stopflagp, "1");
        else
          strcpy(stopflagp, "0");

        ws = write_all(master, inbuf, rs);
        // ws = write(master, inbuf, rs);
        if (ws < 0) {
          warn("write failed");
          fail();
        }
      }
    }

    if (rs < 0 && errno == EINTR && resized) {
      /* transmit window change information to the child */
      if (isterm) {
        ioctl(STDIN_FILENO, TIOCGWINSZ, (char *)&win_default);
        ioctl(slave, TIOCSWINSZ, (char *)&win_default);
      }
      resized = 0;
    } else if (rs <= 0 && errno != EINTR) {
        errsv = errno;
      break;
    }
  }


  /* unblock SIGCHLD */
  sigprocmask(SIG_SETMASK, &unblock_mask, NULL);

  /* To be sure that we don't miss any data */
  //close(slave); //??
  wait_for_empty_fd(slave);
  wait_for_empty_fd(master);

  if (die == 0 && rs == 0 && errsv == 0) {
    /*
     * Forward EOF from stdin (detected by read() above) to slave
     * (shell) to correctly terminate the session. It seems we have
     * to wait for empty terminal FDs otherwise EOF maybe ignored
     * (why?) and typescript is incomplete.      -- kzak Dec-2013
     *
     * We usually use this when stdin is not a tty, for example:
     * echo "ps" | script
    */

    int c = DEF_EOF;
    //if (write(master, &c, 1)) {
    if (write_all(master, &c, 1)) {
      warn ("write failed");
      fail();
    }

    /* wait for "exit" message from shell before we print "Script
     * done" in done() */
    wait_for_empty_fd(master);
  }

  if (!die) finish(1);  /* wait for children */

  done();
}


// child
void read_from_masterpty_write_to_terminal(){
  char n;
  int startflag = 1;
  ssize_t rs;
  ssize_t ws;

  int errsv = 0;
  fd_set readfds;

  char outbuf[BUFSIZ];
  struct timeval tv;
  double oldtime = time(NULL), newtime;

  char time_buf[BUFSIZ];
  char tmp_buf[100000];
  tmp_buf[0] = '\0';

  close(STDIN_FILENO);
  close(slave);

  if (tflg && !timingp)
    timingp = fdopen(STDERR_FILENO, "w");

  if (!qflg) {
    time_t tvec = time((time_t *)NULL);
    strftime(outbuf, sizeof(outbuf) , "%c\n", localtime(&tvec));
    fprintf(fnamep, "Script started on %s", outbuf);
  }

  FD_ZERO(&readfds);


  int size = 0;

  get_time(outbuf);
  fprintf(fnamep, "%s logrec started\n", outbuf);

  for (;;){
    if (die || errsv == EINTR) {
      struct pollfd fds[] = {{ .fd = master, .events = POLLIN }};
      if (poll(fds, 1, 50) <= 0) break;
    }


    /* block SIGCHLD */
    sigprocmask(SIG_SETMASK, &block_mask, &unblock_mask);

    FD_SET(master, &readfds);
    errno = 0;


    /* wait for input or signal (including SIGCHLD) */
    if ((rs = pselect(master+1, &readfds, NULL, NULL, NULL, &unblock_mask)) > 0) {

      rs = read(master, outbuf, sizeof(outbuf));
    }
    errsv = errno;

    /* unblock SIGCHLD */
    sigprocmask(SIG_SETMASK, &unblock_mask, NULL);


    if (tflg)
      gettimeofday(&tv, NULL);

    if (errsv == EINTR && rs <= 0)
      continue; /* try it again */

    /* When received a EOF(Ctrl+D), quit a loop. */
    if (rs <= 0)
      break;


    if (tflg && timingp) {
      newtime = tv.tv_sec + (double) tv.tv_usec / 1000000;
      fprintf(timingp, "%f %zd\n", newtime - oldtime, rs);
      oldtime = newtime;
    }


    if((strcmp(stopflagp, "1")) == 0){
      get_time(time_buf);
      fprintf(fnamep, "%s ", time_buf);
      fprintf(fnamep, "%s\n", tmp_buf);

      strcpy(stopflagp, "0");
      memset(tmp_buf, '\0', sizeof(tmp_buf));
      size=0;
    }else if(strstr(outbuf, "\n")){
      memset(tmp_buf, '\0', sizeof(tmp_buf));
      size=0;
    }else{
      size += rs;
      strncat(tmp_buf, outbuf, rs);
    }

    // ??
    ////if (fwrite(outbuf, 1, rs, fnamep)) {
    //if (fwrite_all(outbuf, 1, rs, fnamep)) {
    //  warn ("cannot write script file");
    //    fail();
    //}

    if (fflg) {
      fflush(fnamep);
      if (tflg && timingp)
        fflush(timingp);
    }


    ws = write(1, outbuf, rs);
    if (ws < 0 ) fail();

    memset(outbuf, '\0', sizeof(outbuf));
  }
  done();
}


// subchild
void doshell(){
  char *shname;


  //getslave();

  /* Make a subchild to be a session(process) leader. */
  /* Cut the child-subchild relationship. */
  setsid();

  /* Make a slave pseudo terminal a controll terminal. */
  ioctl(slave, TIOCSCTTY, 0);

  /* close things irrelevant for this process */
  close(master);

  /* close things irrelevant for this process */
  if (fnamep)
    fclose(fnamep);
  if (timingp)
    fclose(timingp);
  fnamep = timingp = NULL;

  dup2(slave, STDIN_FILENO);
  dup2(slave, STDOUT_FILENO);
  dup2(slave, STDERR_FILENO);
  close(slave);

  master = -1;

  /* ex) /bin/bash to /bash */
  shname = strrchr(shell, '/');

  /* ex) /bash to bash */
  if (shname)
    shname++;
  else
    shname = shell;

    /*
     * When invoked from within /etc/csh.login, script spawns a csh shell
     * that spawns programs that cannot be killed with a SIGTERM. This is
     * because csh has a documented behavior wherein it disables all
     * signals when processing the /etc/csh.* files.
     *
     * Let's restore the default behavior.
    */
  signal(SIGTERM, SIG_DFL);


  if (access(shell, X_OK) == 0) {
    if (cflg)
      execl(shell, shname, "-c", cflg, NULL);
    else
      execl(shell, shname, "-i", NULL);
  } else {
    if (cflg)
      execlp(shname, "-c", cflg, NULL);
    else
      execlp(shname, "-i", NULL);
  }

  warn("failed to execute %s", shell);
  fail();
}


char *get_time(char *ftime){
  struct tm *ptime;
  time_t t;

  t = time(NULL);
  ptime = localtime( &t );

  sprintf(ftime,"%4d-%02d-%02d %02d:%02d:%02d ",
    ptime->tm_year+1900,
    ptime->tm_mon+1,
    ptime->tm_mday,
    ptime->tm_hour,
    ptime->tm_min,
    ptime->tm_sec);

    return ftime;
}


//void sig_finish(int dummy __attribute__ ((__unused__))) {
void sig_finish(int wait) {
  finish(0);
}

void finish(int wait) {
  int status;
  pid_t pid;

  int errsv = errno;
  int options = wait ? 0 : WNOHANG;

  // while ((pid = wait(&status)) > 0)
  while ((pid = wait3(&status, options, 0)) > 0)
    if (pid == child) {
      childstatus = status;
      die = 1;
    }
}


void done() {
  time_t t;

  shmctl(seg_id, IPC_RMID, NULL);

  /* child */
  if (subchild) {
    char buf[BUFSIZ];
    get_time(buf);
    fprintf(fnamep, "%s logrec child done\n", buf);
    fclose(fnamep);
    close(master);
  }
  /* parent */
  else if (child) {
    /* Output all inputs from the terminal. */
    /* Restore the terminal config with no echo flag from backup. */
    tcsetattr(0, TCSAFLUSH, &term_default);
    printf("logrec done\n");
  }
  exit(0);
}


void fail(){
  /* Send a signal to all current process group with flag 0. */
  kill(0, SIGTERM);
  done();
}


void die_if_link(char *fn) {
  struct stat s;
  if (forceflg)
    return;
  if (lstat(fn, &s) == 0 && (S_ISLNK(s.st_mode) || s.st_nlink > 1))
    errx(EXIT_FAILURE,  "output file `%s' is a link\n"
        "Use --force if you really want to use it.\n"
        "Program not started.", fn);
}


static void usage(FILE *out)
{
  fputs("Usage:", out);

  fprintf(out, "%s [options] [file]\n", "logrec");

  fputs("Options:", out);
  fputs("Make a typescript of a terminal session.\n", out);

  fputs(" -a, --append            append the output\n"
    " -c, --command <command> run command rather than interactive shell\n"
    " -e, --return            return exit code of the child process\n"
    " -f, --flush             run flush after each write\n"
    "     --force             use output file even when it is a link\n"
    " -q, --quiet             be quiet\n"
    " -t, --timing[=<file>]   output timing data to stderr (or to FILE)\n"
    " -V, --version           output version information and exit\n"
    " -h, --help              display this help and exit\n\n", out);

  exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}


static void wait_for_empty_fd(int fd)
{
  struct pollfd fds[] = {
    { .fd = fd, .events = POLLIN }
    /* { fd, POLLIN } */
  };

  while (die == 0 && poll(fds, 1, 100) == 1);
}

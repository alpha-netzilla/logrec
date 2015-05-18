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

#ifndef HAVE_openpty
char  line[] = "/dev/YtyXX";
#endif

#ifdef HAVE_openpty
#include <pty.h>
#include <utmp.h>
#endif


void getpty(void);
void fixtty(void);
void resize(int);

void read_from_terminal_write_to_masterpty(void);
void read_from_masterpty_write_to_terminal(void);
void doshell(void);


char *get_time(char *);
void finish(int);
void done(void);
void fail(void);

struct  termios term_default;
struct  winsize win;


char *fname;

FILE *fp;
char *shell;

int master;
int slave;

int die;

pid_t child1;
pid_t child2;

#define SEG_SIZE 1
int seg_id;
char *stopflagp;


int main(int argc, char **argv){
  // setlocale(LC_ALL, "");
  // setlocale(LC_NUMERIC, "C");
  // bindtextdomain(PACKAGE, LOCALEDIR);
  // textdomain(PACKAGE);

  if (argc == 2){
    fname = argv[1];
  }else{
    fprintf(stderr,"%s\n","log file is not specified");
    exit(1);
  }

  if ((fp = fopen(fname, "a")) == NULL) {
    perror("Cannot openfile");
    exit(1);
  }

  shell = getenv("SHELL");
  if (shell == NULL) shell = _PATH_BSHELL;

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

  getpty();
  fixtty();

  // Handling when child1 finished.
  // There is no relation between child1 and child2,
  // As child2 will be a session(process) leader,
  signal(SIGCHLD, finish);

  child1 = fork();

  if(child1 < 0){
    perror("fork error"); fail();
  }
  // perent
  else if(child1 > 0){
    // Get a signal when control terminal is changed.
    signal(SIGWINCH, resize);

    // This process will read inputs as master pty.
    // It is able to send a messages to the foreground process of slave pty.
    read_from_terminal_write_to_masterpty();
  }
  // child1
  else if(child1 == 0){
    child2 = fork();
    if(child2 < 0){ perror("fork error"); fail(); }
    // child1
    else if(child2 > 0) read_from_masterpty_write_to_terminal();
    // child2
    else if(child2 == 0) doshell();
    exit(0);
  }
  return 0;
}


void getpty(){
  // Get a current termios.
  tcgetattr(0, &term_default);

  // Get a current window size.
  ioctl(0, TIOCGWINSZ, &win);

  // Equalize the initial slave pty to the current terminal.
  if (openpty(&master, &slave, NULL, &term_default, &win) < 0) {
    fprintf(stderr, "openpty failed\n");
    fail();
  }
}


void fixtty(){
  struct termios term_new;

  term_new = term_default;

  // Disable the all flags of input and output set to termios struct.
  cfmakeraw(&term_new);

  // tcsetattr(0, TCSAFLUSH, &term_new);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_new);
}


// parent
void resize(int dummy) {
  // Reserve the terminal window size to win struct.
  ioctl(0,     TIOCGWINSZ, (char *)&win);

  // Set the window size of slave pty to the attributes reserved to win struct.
  ioctl(slave, TIOCSWINSZ, (char *)&win);

  kill(child1, SIGWINCH);
}


// parent
void read_from_terminal_write_to_masterpty(){
  ssize_t rs;
  ssize_t ws;
  char inbuf[BUFSIZ];

  // close(1); for "logrec done" is not output.
  fclose(fp);
  close(slave);

  strcpy(stopflagp, "0");

  while (die == 0) {
    if  ((rs = read(0, inbuf, BUFSIZ)) > 0){
      if(inbuf[0] == '\r'){
        strcpy(stopflagp, "1");
      }else{
        strcpy(stopflagp, "0");
      }
      ws = write(master, inbuf, rs);
      if (ws <0) fail();
    }
  }
  done();
}


// child1
void read_from_masterpty_write_to_terminal(){
  char n;
  int startflag = 1;

  ssize_t rs;
  ssize_t ws;

  char outbuf[BUFSIZ];
  char time_buf[BUFSIZ];
  char tmp_buf[100000];
  tmp_buf[0] = '\0';

  close(0);
  close(slave);

  int size = 0;

  get_time(outbuf);
  fprintf(fp, "%s logrec started\n", outbuf);

  for (;;){
    rs = read(master, outbuf, sizeof(outbuf));

    // When received a EOF(Ctrl+D), quit a loop.
    if ( rs <=0 ) break;

    if((strcmp(stopflagp, "1")) == 0){
      get_time(time_buf);
      fprintf(fp, "%s ", time_buf);
      fprintf(fp, "%s\n", tmp_buf);

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

    ws = write(1, outbuf, rs);
    if (ws <0) fail();
    memset(outbuf, '\0', sizeof(outbuf));
  }
  done();
}


// child2
void doshell(){
  char *shname;

  // Make a child2 to be a session(process) leader.
  // Cut the child1-child2 relationship.
  setsid();

  // 擬似端末のスレーブ側を制御端末にする
  // Make a slave pseudo terminal a controll terminal.
  ioctl(slave, TIOCSCTTY, 0);

  close(master);
  fclose(fp);

  dup2(slave, 0);
  dup2(slave, 1);
  dup2(slave, 2);
  close(slave);

  // ex) /bin/bash to /bash
  shname = strrchr(shell, '/');

  // ex) /bash to bash
  if (shname) shname++;
  else shname = shell;

  execl(shell, shname, "-i", 0);

  perror(shell);
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


void finish(int dummy) {
  int status;
  pid_t pid;

  while ((pid = wait(&status)) > 0)
    if (pid == child1) die = 1;
}


void done() {
  time_t t;

  shmctl(seg_id, IPC_RMID, NULL);

  // child1
  if (child2) {
    char buf[BUFSIZ];
    get_time(buf);
    fprintf(fp, "%s logrec child1 done\n", buf);
    fclose(fp);
    close(master);
  }
  // parent
  else if (child1) {
    // Output all inputs from the terminal.
    // Restore the terminal config with no echo flag from backup.
    tcsetattr(0, TCSAFLUSH, &term_default);
    printf("logrec done\n");
  }
  exit(0);
}


void fail(){
  // Send a signal to all current process group with flag 0.
  kill(0, SIGTERM);
  done();
}

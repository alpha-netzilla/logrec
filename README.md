# logrec
records operation logs on linux

# Synopsis
A logrec records operation logs even if you login another servers with telnet, ssh and etc.
Outputs are not recorded. Just do only commands a user hits.

# Usage
1. build on linux  
$ gcc  -lutil -lpthread  logrec.c -o logrec

2. execute  
Argument is a file to record logs.  
$ ./logrec /var/logrec/some_user

3. check processes  
$ ps auxwf | grep logrec  
root     10925  (snip)  \_ ./logrec /var/logrec/some_user  
root     10926  (snip)      \_ ./logrec /var/logrec/some_user


# Usage and Result  
1. Do some operations.

2. Check a file.  
$ cat /var/logrec/some_user  
yyyy-mm-dd 00:00:00 logrec started  
yyyy-mm-dd 00:00:05 $ ls  
yyyy-mm-dd 00:00:10 $ ssh example.com  
yyyy-mm-dd 00:00:15 $ ls

3. when quitting the tool  
Ctrl + D

-notice-  
Logs are buffed.  
You might not see a record in the logfile after you hits one commands.  
It depends on OS you use.  
For checking normality of the tool, hit some commands.  


# See also
See http://alpha-netzilla.blogspot.com/2015/05/logrec.html






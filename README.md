# 404
file system watcher using inotify, libevent and pthread

gcc -Wall -g -I/usr/local/include/event2 -I/usr/include/apr-1 -L/usr/local/lib64 -pthread 404.c -lm -levent -levent_pthreads -lapr-1

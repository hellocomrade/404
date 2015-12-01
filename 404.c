#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <apr.h>
#include <apr_file_io.h>
#include <apr_fnmatch.h>
#include <apr_hash.h>
#include <apr_strings.h>
#include <apr_lib.h>

#define ERRORLOG 2
#define DEBUGLOG 0
#define INFOLOG 1

#define LOGLEVEL DEBUGLOG
#define RUN_AS_DAEMON
#define USING_LOGFILE

#define PORT 8787
#define MAX_READ_BYTE 1024

#define GLIN_PIC "/var/local/404/GLIN.png"
#define GLIN_PIC_SIZ 10240
#define GLIN_PIC_DESP "Service Provided by Great Lakes Information Network"
#define PIC_MAX_SIZ 1024*500
#define TXT_MAX_SIZ 1024

#define LOCKMODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)
#define WATCH_MASK (IN_CREATE|IN_CLOSE_WRITE|IN_DELETE|IN_DELETE_SELF|IN_ONLYDIR)

#define SUSV3 200112L
#define PATH_MAX_GUESS 1024

#define BUFSIZE 16384
#define TERM_TOKEN "DISMISS"

#define THRESHOLD 5
#define INSTANCE_SIZE 128
#define MESSAGE_VECTOR_THRESHOLD 128
#define PIC_STORE_INDEX_THRESHOLD 512

const char *UserName="nobody";
const char *LogFile="/var/log/404.log";
FILE *logger=NULL;
const char *LockFile="/var/run/404.pid";


char GLIN[GLIN_PIC_SIZ];
size_t GLIN_PIC_LEN=0;
#ifdef PATH_MAX
static int pathmax=PATH_MAX;
#else
static int pathmax=0;
#endif

static long posix_version=0;

int pipefd[2];
pid_t cpid;

typedef struct _notifyme{
    int wd;
    char *name;
}NotifyMe;
typedef struct _notifyinstance{
    int fd;
    size_t size;
    int current;
    char *root;
    NotifyMe *list;
}NotifyInstance;

typedef void (*SigFunc)(int);
typedef void (*SetBuggerFun)(const char*,NotifyInstance*);
typedef void (*AppendNodeFunc)(NotifyInstance*,int,char*);
typedef void (*RemoveNodeFunc)(NotifyInstance*,int);
typedef void (*RemoveNodeByNameFunc)(NotifyInstance*,const char*);
typedef char* (*GetPathFunc)(NotifyInstance*,int);
typedef int (*GetWatchIdFunc)(const char*,const char*);
typedef int (*SetFdsFunc)(fd_set*);

NotifyInstance* nInstances[INSTANCE_SIZE];
int END=0;
pthread_cond_t qready=PTHREAD_COND_INITIALIZER;
pthread_mutex_t qlock=PTHREAD_MUTEX_INITIALIZER;
int threadpool_infinite_loop=0;

typedef struct _messagevector{
    char** messages;
    pthread_t tid;
    int size;
    int index;
    int bufsize;
}MessageVector;

MessageVector messagesVector;

typedef struct _readBuf{
    char *buf;
    apr_hash_t *hash_table;
    apr_pool_t *pool;
    int size;
    struct event_base *base;
}FIFOContext;
typedef struct _socketContext{
    struct event_base *base;
    apr_pool_t *pool;
    apr_hash_t *hash_table;
}SocketContext;
typedef struct _picStore{
    char *bytes;
    char *texts;
    char *key;
    apr_time_t mtime[2];
    size_t piclen;
    size_t txtlen;
    size_t index;
}PictureStore;

typedef struct _picstoreindex{
    char **keyarray;
    size_t index;
    size_t size;
    size_t count;
}PicStoreIndex;

PicStoreIndex *picStoreIndex=NULL;

static sigset_t fillset;

static void logEntry(int loglevel,const char *fmt,...)
{
    if(loglevel<LOGLEVEL)
	return;

    va_list vl;
    va_start(vl,fmt);
    if(logger)
    {
	time_t cal=time(NULL);
	char* tstr=ctime(&cal);
	if(tstr)
	{
	    tstr[strlen(tstr)-1]=0;
	    fprintf(logger,"[%s] ",tstr);
	}
	vfprintf(logger,fmt,vl);
	fflush(logger);
    }
    else{
	vfprintf(stderr,fmt,vl);}
    va_end(vl);

}
static void logEntryTH(int loglevel,const char *fmt,...)
{
    if(loglevel<LOGLEVEL)
        return;
    va_list vl;
    va_start(vl,fmt);
    if(logger)
    {
        vfprintf(logger,fmt,vl);
        fflush(logger);
    }
    else
        vfprintf(stderr,fmt,vl);
    va_end(vl);
}
static void logEntry1(const char *msg,int loglevel)
{
    if(loglevel<LOGLEVEL)
        return;
    if(logger)
    {
	time_t cal=time(NULL);
        char* tstr=ctime(&cal);
        if(tstr)
        {
            tstr[strlen(tstr)-1]=0;
            fprintf(logger,"[%s] ",tstr);
        }
        fprintf(logger,"%s\n",msg);
	fflush(logger);
    }
    else
        fprintf(stderr,"%s\n",msg);
}
static void logEntryTH1(const char *msg,int loglevel)
{
    if(loglevel<LOGLEVEL)
        return;
    if(logger)
    {
        fprintf(logger,"%s\n",msg);
        fflush(logger);
    }
    else
        fprintf(stderr,"%s\n",msg);
}
static int lockFile(int fd)
{
    struct flock fl;
    fl.l_type = F_WRLCK;
    fl.l_start = 0;
    fl.l_whence = SEEK_SET;
    fl.l_len = 0;
    return(fcntl(fd, F_SETLK, &fl));
}

int already_running(void)
{
    int fd;
    char buf[16];
    fd=open(LockFile,O_RDWR|O_CREAT,LOCKMODE);
    if(fd<0)
    {
        logEntry(ERRORLOG,"Can't open %s: %s\n",LockFile,strerror(errno));
        exit(1);
    }
    if(lockFile(fd)<0)
    {
        if (errno==EACCES||errno==EAGAIN)
	{
            close(fd);
            return(1);
        }
        logEntry(ERRORLOG,"Can't lock %s: %s",LockFile,strerror(errno));
        exit(1);
    }
    ftruncate(fd, 0);
    sprintf(buf, "%ld", (long)getpid());
    write(fd, buf, strlen(buf)+1);
    return(0);
}
void daemonize()
{
    int i, fd0, fd1, fd2;
    pid_t pid;
    struct rlimit rl;
    struct sigaction sa;
    //Clear file creation mask.
    umask(0);
    //Get maximum number of file descriptors.
    if(getrlimit(RLIMIT_NOFILE,&rl)<0){
        logEntry1("Daemonize can't get file limit",ERRORLOG);
	exit(-1);
    }
    //Become a session leader to lose controlling TTY.
    if((pid=fork())<0){
        logEntry1("Daemonize can't fork",ERRORLOG);
	exit(-1);
    }
    else if(pid!= 0)//parent
	exit(0);
    setsid();
    //Ensure future opens won't allocate controlling TTYs.
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if(sigaction(SIGHUP,&sa,NULL)<0){
        logEntry1("Daemonize can't ignore SIGHUP",ERRORLOG);
	exit(-1);
    }
    if ((pid=fork())<0){
        logEntry1("Daemonize can't fork",ERRORLOG);
	exit(-1);
    }
    else if(pid!= 0)//parent
        exit(0);
    //Change the current working directory to the root so
    //we won't prevent file systems from being unmounted.
    if(chdir("/")<0){
        logEntry1("Daemonzie can't change directory to /",ERRORLOG);
	exit(-1);
    }
    //Close all open file descriptors.
    if(rl.rlim_max==RLIM_INFINITY)
        rl.rlim_max=1024;
    for (i=0;i<rl.rlim_max;i++)
        close(i);
    //Attach file descriptors 0, 1, and 2 to /dev/null.
    fd0 = open("/dev/null", O_RDWR);
    fd1 = dup(0);
    fd2 = dup(0);
}
static void* _alloc(size_t size,apr_pool_t *ap)
{
    void *p=NULL;
    p=(ap==NULL)?malloc(size):apr_pcalloc(ap,(apr_size_t)size);
    if(!p)
        logEntry1("Failed to malloc.",ERRORLOG);
    return p;
}
static char* path_alloc(int *psize,apr_pool_t *ap)
{
    char *ptr;
    int size;
    if(posix_version==0)
	posix_version=sysconf(_SC_VERSION);
    if(0==pathmax)
    {
	errno=0;
	if((pathmax=pathconf("/",_PC_PATH_MAX))<0)
	{
	    if(errno==0)
		pathmax=PATH_MAX_GUESS;
	    else
	    {
		logEntry1("pathconf error for _PC_PATH_MAX.",ERRORLOG);
		exit(-1);
	    }
	}
	else
	    ++pathmax;
    }
    if(posix_version<SUSV3)
	size=pathmax+1;
    else
	size=pathmax;
    ptr=_alloc(size,ap);
    if(psize)
	*psize=size;
    return ptr;
}
static void dispose(int fd,NotifyMe* ptr,size_t max)
{
    if(ptr&&max>0)
    {
	int i=0;
	for(;i<=max;++i,++ptr)
	    if(ptr->wd>0)
            {
                if(0==inotify_rm_watch(fd,ptr->wd))
                    logEntry(DEBUGLOG,"inotify watch removed with wd: %d.\n",ptr->wd);
                else
                    logEntry(ERRORLOG,"inotify watch NOT removed properly: %d.\n",ptr->wd);
		if(ptr->name)
		    free(ptr->name);
            }
    }
}
static void closeInstances()
{
    int i=0;
    for(;i<=END;++i)
    {
	if(nInstances[i])
	{
	    dispose(nInstances[i]->fd,nInstances[i]->list,nInstances[i]->current);
	    free(nInstances[i]->list);
	    if(0==close(nInstances[i]->fd))
                logEntry(INFOLOG,"inotify instance closed: %d.\n",nInstances[i]->fd);
            else
                logEntry(ERRORLOG,"inotify instance NOT closed properly: %d.\n",nInstances[i]->fd);
	    free(nInstances[i]->root);
	    free(nInstances[i]);
	    nInstances[i]=NULL;
	}
    }
    logEntry1("Call to dismiss foreman...",INFOLOG);
    write(pipefd[1],TERM_TOKEN,strlen(TERM_TOKEN));
    waitpid(cpid,NULL,0);
    logEntry1("Foreman stopped.",INFOLOG);
}
static void sig_handler(int sig)
{
    int i=0;
    switch(sig)
    {
	case SIGINT:
	case SIGTERM:
	case SIGPIPE:
	    //this line below has a bug took me 2 hours to figure out, I put %s
	    //as the conversion specifier for sig, which is an integer.
	    //so, vfprintf in logEntry took 0x02 (SIGINT) as the address for looking
	    //up the string, then of course, SIGSEGV
	    //logEntry(DEBUGLOG,"Process %d got signal:%s\n",getpid(),sig);
	    logEntry(DEBUGLOG,"Process %d got signal:%d\n",getpid(),sig);
	    pthread_mutex_lock(&qlock);
	    threadpool_infinite_loop=0;
            pthread_cond_broadcast(&qready);
            pthread_mutex_unlock(&qlock);
	    if(0==pthread_kill(messagesVector.tid,0))
	        pthread_join(messagesVector.tid,NULL);
            logEntry(INFOLOG,"Thread %d in parent process was terminated\n",i);
            for(;i<messagesVector.size;++i)
                free(messagesVector.messages[i]);
            free(messagesVector.messages);
	    closeInstances();
	    close(pipefd[1]);
     	    exit(0);
	break;
	default:
	break;
    }
}
static NotifyMe* reallocNodes(NotifyMe* old,size_t oldcount,size_t count)
{
    NotifyMe *ptr=NULL;
    if(old)
    {
	ptr=(NotifyMe*)_alloc(sizeof(NotifyMe)*count,NULL);
	if(!ptr)
	{
	    logEntry(ERRORLOG,"Failed to alloc memory for expanding list size:%d byte.\n",sizeof(NotifyMe)*count);
	    return ptr;
	}
	memcpy(ptr,old,oldcount*sizeof(NotifyMe));
    }
    return ptr;
}
static void clearNodes(NotifyMe* n,size_t count)
{
    if(n)
    {
	size_t i=0;
	for(;i<count;++n,++i)
	{
	    n->wd=-1;
	    n->name=NULL;
	}
    }
}
static size_t nearest_pow (size_t num)
{
    size_t n=num>0?num-1:0;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;

    return n;
}
static int expandArray(NotifyInstance *ins,int wd)
{
    wd=(wd==ins->size)?wd+1:wd;
    size_t t=nearest_pow(wd);
    logEntry(INFOLOG,"Have to expand the pool for instance %d from %d to %d\n",ins->fd,ins->size,t);
    NotifyMe *tmp=reallocNodes(ins->list,ins->size,t);
    if(!tmp)
        return -1;
    else
    {
       free(ins->list);
       ins->list=tmp;
       clearNodes(ins->list+ins->size,t-ins->size);
       ins->size=t;
    }
    return 0;
}
static void addWatch(NotifyInstance *ins,const char *path)
{
    int wd=inotify_add_watch(ins->fd,path,WATCH_MASK);
    if(wd>0)
    {
        if(wd>=ins->size)
            if(-1==expandArray(ins,wd))
	    {
		inotify_rm_watch(ins->fd,wd);
		logEntry(ERRORLOG,"Failed to expand array for instance %d with size %d.Abort!",ins->fd,ins->size);
		return;
	    }
        size_t rootlen=strlen(ins->root);
        size_t len=strlen(path)-rootlen+1;
	if(len==0)//as same as root
	  len=1;
        ins->list[wd].name=(char*)_alloc(len,NULL);
	if(!ins->list[wd].name)
	{
	    inotify_rm_watch(ins->fd,wd);
            logEntry(ERRORLOG,"Failed to allocate memory for path %s.Abort!",path);
            return;
	}
	//logEntry("root:%s; path:%s; rootlen:%d; len:%d\n",ins->root,path,rootlen,len);
	if(len>1)
	{
            strncpy(ins->list[wd].name,path+rootlen,len-1);
            ins->list[wd].name[len-1]=0;
	}
	else
	    ins->list[wd].name[0]=0;
        ins->list[wd].wd=wd;
        if(wd>ins->current)ins->current=wd;
        logEntry(INFOLOG,"Add watch on:%s%s with instance %d with wd=%d\n",ins->root,ins->list[wd].name,ins->fd,ins->list[wd].wd);
    }
    else
        logEntry(ERRORLOG,"Can not add watch at %s. Exceed limit?\n",path);
}
static void setbugger(const char *path,NotifyInstance *ins)
{
    if(!ins)return;
    if(ins->list==NULL)
    {
	ins->list=(NotifyMe*)_alloc(sizeof(NotifyMe)*(1<<THRESHOLD),NULL);
	ins->size=1<<THRESHOLD;
	clearNodes(ins->list,ins->size);
    }
    addWatch(ins,path);
}
static void dopath(char* path,size_t plen,int fd,NotifyInstance *ins,void (*callback)(const char*,NotifyInstance*))
{
    struct stat dirstat;
    DIR *dir;
    struct dirent *dirent;
    char *ptr;
    if(stat(path,&dirstat)!=0)
        return;
    if(S_ISREG(dirstat.st_mode)){
	addMessage(path);
	return;
    }
    if(!S_ISDIR(dirstat.st_mode))
	return;
    if((dir=opendir(path))==NULL)
    {
	logEntry(ERRORLOG,"can not read dir:%s\n",path);
	return;
    }
    if(callback!=NULL)
        callback(path,ins);
    ptr=path+strlen(path);
    *ptr++='/';
    *ptr=0;
    while((dirent=readdir(dir))!=NULL)
    {
	if(strcmp(dirent->d_name,".")==0||strcmp(dirent->d_name,"..")==0)
	    continue;
	strcpy(ptr,dirent->d_name);
	dopath(path,plen,fd,ins,callback);
    }
    closedir(dir);
}
static void addInstance(char *path,int plen,int fd,void (*callback)(const char*,NotifyInstance*))
{
    if(path&&path[0]=='/')
    {
	size_t len=sizeof(NotifyInstance);
        NotifyInstance *ins=(NotifyInstance*)_alloc(sizeof(NotifyInstance),NULL);
        memset(ins,0,len);
	if(!ins)
        {
            exit(-1);
        }
        ins->fd=inotify_init();
        if(ins->fd<=0)
        {
            logEntry(ERRORLOG,"Failed to init notify instance at: %s:%s\n",path,strerror(errno));
            exit(-1);
        }
        else if(ins->fd>=INSTANCE_SIZE)
        {
            logEntry(ERRORLOG,"Too many instances, more than %d\n",INSTANCE_SIZE);
            return;
        }
        len=strlen(path);
        if(path[len-1]!='/')
        {
            ins->root=(char*)_alloc(len+2,NULL);
            strncpy(ins->root,path,len);
            ins->root[len]='/';
            ins->root[len+1]='\0';
        }
        else
        {
            ins->root=(char*)_alloc(len+1,NULL);
            strcpy(ins->root,path);
        }
	ins->current=0;
        nInstances[ins->fd]=ins;
	if(ins->fd>END)END=ins->fd;
        dopath(path,plen,fd,ins,callback);
    }
}
static void removeNode(NotifyInstance *ins,int wd)
{
    if(ins&&ins->list[wd].wd==wd)
    {
	inotify_rm_watch(ins->fd,wd);
	ins->list[wd].wd=-1;
	logEntry(DEBUGLOG,"Remove watch at %s%s for instance %d\n",ins->root,ins->list[wd].name,ins->fd);
	free(ins->list[wd].name);
	ins->list[wd].name=NULL;
	if(ins->current==wd)ins->current--;
    }
}
static int setfds(fd_set *readset)
{
    int maxfd=0;
    size_t i=0;
    for(;i<=END;++i)
	if(nInstances[i]){
	     FD_SET(nInstances[i]->fd,readset);
	     if(nInstances[i]->fd>maxfd)maxfd=nInstances[i]->fd;
	}
	return maxfd;
}
static void removeNodeByName(NotifyInstance *ins,const char* path)
{
    static char *fullpath;
    if(!fullpath)
    {
        int s;
        fullpath=path_alloc(&s,NULL);
    }
    if(!fullpath)
    {
        logEntry1("Can not allocate memory in removeNodeByName.Abort!",ERRORLOG);
        return;
    }
    if(ins&&ins->list&&path)
    {
        int i=0;
        for(;i<=ins->current;++i)
	{
	    if(ins->list[i].wd>0)
	    {
		if(strlen(ins->list[i].name))
		    sprintf(fullpath,"%s%s/",ins->root,ins->list[i].name);
		else
        	    sprintf(fullpath,"%s",ins->root);
		if(0==strncmp(path,fullpath,strlen(path)))
		    break;
	    }
	}
	if(i<=ins->current)
	    removeNode(ins,i);
    }
}
static void appendNode(NotifyInstance *ins,int wd,char *name)
{
    if(wd>0&&ins&&ins->list[wd].wd==wd&&name)
    {
	static char *fullpath;
	if(!fullpath)
	{
	    int s;
	    fullpath=path_alloc(&s,NULL);
	}
	if(!fullpath)
	{
	    logEntry1("Can not allocate memory for new node.Abort!",ERRORLOG);
	    return;
	}
	if(strlen(ins->list[wd].name))
	    sprintf(fullpath,"%s%s/%s",ins->root,ins->list[wd].name,name);
	else
	    sprintf(fullpath,"%s%s",ins->root,name);
	addWatch(ins,fullpath);
    }
}
static char* getPath(NotifyInstance *ins,int wd)
{
    static char *fullpath;
    if(!fullpath)
    {
	int s;
	fullpath=path_alloc(&s,NULL);
    }
    if(!fullpath)
    {
        logEntry1("Can not allocate memory in getPath.Abort!",ERRORLOG);
        return fullpath;
    }
    if(strlen(ins->list[wd].name))
	sprintf(fullpath,"%s%s/",ins->root,ins->list[wd].name);
    else
        sprintf(fullpath,"%s",ins->root);
    return fullpath;
}
static int config(int pathsize)
{
    messagesVector.messages=NULL;
    messagesVector.index=-1;
    messagesVector.size=0;
    messagesVector.bufsize=pathsize;
    return 0;
}
int addMessage(char* buf)
{
    int i=0;
    if(!buf||strlen(buf)>messagesVector.bufsize)
	return -1;
    pthread_mutex_lock(&qlock);
    if(!messagesVector.messages)
    {
	messagesVector.messages=(char**)_alloc(sizeof(char*)*MESSAGE_VECTOR_THRESHOLD,NULL);
	if(!messagesVector.messages)
	{
	    logEntry1("Failed to allocate mem for messagesVector.messages",ERRORLOG);
	    exit(-1);
	}
	messagesVector.size=MESSAGE_VECTOR_THRESHOLD;
	for(;i<MESSAGE_VECTOR_THRESHOLD;++i)
	{
	    messagesVector.messages[i]=(char*)_alloc(sizeof(char)*messagesVector.bufsize,NULL);
	    if(!messagesVector.messages[i])
	    {
		logEntry(ERRORLOG,"Failed to allocate mem for messagesVector.messages[%d]\n",i);
                exit(-1);
	    }
	    memset(messagesVector.messages[i],0,messagesVector.bufsize);
	}
    }
    else if(messagesVector.index==messagesVector.size-1)
    {
	char **ptmp=(char**)_alloc(sizeof(char*)*messagesVector.size*2,NULL);
	if(!ptmp)
	{
	    logEntry1("Failed to allocate mem for ptmp in addMessage",ERRORLOG);
            exit(-1);
	}
	memcpy(ptmp,messagesVector.messages,messagesVector.size*sizeof(char*));
	free(messagesVector.messages);
	messagesVector.messages=ptmp;
	for(i=messagesVector.size;i<messagesVector.size*2;++i)
	{
	    messagesVector.messages[i]=(char*)_alloc(sizeof(char)*messagesVector.bufsize,NULL);
            if(!messagesVector.messages[i])
            {
                logEntry(ERRORLOG,"Failed to allocate mem for messagesVector.messages[%d](expand)\n",i);
                exit(-1);
            }
            memset(messagesVector.messages[i],0,messagesVector.bufsize);
	}
	messagesVector.size*=2;
	logEntry(INFOLOG,"***Expand messages vector to %d***\n",messagesVector.size);
    }
    logEntry(DEBUGLOG,"Add index %d message:%s\n",messagesVector.index+1,buf);
    strcpy(messagesVector.messages[++messagesVector.index],buf);
    pthread_cond_signal(&qready);
    pthread_mutex_unlock(&qlock);
    return 0; 
}

static int isValidPicFile(const char* buf){
    if(!buf)return -1;
    size_t len=strlen(buf);
    if(len<4)return -1;
    if(tolower(buf[len-1])=='g'&&buf[len-4]=='.'){
	if(tolower(buf[len-3])=='p'&&tolower(buf[len-2])=='n')return 0;
	if(tolower(buf[len-3])=='j'&&tolower(buf[len-2])=='p')return 0;
    }
    return -1;
}
static int isValidTxtUpdate(const char *buf,apr_pool_t *pool,char *dump){
    if(!buf)return -1;
    size_t len=strlen(buf);
    if(len<4)return -1;
    const char *bname;
    char *root,*name,*dpath;
    char **list;
    int i,val=-1;
    apr_array_header_t *result;
    if((bname=apr_filepath_name_get(buf))!=NULL){
	len=bname-buf+1;
	root=(char*)_alloc(len,pool);
	strncpy(root,buf,bname-buf);
	*(root+len)=0;
        if(APR_SUCCESS==apr_filepath_get(&dpath,APR_FILEPATH_NATIVE,pool)&&APR_SUCCESS==apr_filepath_set(root,pool)){
/*printf("root with :%s\n",root);
printf("bname with: %s\n",bname);
printf("buf with: %s\n",buf);
printf("dpath: %s\n",dpath);*/
	    len=strlen(bname);

	    name=(char*)_alloc(len+1,pool);
	    *name=0;
	    strncat(name,bname,len);
	    *(name+len-3)='*';
	    *(name+len-2)=0;

	    if(APR_SUCCESS==apr_match_glob(name,&result,pool)){
		list=(char **)result->elts;
		for(i=0;i<result->nelts;i++){
//printf("Found:%s\n",list[i]);
		    strncpy(name+len-3,"png",3);
        	    if(0==strncasecmp(name,list[i],len))val=0;
		    strncpy(name+len-3,"jpg",3);
		    if(0==strncasecmp(name,list[i],len))val=0;
		    if(0==val){
//printf("return 0\n");
			if(dump){
			    strncpy(dump,root,strlen(root));
			    strncpy(dump+strlen(root),list[i],strlen(list[i]));
			    *(dump+strlen(root)+strlen(list[i])+1)=0;
			}
			break;
		    }
    		}
	    }
	    apr_filepath_set(dpath,pool);
        }
    }
    return val;
}
void *consumeMessage(void* arg)
{
    pthread_mutex_lock(&qlock);
    logEntryTH1("Parent process's thread is up...",DEBUGLOG);
    apr_status_t apr_status;
    apr_pool_t *pool=NULL,*lpool=NULL;
    size_t len=0;
    int updatetxt=0;
    if((apr_status=apr_initialize())!=APR_SUCCESS){
    	logEntry1("Parent Process: Failed to init apr? Abort!",ERRORLOG);
        goto CLEANUP;
    }
    atexit(apr_terminate);
    if((apr_status=apr_pool_create(&pool,NULL))!=APR_SUCCESS)
    {
	logEntry1("Parent Process: Failed to init apr pool? Abort!",ERRORLOG);
        goto CLEANUP;
    }
    pthread_cond_signal(&qready);
    pthread_mutex_unlock(&qlock);
    while(1)
    {
	pthread_mutex_lock(&qlock);
	while(threadpool_infinite_loop==1&&(!messagesVector.messages||messagesVector.index==-1))
	{
	    logEntryTH1("Parent process thread waiting for cond.",DEBUGLOG);
            pthread_cond_wait(&qready,&qlock);
	}
	if(threadpool_infinite_loop==0)
	{
	    pthread_mutex_unlock(&qlock);
	    break;
	}
	logEntryTH(DEBUGLOG,"Get index %d on buf:%s\n",messagesVector.index,messagesVector.messages[messagesVector.index]);
	//fcntl(pipefd[0],F_GETFD,0)&O_NONBLOCK?printf("nonblock\n"):printf("blocked\n");
	updatetxt=0;
	len=strlen(messagesVector.messages[messagesVector.index]);
	if(0==strncasecmp(".txt",messagesVector.messages[messagesVector.index]+len-4,4)){
//printf("A txt file: %s\n",messagesVector.messages[messagesVector.index]);
	    if(APR_SUCCESS==apr_pool_create(&lpool,pool)){
	        if(0==isValidTxtUpdate(messagesVector.messages[messagesVector.index],lpool,NULL))
	    	    updatetxt=1;
	        apr_pool_destroy(lpool);
 	    }
	}
	if(updatetxt==1||isValidPicFile(messagesVector.messages[messagesVector.index])==0)
            write(pipefd[1],messagesVector.messages[messagesVector.index--],messagesVector.bufsize);
	else
	    --messagesVector.index;
	pthread_mutex_unlock(&qlock);
    }
    
CLEANUP:
    if(APR_SUCCESS!=apr_status){
	pthread_cond_signal(&qready);
        pthread_mutex_unlock(&qlock);
    }
    if(pool)apr_pool_destroy(pool);
    logEntryTH1("Parent process thread terminated.",INFOLOG);
    return NULL;    
}

static size_t add2PSIndex(char *key){
    if(picStoreIndex->index>=picStoreIndex->size){
	char **tmp=picStoreIndex->keyarray;
	picStoreIndex->keyarray=(char**)_alloc(sizeof(char*)*picStoreIndex->size*2,NULL);
	memcpy(picStoreIndex->keyarray,tmp,picStoreIndex->size);
	picStoreIndex->size*=2;
	free(tmp);
    }
    picStoreIndex->keyarray[picStoreIndex->index++]=key;
    picStoreIndex->count++;
    return picStoreIndex->index-1;
}
static size_t remove4PSindex(size_t idx){
    if(idx<0)return -1;
    //picStoreIndex->keyarray[idx]=NULL;
    //move the last pic at index-1 to idx for filling the hole
    char *p=picStoreIndex->keyarray[idx];
    if(p){
        //free(p);
        picStoreIndex->count--;
        if(picStoreIndex->index-1>idx){
	    picStoreIndex->keyarray[idx]=picStoreIndex->keyarray[--(picStoreIndex->index)];
	    return idx;
        }
        else{//we are removing the last pic
	    picStoreIndex->index--;
	    if(picStoreIndex->index<0)picStoreIndex->index=0;
	    return -1;
        }
    }
    return -1;
}
/* apr_hash_t doesn't duplicate strings of neither key nor value */
void do_fifo(evutil_socket_t fd, short event, void *arg){
    int blen=0,updatetxt=0,dopic=0,dotxt=0;
    size_t slen=0;
    apr_pool_t *lpool=NULL;
    PictureStore *psctx=NULL;
    char ext[3]={0};
    apr_size_t readb=0;
    FIFOContext* ctx=(FIFOContext*)arg;
    memset(ctx->buf,0,ctx->size);
    blen=read(fd,ctx->buf,ctx->size);
    if(blen>0){
	logEntry(INFOLOG,"Child process RECEIVE:%s\n",ctx->buf);
        if(0==strncmp(TERM_TOKEN,ctx->buf,strlen(TERM_TOKEN))){
    	    logEntry1("Received message to halt. Preparing...",INFOLOG);
	    event_base_loopbreak(ctx->base);
	    return;
        }
	slen=strlen(ctx->buf);
	if(slen>4){
	    //skip any file with the name as macro GLIN_PIC
	    if(strncasecmp(GLIN_PIC,apr_filepath_name_get(ctx->buf),strlen(GLIN_PIC))==0)return;
	    apr_finfo_t finfo,finfo1;
	    apr_file_t *pft;
	    PictureStore *pic=NULL;
	    if(APR_SUCCESS!=apr_pool_create(&lpool,ctx->pool)){
		logEntry1("Failed to create local mem pool.",ERRORLOG);
		return;
   	    }
	    if(0==strncasecmp(".txt",ctx->buf+slen-4,4)&&0==isValidTxtUpdate(ctx->buf,lpool,ctx->buf)){//txt update
		updatetxt=1;
		logEntry(DEBUGLOG,"Updating txt: %s\n",ctx->buf);
	    }
	    pic=(PictureStore*)apr_hash_get(ctx->hash_table,ctx->buf,APR_HASH_KEY_STRING);
	    
	    if(APR_SUCCESS!=apr_stat(&finfo,ctx->buf,APR_FINFO_SIZE|APR_FINFO_TYPE|APR_FINFO_MTIME,lpool)){
		if(pic){//remove pic
		    int id=remove4PSindex(pic->index);
		    if(id>=0){
			PictureStore *ps=(PictureStore*)apr_hash_get(ctx->hash_table,picStoreIndex->keyarray[id],APR_HASH_KEY_STRING);
			if(ps)ps->index=id;
		    }
		    apr_hash_set(ctx->hash_table,pic->key,APR_HASH_KEY_STRING,NULL);
		    free(pic->bytes);
		    free(pic->texts);
		    free(pic->key);
		    free(pic);
		    logEntry(INFOLOG,"Picture: %s has been removed.\n",ctx->buf);
		}
		else
	    	    logEntry(ERRORLOG,"Picture: %s doesn't exist.\n",ctx->buf);
		goto CLEANUP;
		
	    }
	    if(finfo.size==0||finfo.size>PIC_MAX_SIZ||finfo.filetype!=APR_REG){
	        logEntry(ERRORLOG,"Picture %s is not a regular file or its size exceeds % byte or 0 byte.\n",ctx->buf,PIC_MAX_SIZ);
		goto CLEANUP;
	    }
	    strncpy(ext,ctx->buf+slen-3,3);
	    strncpy(ctx->buf+slen-3,"txt",3);
	    if(APR_SUCCESS!=apr_stat(&finfo1,ctx->buf,APR_FINFO_SIZE|APR_FINFO_TYPE|APR_FINFO_MTIME,lpool)){
		logEntry(ERRORLOG,"Text file: %s doesn't exist.\n",ctx->buf);
		goto CLEANUP;
	    }
	    if(finfo1.size==0||finfo1.size>TXT_MAX_SIZ||finfo1.filetype!=APR_REG){
	        logEntry(ERRORLOG,"Text %s is not a regular file or its size exceeds % byte.\n",ctx->buf,TXT_MAX_SIZ);
		goto CLEANUP;
	    }
	    if(pic){//update
		psctx=pic;
		if(1==updatetxt&&psctx->mtime[1]<finfo1.mtime){
		    psctx->mtime[1]=finfo1.mtime;
		    if(finfo1.size>psctx->txtlen){
		    	psctx->txtlen=finfo1.size+1;
			free(psctx->texts);
			psctx->texts=(char*)_alloc(psctx->txtlen,NULL);
		    }
		    dotxt=1;
		}
		if(psctx->mtime[0]<finfo.mtime){
		    psctx->mtime[0]=finfo.mtime;
		    if(finfo.size>psctx->piclen){
			psctx->piclen=finfo.size;
			free(psctx->bytes);
			psctx->bytes=(char*)_alloc(psctx->piclen,NULL);
		    }
		    dopic=1;
		}
	    }
	    else{//new
	        if((psctx=(PictureStore*)_alloc(sizeof(PictureStore),NULL))==NULL){
	    	    logEntry1("Failed to alloc psctx from pool",ERRORLOG);
		    goto CLEANUP;
	    	}
		psctx->piclen=finfo.size;
	        psctx->txtlen=finfo1.size+1;
	        psctx->mtime[0]=finfo.mtime;
	        psctx->mtime[1]=finfo1.mtime;
		if((psctx->bytes=(char*)_alloc(psctx->piclen,NULL))==NULL){
		    logEntry1("Failed to alloc psctx->bytes from pool",ERRORLOG);
		    goto CLEANUP;
	        }
	        if((psctx->texts=(char*)_alloc(psctx->txtlen,NULL))==NULL){
		    logEntry1("Failed to alloc psctx->texts from pool",ERRORLOG);
		    goto CLEANUP;
	        }
		psctx->texts[psctx->txtlen-1]=0;
		if((psctx->key=(char*)_alloc(slen+1,NULL))==NULL){
		    logEntry1("Failed to alloc psctx->key from pool",ERRORLOG);
		    goto CLEANUP;
	        }
		dotxt=dopic=1;
	    }
	    if(dotxt){
	        if(APR_SUCCESS!=apr_file_open(&pft,ctx->buf,APR_READ,APR_OS_DEFAULT,lpool)){
		    logEntry(ERRORLOG,"Failed to open text: %s\n",ctx->buf);
        	    goto CLEANUP;
    	        }
	        if(APR_EOF!=apr_file_read_full(pft,psctx->texts,psctx->txtlen,&readb)){
        	    logEntry(ERRORLOG,"Failed to read text: %s\n",ctx->buf);
		    goto CLEANUP;
	        }
		//printf("txt len:%lu, read: %lu, content: %s\n",psctx->txtlen,readb,psctx->texts);
	        apr_file_close(pft);
		logEntry(INFOLOG,"Read text done on: %s\n",ctx->buf);
	    }
	    strncpy(ctx->buf+slen-3,ext,3);
	    if(dopic){
	        if(APR_SUCCESS!=apr_file_open(&pft,ctx->buf,APR_READ,APR_OS_DEFAULT,lpool)){
		    logEntry(ERRORLOG,"Failed to open pic: %s\n",ctx->buf);
        	    goto CLEANUP;
    	        }
	        if(APR_SUCCESS!=apr_file_read_full(pft,psctx->bytes,psctx->piclen,&readb)){
        	    logEntry(ERRORLOG,"Failed to read pic: %s\n",ctx->buf);
		    goto CLEANUP;
	        }
	        apr_file_close(pft);
		logEntry(INFOLOG,"Read pic done on: %s\n",ctx->buf);
	    }
	    //apr_pstrdup(ctx->pool,ctx->buf);
	    //cheat the hashtable only store truncated string with file basename
	    //the actually memory allocated with 4 more bytes that is able to
	    //hold extension like ".png"
	    //key[slen-4]=0;

	    if(!pic){//new pic
	        *(psctx->key)=0;
	        strncat(psctx->key,ctx->buf,slen);
		psctx->index=add2PSIndex(psctx->key);
	    }
	   
	    apr_hash_set(ctx->hash_table,psctx->key,APR_HASH_KEY_STRING,psctx);
	    
CLEANUP:
	    if(lpool)apr_pool_destroy(lpool);
	    return;
	}
    }
    else{
        logEntry1("Read EOF. Pipe close on the other end?...",INFOLOG);
        event_base_loopbreak(ctx->base);
        return;
    }
	
    
}
/*static int strncasecmp(const char *s1,const char *s2,size_t n){
    char c1,c2;
    while(n){
	c1=*s1++;
	c2=*s2++;
	c1=(c1>='A'&&c1<='Z')?(c1|0x20):c1;
	c2=(c2>='A'&&c2<='Z')?(c2|0x20):c2;
	if(c1==c2){
	    if(c1){
		--n;
		continue;
	    }
	    return 0;
	}
	return c1-c2;
    }
    return 0;
}*/
static int myatoi(const char *line,size_t n){
    int value;
    if(0>=n||n>10)return -1;//10-digits is big enough as an int
    for(value=0;n--;line++){
        if(*line<'0'||*line>'9')return -1;
        value=value*10+(*line-'0');
    }
    if(value<0)return -1;//if negative or overflow
    else return value;
}
static void writePic(struct evbuffer *output,const char*buf,size_t length){
    evbuffer_add_printf(output, "HTTP/1.0 %d OK\r\n",200);
    evbuffer_add_printf(output, "%s: %s\r\n","Content-Type","image/png");
    evbuffer_add_printf(output, "%s: %lu\r\n","Content-Length",length);
    evbuffer_add(output,"\r\n",2);
    evbuffer_add(output,buf,length);
}
static void writeNumber(struct evbuffer *output,int n){
    int len=(n==0)?1:(int)floor(log10(abs(n)))+1;
    if(n<0)++len;
    evbuffer_add_printf(output, "HTTP/1.0 %d OK\r\n",200);
    evbuffer_add_printf(output, "%s: %s\r\n","Content-Type","text/plain");
    evbuffer_add_printf(output, "%s: %d\r\n","Content-Length",len);
    evbuffer_add(output,"\r\n",2);
    evbuffer_add_printf(output, "%d",n);
}
static void writeTxt(struct evbuffer *output,const char*buf){
    evbuffer_add_printf(output, "HTTP/1.0 %d OK\r\n",200);
    evbuffer_add_printf(output, "%s: %s\r\n","Content-Type","text/plain");
    evbuffer_add_printf(output, "%s: %lu\r\n","Content-Length",strlen(buf));
    evbuffer_add(output,"\r\n",2);
    evbuffer_add_printf(output, "%s",buf);
}
void readcb(struct bufferevent *bev, void *arg){
    struct evbuffer *input, *output;
    char *line,*pline,*rpath;
    const char *name;
    PictureStore *ps=NULL;
    size_t n,rplen;
    SocketContext *ctx=(SocketContext*)arg;
    input = bufferevent_get_input(bev);
    output = bufferevent_get_output(bev);

    if ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_LF))) {
	pline=line;
        strsep(&line," ");
	rpath=strsep(&line," ");
	logEntry(INFOLOG,"Request: %s\n",rpath);
	rplen=strlen(rpath);
	if(rplen>=11&&0==strncasecmp(rpath,"/404/random",11)){
	    if(0==picStoreIndex->count)//no pic, will return GLIN default instead
		writeNumber(output,-1);
	    else//random [0,index-1]
		writeNumber(output,rand()/(RAND_MAX/picStoreIndex->index/+1));
	}
	else if(rplen>9&&0==strncasecmp(rpath,"/404/pic/",9)){
	    name=apr_filepath_name_get(rpath);
	    int idx=myatoi(name,strlen(name));
	    if(idx>=0&&picStoreIndex->index>idx){
		ps=(PictureStore*)apr_hash_get(ctx->hash_table,picStoreIndex->keyarray[idx],APR_HASH_KEY_STRING);
		if(ps!=NULL)
		    writePic(output,ps->bytes,ps->piclen);
		else
		    writePic(output,GLIN,GLIN_PIC_LEN);
	    }
	    else
		writePic(output,GLIN,GLIN_PIC_LEN);
	}
	else if(rplen>9&&0==strncasecmp(rpath,"/404/txt/",9)){
	    name=apr_filepath_name_get(rpath);
	    int idx=myatoi(name,strlen(name));
	    if(idx>=0&&picStoreIndex->index>idx){
		ps=(PictureStore*)apr_hash_get(ctx->hash_table,picStoreIndex->keyarray[idx],APR_HASH_KEY_STRING);
		if(ps!=NULL)
		    writeTxt(output,ps->texts);
		else
		    writeTxt(output,GLIN_PIC_DESP);
	    }
	    else
		writeTxt(output,GLIN_PIC_DESP);
	}
	else//unknow request
	    writePic(output,GLIN,GLIN_PIC_LEN);
        //evbuffer_add(output, "\n", 1);
        free(pline);
	evbuffer_drain(input,evbuffer_get_length(input));
    }

    if (evbuffer_get_length(input) >= MAX_READ_BYTE) {
        /* Too long; just process what there is and go on so that the buffer
         * doesn't grow infinitely long. */
        evbuffer_drain(input,evbuffer_get_length(input));
        //evbuffer_add(output, "\n", 1);
	writePic(output,GLIN,GLIN_PIC_LEN);
//http://www.mail-archive.com/libevent-users@monkey.org/msg01533.html
	bufferevent_free(bev);
    }
}
void errorcb(struct bufferevent *bev, short error, void *ctx){
    if (error & BEV_EVENT_EOF) {
        /* connection has been closed, do any clean up here */
        /* ... */
    } else if (error & BEV_EVENT_ERROR) {
        /* check errno to see what error occurred */
        /* ... */
    } else if (error & BEV_EVENT_TIMEOUT) {
        /* must be a timeout event handle, handle it */
        /* ... */
    }
    bufferevent_free(bev);
}
void do_socket(evutil_socket_t listener, short event, void *arg){
    struct event_base *base = ((SocketContext*)arg)->base;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int fd = accept(listener, (struct sockaddr*)&ss, &slen);

    if (fd < 0) {
        logEntry1("Failed to accept a connection!",ERRORLOG);
    } else if (fd > FD_SETSIZE) {
        close(fd);
    } else {
        struct bufferevent *bev;
        evutil_make_socket_nonblocking(fd);
        bev=bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        bufferevent_setcb(bev, readcb, NULL, errorcb, (void*)arg);
        bufferevent_setwatermark(bev, EV_READ, 0, MAX_READ_BYTE);
        bufferevent_enable(bev, EV_READ|EV_WRITE);
    }
}
int dispatch(int rfd,int wfd){
    if(rfd<0||wfd<0)return -1;
    int status=0;
    struct event_base *base;
    struct event *fifo_event=NULL;
    struct event *socket_event=NULL;
    evutil_socket_t listener;
    struct sockaddr_in sin;
    apr_finfo_t finfo;
    apr_file_t *pft;
    apr_size_t readb;
    apr_status_t apr_status;
    apr_pool_t *pool=NULL,*lpool=NULL;
    if((apr_status=apr_initialize())!=APR_SUCCESS){
    	logEntry1("Failed to init apr? Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    atexit(apr_terminate);
    if((apr_status=apr_pool_create(&pool,NULL))!=APR_SUCCESS)
    {
	logEntry1("Failed to init apr pool? Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    SocketContext *ctx=(SocketContext*)_alloc(sizeof(SocketContext),pool);
    ctx->pool=pool;
    FIFOContext *rb=(FIFOContext*)_alloc(sizeof(FIFOContext),pool);
    rb->pool=pool;
    rb->buf=NULL;
    rb->buf=path_alloc(&rb->size,pool);
    if(!rb->buf){
	logEntry(ERRORLOG,"Can not allocate memory for foreman buf:%s\n",strerror(errno));
	status=-1;
	goto CLEANUP;
    }
    logEntry1("Foreman is starting...",INFOLOG);
    //tell parent process that we are about to ready
    /*if(5!=write(wfd,"SYNC",5)){
        logEntry1("Failed to notify the status to parent process. Abort!",ERRORLOG);
        status=-1;
	goto CLEANUP;
    }*/
    close(wfd);
    char sb[5];
    if(5!=read(rfd,sb,5))
    {
	logEntry1("Failed to SYNC with parent process. Abort!",ERRORLOG);
        status=-1;
	goto CLEANUP;
    }
    else
	logEntry1("SYNC with parent done.",INFOLOG);
    
    if((apr_status=apr_pool_create(&lpool,pool))!=APR_SUCCESS){
	logEntry1("Failed to create local mem pool. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    if((apr_status=apr_stat(&finfo,GLIN_PIC,APR_FINFO_SIZE|APR_FINFO_TYPE,lpool))!=APR_SUCCESS){
        logEntry1("Failed to get GLIN PIC size. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    if(finfo.size>GLIN_PIC_SIZ||finfo.filetype!=APR_REG){
	logEntry(ERRORLOG,"GLIN_PIC is not a regular file or its size exceeds % byte",GLIN_PIC_SIZ);
	status=-1;
	goto CLEANUP;
    }
    GLIN_PIC_LEN=finfo.size;
    
    if((apr_status=apr_file_open(&pft,GLIN_PIC,APR_READ,APR_OS_DEFAULT,lpool))!=APR_SUCCESS){
	logEntry1("Failed to open GLIN PIC. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    if((apr_status=apr_file_read_full(pft,GLIN,GLIN_PIC_SIZ,&readb))!=APR_EOF){
        logEntry1("Failed to read GLIN PIC. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    apr_file_close(pft);
    apr_pool_destroy(lpool);
    sin.sin_family=AF_INET;
    sin.sin_addr.s_addr=0;
    sin.sin_port=htons(PORT);
    listener=socket(AF_INET,SOCK_STREAM,0);
    evutil_make_socket_nonblocking(listener);
    if(0>bind(listener,(struct sockaddr*)&sin,sizeof(sin))){
	logEntry(ERRORLOG,"Failed to bind on the address 0.0.0.0: %s Abort!\n",strerror(errno));
        status=-1;
        goto CLEANUP;
    }
    //evutil_make_socket_nonblocking(rfd);
    //evthread_use_pthreads();
    /*struct event_config *cfg = event_config_new();

    event_config_avoid_method(cfg, "epoll");

    base = event_base_new_with_config(cfg);  
    event_config_free(cfg);*/
    //fcntl(rfd,F_GETFD,0);
    fcntl(rfd,F_SETFL,O_NONBLOCK);
    if(NULL==(base=event_base_new())){
	logEntry1("Failed to start libevent2. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    rb->base=base;
    rb->hash_table=apr_hash_make(pool);
    ctx->hash_table=rb->hash_table;
    ctx->base=base;
    if(NULL==(fifo_event=event_new(base, (evutil_socket_t)rfd, EV_READ|EV_PERSIST, do_fifo, (void*)rb))){
        logEntry1("Failed to event_new on pipe. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    if(0>listen(listener,64)){
	logEntry1("Failed to listen on the socket. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    if(NULL==(socket_event=event_new(base, listener, EV_READ|EV_PERSIST, do_socket, (void*)ctx))){
        logEntry1("Failed to event_new on socket. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    event_add(fifo_event, NULL);
    event_add(socket_event, NULL);
    if((picStoreIndex=(PicStoreIndex*)_alloc(sizeof(PicStoreIndex),NULL))==NULL){
        logEntry1("Failed to alloc picStoreIndex from the pool. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    if((picStoreIndex->keyarray=(char**)_alloc(sizeof(char*)*PIC_STORE_INDEX_THRESHOLD,NULL))==NULL){
	logEntry1("Failed to alloc picStoreIndex->keyarray from the pool. Abort!",ERRORLOG);
        status=-1;
        goto CLEANUP;
    }
    picStoreIndex->index=0;
    picStoreIndex->count=0;
    picStoreIndex->size=PIC_STORE_INDEX_THRESHOLD;
    logEntry1("Start Event Loop.",DEBUGLOG);
    event_base_dispatch(base);
    
    //available since 2.1.1-alpha
    //libevent_global_shutdown();
CLEANUP:
    if(APR_SUCCESS!=apr_status&&APR_EOF!=apr_status)
	logEntry1(strerror(APR_TO_OS_ERROR(apr_status)),ERRORLOG);
    if(fifo_event)event_free(fifo_event);
    if(socket_event)event_free(socket_event);
    if(base)event_base_free(base);
    if(picStoreIndex){
	int i=0;
	PictureStore *ps=NULL;
	for(;i<picStoreIndex->count;++i){
	    ps=(PictureStore*)apr_hash_get(ctx->hash_table,picStoreIndex->keyarray[i],APR_HASH_KEY_STRING);
	    if(ps){
		free(ps->bytes);
		free(ps->texts);
		free(ps->key);
		free(ps);

	    }
	}
        if(picStoreIndex->keyarray)free(picStoreIndex->keyarray);
        free(picStoreIndex);
    }
    if(pool)apr_pool_destroy(pool);
    
    close(rfd);
    logEntry(DEBUGLOG,"Child process %d cleanup done.\n",getpid());
    return status;
}

SigFunc sigCallback=sig_handler;
AppendNodeFunc appendNodeCallback=appendNode;
RemoveNodeFunc removeNodeCallback=removeNode;
RemoveNodeByNameFunc removeNodeByNameCallback=removeNodeByName;
SetFdsFunc setFdsCallback=setfds;
SetBuggerFun setBuggerCallback=setbugger;
GetPathFunc getPathCallback=getPath;
   
int main(int argc,char **argv)
{
    int psize,i,err=0;
    sigset_t oset;
    char *path=path_alloc(&psize,NULL);
    if(path==NULL||config(psize)==-1)
	return -1;
    memset(path,0,psize);
    
    struct stat dirstat;
#ifdef RUN_AS_DAEMON
    daemonize();
#endif
printf("got here?");
#ifdef USING_LOGFILE
    logger=fopen(LogFile,"a");
    if(!logger)
    {
	perror("Can't access the log file");
	return -1;
    }
#endif
#ifdef RUN_AS_DAEMON
    if(already_running())
    {
	logEntry1("Instance already running as a daemon.",ERRORLOG);
	exit(1);
    }
    //Handle signals of interest.
    struct sigaction sa;
    sa.sa_handler=sigCallback;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=0;
    if(sigaction(SIGTERM,&sa,NULL)<0)
    {
        logEntry(ERRORLOG,"Daemon can't catch SIGTERM: %s", strerror(errno));
        exit(1);
    }
    if(sigaction(SIGPIPE,&sa,NULL)<0)
    {
        logEntry(ERRORLOG,"Daemon can't catch SIGPIPE: %s", strerror(errno));
        exit(1);
    }
    struct passwd *pwd=getpwnam(UserName);
    if(!pwd)
        logEntry(ERRORLOG,"Failed to retrieve /etc/passwd for user %s\n",UserName);
    setuid(pwd->pw_uid);
#else
    signal(SIGINT,sigCallback);
#endif
//O_NONBLOCK may cause the loss of message since kernel pipe size is only 4Kx16=64K
////BLOCK mode will guarantee that the write is hanging if the buffer is full
    if(-1==pipe2(pipefd,O_CLOEXEC)){
    	logEntry(ERRORLOG,"Can not create pipe:%s\n",strerror(errno));
	return -1;
    }
    if((cpid=fork())<0){
	logEntry(ERRORLOG,"Can not fork foreman:%s\n",strerror(errno));
	return -1;
    }
    else if(cpid==0){//child
	free(path);
	struct sigaction sa;
        sa.sa_handler=SIG_IGN;//childSigHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags=0;
        if(sigaction(SIGTERM,&sa,NULL)<0)
        {
            logEntry(ERRORLOG,"Child process can't ignore SIGTERM: %s", strerror(errno));
          exit(1);
        }
	if(sigaction(SIGINT,&sa,NULL)<0)
        {
            logEntry(ERRORLOG,"Child process can't ignore SIGINT: %s", strerror(errno));
            exit(1);
        }
    	return dispatch(pipefd[0],pipefd[1]);
    }
    /*int rflag;
    if((rflag=fcntl(pipefd[0],F_GETFD,0))<0)
    {
        logEntry1("Failed to retrive pipe read fd flag. Abort!",ERRORLOG);
        return -1;
    }
    if(fcntl(pipefd[0],F_SETFL,rflag&~(O_NONBLOCK))<0)
    {
        logEntry1("Failed to remove NONBLOCK pipe read fd flag. Abort!",ERRORLOG);
        return -1;
    }*/
//    char sb[5];
//    int isb=0;
    //logEntry1("Parent waits for child.",INFOLOG);
    //printf("%s\n",fcntl(pipefd[0],F_GETFD,0)&O_NONBLOCK?"nonblock":"block");
    //while(1){
  //      if((isb=read(pipefd[0],sb,5))!=5)
    //    {
//printf("%s\n",fcntl(pipefd[0],F_GETFD,0)&O_NONBLOCK?"nonblock":"block");
//since the read fd could be set as O_NONBLOCK on the child process, even though on the parent side
//it still reports as "block", EAGAIN will be set on errno. The formal message is:
//Resource temporarily unavailable though.
//printf("isb:%d:%s\n",isb,strerror(errno));
	    //if(errno!=EAGAIN){
//	        logEntry1("Failed to get notification from child. Abort!",ERRORLOG);
//	        return -1;
	    //}
	    //else
	//	continue;
  //      }
	//break;
    //}
    logEntry1("Tell Child to go ahead.",INFOLOG);
    close(pipefd[0]);
    write(pipefd[1],"SYNC",5);
    logEntry1("Parent process creating thread...",DEBUGLOG);
    pthread_mutex_lock(&qlock);
    threadpool_infinite_loop=1;
    pthread_sigmask(SIG_SETMASK,&fillset,&oset);
    if((err=pthread_create(&messagesVector.tid,NULL,consumeMessage,NULL))!=0)
    {
	pthread_sigmask(SIG_SETMASK,&oset,NULL);
	logEntry(ERRORLOG,"Failed to create thread for parent process:%d\n",err);
	pthread_mutex_unlock(&qlock);
	return -1;
    }
    pthread_sigmask(SIG_SETMASK,&oset,NULL);
    pthread_cond_wait(&qready,&qlock);
    pthread_mutex_unlock(&qlock);
    memset(nInstances,0,sizeof(NotifyInstance*)*INSTANCE_SIZE);
    if(argc==1){
        getcwd(path,psize);
        addInstance(path,psize,pipefd[1],setBuggerCallback);
    }
    else{
	for(i=1;i<argc;++i){
	    *path=0;//strncpy does not alwyas place a '\0' terminator in dest string. using strncat instead: C FAQ 13.2
            strncat(path,argv[i],psize);
            if(*(path+strlen(path)-1)=='/')//no tailing slash allowed
                *(path+strlen(path)-1)='\0';
            if(stat(path,&dirstat)!=0&&!S_ISDIR(dirstat.st_mode)){
                logEntry(ERRORLOG,"Not a valid directory path:%s\n",path);
                continue;
            }
            addInstance(path,psize,pipefd[1],setBuggerCallback);
        }
    }
    if(END==0){
        logEntry1("No valid directory path to watch!",ERRORLOG);
	closeInstances();
        return 0;
    }
    int result;
    char buf[BUFSIZE]__attribute__((aligned(4)));
    fd_set readset;
    int maxfd;
    NotifyInstance *ins;
    char *dir=NULL;
    size_t len,j;
    while(1){
	do
	{
	    FD_ZERO(&readset);
    	    maxfd=setFdsCallback(&readset);
	    result=select(maxfd+1,&readset,NULL,NULL,NULL);
	}while(result==-1&&errno==EINTR);
	for(j=0;result>0&&j<=END;++j)
	{
	    ins=nInstances[j];
	    if(!ins)continue;
	    if(FD_ISSET(ins->fd,&readset))
	    {
	        do
		{
		    i=0;
	            len=read(ins->fd,buf,BUFSIZE);//inotify slurping: read as many events as the buf could fit
	            while(len>0&&i<len)
	            {
		        struct inotify_event *event=(struct inotify_event*)&buf[i];
		        if(event!=NULL)
		        {
		            if(event->mask&IN_CREATE)
		            {
			        logEntry(INFOLOG,"%s at %s%s was created\n",(event->mask&IN_ISDIR)?"Directory":"File",getPathCallback(ins,event->wd),event->name);
			        if((event->mask&IN_ISDIR)&&event->len)
			        appendNodeCallback(ins,event->wd,event->name);
			    }
			    else if(event->mask&IN_CLOSE_WRITE)
			    {
				dir=getPathCallback(ins,event->wd);
			        logEntry(INFOLOG,"%s at %s%s was closed for writting\n",(event->mask&IN_ISDIR)?"Directory":"File",dir,event->name);
				sprintf(path,"%s%s",dir,event->name);
				addMessage(path);
			    }
		            else if(event->mask&IN_DELETE)
			    {
				dir=getPathCallback(ins,event->wd);
			        logEntry(INFOLOG,"%s at %s%s was deleted\n",(event->mask&IN_ISDIR)?"Directory":"File",dir,event->name);
				if(!(event->mask&IN_ISDIR)){
				    sprintf(path,"%s%s",dir,event->name);
				    addMessage(path);
				}
				    
			    }
			    else if(event->mask&IN_DELETE_SELF)//&&(event->mask&IN_ISDIR))
			    {
			        logEntry(INFOLOG,"Directory at %s%s was deleted(self)\n",getPathCallback(ins,event->wd),event->name);
			        removeNodeCallback(ins,event->wd);
			    }
			    i+=sizeof(struct inotify_event)+event->len;
		        }
			else
			{
			    logEntry1("Failed to read inotify_event!",ERRORLOG);
			    break;
			}
		    }
		    if(0>ioctl(ins->fd,FIONREAD,&i))
		    {
			logEntry(ERRORLOG,"Failed to get the size of pending events for instance %d:%s\n",ins->fd,strerror(errno));
			i=0;
		    }
		}
		while(i>0);
		--result;
	    }
	}
    }
    return 0;
}

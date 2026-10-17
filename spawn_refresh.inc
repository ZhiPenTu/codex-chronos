// On-demand egress refresh so the plugin works with no daemon installed.
//
// The preferred transport is the refresh_egress.py --serve helper (Unix socket
// on Linux/macOS, file mailbox on Windows; see refresh_bridge.inc). When
// nothing serves it — the common desktop case, where the user never starts a
// background job — the plugin runs the helper itself:
//
//   * tz_refresh_once()      one-shot, blocking; used by the management
//                            "egress/refresh" route (the 获取 button).
//   * tz_refresh_background() fire-and-forget; used by
//                            tz_maybe_background_refresh() to keep the state
//                            file fresh from the request path. POSIX double
//                            forks so no zombie accumulates; Windows starts a
//                            detached, unwaited process.
//
// Helper script lookup order: CODEX_TZ_HELPER > data directory > plugin
// library directory > EasyCLIProxyAPI app data > ~/.codex-chronos (Windows
// also checks %LOCALAPPDATA%\codex-chronos) > /usr/local/lib/codex-chronos.
// Interpreter: CODEX_TZ_PYTHON > system python3 > PATH lookup > the script
// itself when it is executable. refresh_egress.py mirrors the data path
// resolution in paths.inc.
#ifndef CODEX_TZ_SPAWN_REFRESH_INC
#define CODEX_TZ_SPAWN_REFRESH_INC

#include <time.h>

#ifdef _WIN32
#include <process.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// Not exposed under -std=c11 on macOS; the symbol exists in libSystem.
extern char **environ;
#endif

#ifndef CODEX_TZ_HELPER_SCRIPT
#define CODEX_TZ_HELPER_SCRIPT "refresh_egress.py"
#endif

static int tz_file_exists(const char *path) {
  struct stat st;
  return path && *path && stat(path,&st)==0 && S_ISREG(st.st_mode);
}

// Build "a/b/c/d" from up to four segments, skipping NULL/empty ones, using
// the platform separator. Returns 0 on success or -1 when the result would
// not fit, so callers never depend on silent snprintf truncation (which
// -Wformat-truncation rightly rejects).
static int tz_path_build(char *dst,size_t cap,const char *a,const char *b,
                         const char *c,const char *d) {
  if(!dst||!cap)return -1;
  const char *seg[4]={a,b,c,d};
  size_t w=0;
  for(int i=0;i<4;i++) {
    const char *part=seg[i];
    if(!part||!*part)continue;
    if(w)while(*part=='/'||*part=='\\')part++;
    if(!*part)continue;
    size_t n=strlen(part);
    while(n&&(part[n-1]=='/'||part[n-1]=='\\'))n--;
    if(!n) {
      if(!w && (part[0]=='/' || part[0]=='\\')) {
        dst[w++]=CODEX_TZ_SEP_CHAR;
      }
      continue;
    }
    size_t pad=w?1:0;
    if(w+pad+n+1>cap)return -1;
    if(pad)dst[w++]=CODEX_TZ_SEP_CHAR;
    memcpy(dst+w,part,n);
    w+=n;
  }
  if(!w)return -1;
  dst[w]=0;
  return 0;
}

// Directory holding the loaded plugin library, for drop-in installs.
static int tz_library_dir(char *out,size_t cap) {
#ifdef _WIN32
  static const int tz_module_marker=0;
  HMODULE module=NULL;
  if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&tz_module_marker,&module))return -1;
  char path[CODEX_TZ_PATH_CAP];
  DWORD n=GetModuleFileNameA(module,path,sizeof(path));
  if(n==0||n>=sizeof(path))return -1;
  char *cut=strrchr(path,'\\');
  char *alt=strrchr(path,'/');
  if(alt&&(!cut||alt>cut))cut=alt;
  if(!cut||cut==path)return -1;
  *cut=0;
  if(strlen(path)+1>cap)return -1;
  memcpy(out,path,strlen(path)+1);
  return 0;
#else
  Dl_info info;
  memset(&info,0,sizeof(info));
  if(!dladdr((void *)&tz_file_exists,&info)||!info.dli_fname||!*info.dli_fname)return -1;
  char path[CODEX_TZ_PATH_CAP];
  snprintf(path,sizeof(path),"%s",info.dli_fname);
  char *cut=strrchr(path,'/');
  if(!cut||cut==path)return -1;
  *cut=0;
  if(strlen(path)+1>cap)return -1;
  memcpy(out,path,strlen(path)+1);
  return 0;
#endif
}

// First existing refresh_egress.py, or NULL when none of the candidates
// exists. The result points at a static buffer valid until the next call.
static const char *tz_helper_script(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env=getenv("CODEX_TZ_HELPER");
  if(env && *env && strlen(env)<sizeof(out) && tz_file_exists(env)) {
    snprintf(out,sizeof(out),"%s",env);
    return out;
  }
  const char *dir=tz_data_dir();
  if(dir && *dir) {
    if(tz_path_build(out,sizeof(out),dir,CODEX_TZ_HELPER_SCRIPT,NULL,NULL)==0 &&
       tz_file_exists(out))return out;
  }
  // Next to the loaded plugin library (manual drop-in installs).
  char folder[CODEX_TZ_PATH_CAP];
  if(tz_library_dir(folder,sizeof(folder))==0 &&
     tz_path_build(out,sizeof(out),folder,CODEX_TZ_HELPER_SCRIPT,NULL,NULL)==0 &&
     tz_file_exists(out))return out;
  const char *local=getenv("LOCALAPPDATA");
  if(local && *local) {
    if(tz_path_build(out,sizeof(out),local,CODEX_TZ_DIRNAME,
         CODEX_TZ_HELPER_SCRIPT,NULL)==0 && tz_file_exists(out))return out;
  }
  const char *home=getenv("HOME");
  if(home && *home) {
    // EasyCLIProxyAPI desktop layout (see README).
    if(tz_path_build(out,sizeof(out),home,
         "Library/Application Support/com.cpa.gui",CODEX_TZ_DIRNAME,
         CODEX_TZ_HELPER_SCRIPT)==0 && tz_file_exists(out))return out;
    char dotdir[CODEX_TZ_PATH_CAP];
    if(snprintf(dotdir,sizeof(dotdir),".%s",CODEX_TZ_DIRNAME)>0 &&
       tz_path_build(out,sizeof(out),home,dotdir,CODEX_TZ_HELPER_SCRIPT,NULL)==0 &&
       tz_file_exists(out))return out;
  }
  if(tz_path_build(out,sizeof(out),"/usr/local/lib",CODEX_TZ_DIRNAME,
       CODEX_TZ_HELPER_SCRIPT,NULL)==0 && tz_file_exists(out))return out;
  return NULL;
}

// Windows has no execute bit; any readable regular file can be handed to
// CreateProcess (the interpreter runs it, or the shell association does).
static int tz_is_executable(const char *path) {
#ifdef _WIN32
  return tz_file_exists(path);
#else
  return path && *path && access(path,X_OK)==0;
#endif
}

// Absolute python interpreter path, or NULL when none is available.
static const char *tz_python_absolute(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env=getenv("CODEX_TZ_PYTHON");
  if(env && *env && strlen(env)<sizeof(out) && tz_is_executable(env)) {
    snprintf(out,sizeof(out),"%s",env);
    return out;
  }
#ifndef _WIN32
  static const char *candidates[]={
    "/usr/bin/python3","/usr/local/bin/python3","/opt/homebrew/bin/python3",
    "/usr/bin/python","/usr/local/bin/python",NULL};
  for(int i=0;candidates[i];i++) {
    if(access(candidates[i],X_OK)==0) {
      snprintf(out,sizeof(out),"%s",candidates[i]);
      return out;
    }
  }
#endif
  // Manual PATH search so a spawned child never has to resolve the name.
  const char *path=getenv("PATH");
  if(path && *path) {
    char folder[CODEX_TZ_PATH_CAP];
    snprintf(folder,sizeof(folder),"%s",path);
    for(char *cursor=folder;*cursor;) {
      char *sep=strchr(cursor,CODEX_TZ_PATH_LIST_SEP);
      if(sep)*sep=0;
      if(*cursor) {
#ifdef _WIN32
        static const char *names[]={"python.exe","python3.exe",NULL};
        for(int i=0;names[i];i++) {
          if(tz_path_build(out,sizeof(out),cursor,names[i],NULL,NULL)==0 &&
             tz_is_executable(out)) {
            return out;
          }
        }
#else
        if(tz_path_build(out,sizeof(out),cursor,"python3",NULL,NULL)==0 &&
           access(out,X_OK)==0)return out;
        if(tz_path_build(out,sizeof(out),cursor,"python",NULL,NULL)==0 &&
           access(out,X_OK)==0)return out;
#endif
      }
      if(!sep)break;
      cursor=sep+1;
    }
  }
  return NULL;
}

// Fill argv for spawning the helper. Returns 1 on success.
static int tz_helper_argv(char **argv,size_t cap) {
  const char *script=tz_helper_script();
  if(!script||cap<3)return 0;
  const char *python=tz_python_absolute();
  if(python) {
    argv[0]=(char *)python;
    argv[1]=(char *)script;
    argv[2]=NULL;
    return 1;
  }
  // No interpreter found: the script may still be directly executable.
  if(tz_is_executable(script)) {
    argv[0]=(char *)script;
    argv[1]=NULL;
    return 1;
  }
  return 0;
}

// Append a one-line note to <data_dir>/helper.log (best effort).
static void tz_helper_log(const char *line) {
  const char *dir=tz_data_dir();
  if(!dir||!*dir)return;
  tz_ensure_dir(dir);
  char logpath[CODEX_TZ_PATH_CAP];
  snprintf(logpath,sizeof(logpath),"%s",tz_join("helper.log"));
  int fd=open(logpath,O_WRONLY|O_CREAT|O_APPEND,0600);
  if(fd<0)return;
  char stamp[64];
  time_t now=time(NULL);
  struct tm tm_buf;
  if(localtime_r(&now,&tm_buf)) {
    strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",&tm_buf);
  } else {
    snprintf(stamp,sizeof(stamp),"?");
  }
  char entry[CODEX_TZ_PATH_CAP+128];
  int n=snprintf(entry,sizeof(entry),"[%s] %s\n",stamp,line?line:"");
  if(n>0) {
    ssize_t written=write(fd,entry,(size_t)n);
    (void)written;
  }
  close(fd);
}

#ifdef _WIN32
// Quote one argument the way the MSVC runtime splits a command line:
// backslashes only need doubling when they directly precede a quote.
static void tz_win_quote(char *dst,size_t cap,const char *src) {
  size_t w=0;
  if(w+1<cap)dst[w++]='"';
  size_t backslashes=0;
  for(const char *p=src;*p;p++) {
    if(*p=='\\'){backslashes++;continue;}
    if(*p=='"') {
      for(size_t i=0;i<backslashes*2+1&&w+1<cap;i++)dst[w++]='\\';
      if(w+1<cap)dst[w++]='"';
    } else {
      for(size_t i=0;i<backslashes&&w+1<cap;i++)dst[w++]='\\';
      if(w+1<cap)dst[w++]=*p;
    }
    backslashes=0;
  }
  for(size_t i=0;i<backslashes*2&&w+1<cap;i++)dst[w++]='\\';
  if(w+1<cap)dst[w++]='"';
  dst[w]=0;
}

// Start the helper, appending its stdout/stderr to <data_dir>/helper.log.
// wait != 0 blocks up to timeout_seconds and reports the exit status.
static int tz_win_run(const char *exe,const char *script,const char *logpath,
                      int wait,long timeout_seconds) {
  char exe_arg[CODEX_TZ_PATH_CAP*2+8],script_arg[CODEX_TZ_PATH_CAP*2+8];
  char command[CODEX_TZ_PATH_CAP*4+16];
  tz_win_quote(exe_arg,sizeof(exe_arg),exe);
  tz_win_quote(script_arg,sizeof(script_arg),script);
  int n=snprintf(command,sizeof(command),"%s %s",exe_arg,script_arg);
  if(n<=0||(size_t)n>=sizeof(command))return -1;

  SECURITY_ATTRIBUTES attrs={sizeof(attrs),NULL,TRUE};
  HANDLE log=CreateFileA(logpath,FILE_APPEND_DATA,
    FILE_SHARE_READ|FILE_SHARE_WRITE,&attrs,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  int have_log=log!=INVALID_HANDLE_VALUE;

  STARTUPINFOA info;
  memset(&info,0,sizeof(info));
  info.cb=sizeof(info);
  if(have_log) {
    info.dwFlags=STARTF_USESTDHANDLES;
    info.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    info.hStdOutput=log;
    info.hStdError=log;
  }
  PROCESS_INFORMATION process;
  memset(&process,0,sizeof(process));
  // CREATE_NO_WINDOW: no console flash from a GUI-hosted CPA process.
  BOOL started=CreateProcessA(exe,command,
    have_log?&attrs:NULL,have_log?&attrs:NULL,have_log?TRUE:FALSE,
    CREATE_NO_WINDOW,NULL,NULL,&info,&process);
  if(have_log)CloseHandle(log);
  if(!started)return -1;
  CloseHandle(process.hThread);
  if(!wait) {
    CloseHandle(process.hProcess);
    return 0;
  }
  long seconds=timeout_seconds<5?5:timeout_seconds;
  DWORD result=WaitForSingleObject(process.hProcess,(DWORD)seconds*1000);
  if(result==WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess,1);
    WaitForSingleObject(process.hProcess,INFINITE);
    CloseHandle(process.hProcess);
    return 1;
  }
  DWORD code=1;
  GetExitCodeProcess(process.hProcess,&code);
  CloseHandle(process.hProcess);
  return code==0?0:1;
}

// Run refresh_egress.py once and wait for it. 0 = exited 0,
// 1 = ran but failed (or timed out), -1 = could not be started.
static int tz_refresh_once(long timeout_seconds) {
  char *argv[3];
  if(!tz_helper_argv(argv,3))return -1;
  const char *dir=tz_data_dir();
  if(dir && *dir)tz_ensure_dir(dir);
  char logpath[CODEX_TZ_PATH_CAP];
  snprintf(logpath,sizeof(logpath),"%s",tz_join("helper.log"));
  int rc=tz_win_run(argv[0],argv[1]?argv[1]:"",logpath,1,timeout_seconds);
  if(rc<0)tz_helper_log("on-demand refresh: spawn failed");
  else if(rc==0)tz_helper_log("on-demand refresh: completed");
  else tz_helper_log("on-demand refresh: helper failed");
  return rc;
}

// Fire-and-forget: the helper runs detached and is never waited for.
static void tz_refresh_background(void) {
  char *argv[3];
  if(!tz_helper_argv(argv,3))return;
  const char *dir=tz_data_dir();
  if(dir && *dir)tz_ensure_dir(dir);
  char logpath[CODEX_TZ_PATH_CAP];
  snprintf(logpath,sizeof(logpath),"%s",tz_join("helper.log"));
  tz_win_run(argv[0],argv[1]?argv[1]:"",logpath,0,0);
}
#else
// Run refresh_egress.py once and wait for it.
// Returns 0 when it exited 0, 1 when it ran but failed (or timed out),
// -1 when it could not be spawned (helper script or interpreter missing).
static int tz_refresh_once(long timeout_seconds) {
  char *argv[3];
  if(!tz_helper_argv(argv,3))return -1;
  const char *dir=tz_data_dir();
  if(dir && *dir)tz_ensure_dir(dir);
  char logpath[CODEX_TZ_PATH_CAP];
  snprintf(logpath,sizeof(logpath),"%s",tz_join("helper.log"));
  posix_spawn_file_actions_t actions;
  int actions_ready=posix_spawn_file_actions_init(&actions)==0;
  if(actions_ready) {
    posix_spawn_file_actions_addopen(&actions,1,logpath,O_WRONLY|O_CREAT|O_APPEND,0600);
    posix_spawn_file_actions_adddup2(&actions,1,2);
  }
  pid_t pid=0;
  int rc=posix_spawnp(&pid,argv[0],actions_ready?&actions:NULL,NULL,argv,environ);
  if(actions_ready)posix_spawn_file_actions_destroy(&actions);
  if(rc!=0) {
    tz_helper_log("on-demand refresh: spawn failed");
    return -1;
  }
  if(timeout_seconds<5)timeout_seconds=5;
  time_t deadline=time(NULL)+timeout_seconds;
  for(;;) {
    int status=0;
    pid_t done=waitpid(pid,&status,WNOHANG);
    if(done==pid) {
      int ok=WIFEXITED(status)&&WEXITSTATUS(status)==0;
      tz_helper_log(ok?"on-demand refresh: completed":"on-demand refresh: helper failed");
      return ok?0:1;
    }
    if(done<0)return 1;
    if(time(NULL)>=deadline) {
      kill(pid,SIGKILL);
      while(waitpid(pid,&status,0)<0&&errno==EINTR){}
      tz_helper_log("on-demand refresh: timed out and was killed");
      return 1;
    }
    struct timespec pause={0,200000000L};
    nanosleep(&pause,NULL);
  }
}

// Fire-and-forget refresh for self-healing. The intermediate child is reaped
// here; the grandchild is reparented to launchd, so no zombie accumulates.
// Only async-signal-safe calls run between fork and exec.
static void tz_refresh_background(void) {
  char *argv[3];
  if(!tz_helper_argv(argv,3))return;
  const char *dir=tz_data_dir();
  if(dir && *dir)tz_ensure_dir(dir);
  char logpath[CODEX_TZ_PATH_CAP];
  snprintf(logpath,sizeof(logpath),"%s",tz_join("helper.log"));
  fflush(NULL);
  pid_t child=fork();
  if(child<0)return;
  if(child==0) {
    int fd=open(logpath,O_WRONLY|O_CREAT|O_APPEND,0600);
    if(fd<0)fd=open("/dev/null",O_WRONLY);
    if(fd>=0) {
      if(fd!=1)dup2(fd,1);
      if(fd!=2)dup2(fd,2);
      if(fd>2)close(fd);
    }
    pid_t grand=fork();
    if(grand==0) {
      execv(argv[0],argv);
      _exit(127);
    }
    _exit(grand<0?1:0);
  }
  int status=0;
  while(waitpid(child,&status,0)<0&&errno==EINTR){}
}
#endif

// Mirrors refresh_egress.py: CODEX_TZ_REFRESH_INTERVAL, default 900 seconds,
// 0 disables periodic refresh (and self-healing with it).
static long tz_refresh_interval(void) {
  const char *raw=getenv("CODEX_TZ_REFRESH_INTERVAL");
  if(!raw||!*raw)return 900;
  char *end=NULL;
  long value=strtol(raw,&end,10);
  if(end==raw||value<0)return 900;
  return value;
}

// Self-heal: when the egress state file is missing or staler than the refresh
// interval, run one background refresh. Rate limited to once per interval so
// the request path never spawns more than that.
static void tz_maybe_background_refresh(void) {
  static plugin_mutex_t tz_refresh_mutex=PLUGIN_MUTEX_INITIALIZER;
  static time_t tz_last_refresh_attempt=0;
  long interval=tz_refresh_interval();
  if(interval<=0)return;
  const char *state=tz_state_path();
  struct stat st;
  long age=-1;
  if(stat(state,&st)==0)age=(long)difftime(time(NULL),st.st_mtime);
  if(age>=0&&age<interval)return; // fresh enough
  plugin_mutex_lock(&tz_refresh_mutex);
  time_t now=time(NULL);
  if(tz_last_refresh_attempt!=0&&difftime(now,tz_last_refresh_attempt)<(double)interval) {
    plugin_mutex_unlock(&tz_refresh_mutex);
    return;
  }
  tz_last_refresh_attempt=now;
  plugin_mutex_unlock(&tz_refresh_mutex);
  tz_refresh_background();
}
#endif // CODEX_TZ_SPAWN_REFRESH_INC

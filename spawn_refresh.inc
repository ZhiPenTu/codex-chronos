// On-demand egress refresh so the plugin works with no daemon installed.
//
// The preferred transport is the refresh_egress.py --serve Unix socket (see
// refresh_bridge.inc). When nothing listens there — the common desktop case,
// where the user never starts a background job — the plugin runs the helper
// itself:
//
//   * tz_refresh_once()      one-shot, blocking; used by the management
//                            "egress/refresh" route (the 获取 button).
//   * tz_refresh_background() fire-and-forget, zombie-free (double fork);
//                            used by tz_maybe_background_refresh() to keep
//                            the state file fresh from the request path.
//
// Helper script lookup order: CODEX_TZ_HELPER > data directory > plugin
// library directory > EasyCLIProxyAPI app data > ~/.codex-chronos >
// /usr/local/lib/codex-chronos. Interpreter: CODEX_TZ_PYTHON > system
// python3 > PATH lookup > the script itself when it is executable.
// refresh_egress.py mirrors the data path resolution in paths.inc.
#ifndef CODEX_TZ_SPAWN_REFRESH_INC
#define CODEX_TZ_SPAWN_REFRESH_INC

#include <time.h>

#ifdef _WIN32
// Windows keeps the mailbox-based --serve daemon; no on-demand spawn yet.
static int tz_refresh_once(long timeout_seconds){(void)timeout_seconds;return -1;}
static void tz_refresh_background(void) {}
static long tz_refresh_interval(void) { return 900; }
static void tz_maybe_background_refresh(void) {}
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// Not exposed under -std=c11 on macOS; the symbol exists in libSystem.
extern char **environ;

#ifndef CODEX_TZ_HELPER_SCRIPT
#define CODEX_TZ_HELPER_SCRIPT "refresh_egress.py"
#endif

static int tz_file_exists(const char *path) {
  struct stat st;
  return path && *path && stat(path,&st)==0 && S_ISREG(st.st_mode);
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
    snprintf(out,sizeof(out),"%s/%s",dir,CODEX_TZ_HELPER_SCRIPT);
    if(tz_file_exists(out))return out;
  }
  // Next to the loaded plugin library (manual drop-in installs).
  Dl_info info;
  memset(&info,0,sizeof(info));
  if(dladdr((void *)tz_helper_script,&info) && info.dli_fname && *info.dli_fname) {
    char folder[CODEX_TZ_PATH_CAP];
    snprintf(folder,sizeof(folder),"%s",info.dli_fname);
    char *cut=strrchr(folder,'/');
    if(cut && cut!=folder) {
      *cut=0;
      snprintf(out,sizeof(out),"%s/%s",folder,CODEX_TZ_HELPER_SCRIPT);
      if(tz_file_exists(out))return out;
    }
  }
  const char *home=getenv("HOME");
  if(home && *home) {
    // EasyCLIProxyAPI desktop layout (see README).
    snprintf(out,sizeof(out),"%s/Library/Application Support/com.cpa.gui/%s/%s",
      home,CODEX_TZ_DIRNAME,CODEX_TZ_HELPER_SCRIPT);
    if(tz_file_exists(out))return out;
    snprintf(out,sizeof(out),"%s/.%s/%s",home,CODEX_TZ_DIRNAME,CODEX_TZ_HELPER_SCRIPT);
    if(tz_file_exists(out))return out;
  }
  snprintf(out,sizeof(out),"/usr/local/lib/%s/%s",CODEX_TZ_DIRNAME,CODEX_TZ_HELPER_SCRIPT);
  if(tz_file_exists(out))return out;
  return NULL;
}

// Absolute python interpreter path, or NULL when none is available.
static const char *tz_python_absolute(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env=getenv("CODEX_TZ_PYTHON");
  if(env && *env && strlen(env)<sizeof(out) && access(env,X_OK)==0) {
    snprintf(out,sizeof(out),"%s",env);
    return out;
  }
  static const char *candidates[]={
    "/usr/bin/python3","/usr/local/bin/python3","/opt/homebrew/bin/python3",
    "/usr/bin/python","/usr/local/bin/python",NULL};
  for(int i=0;candidates[i];i++) {
    if(access(candidates[i],X_OK)==0) {
      snprintf(out,sizeof(out),"%s",candidates[i]);
      return out;
    }
  }
  // Manual PATH search so the forked child can execv() without allocating.
  const char *path=getenv("PATH");
  if(path && *path) {
    char folder[CODEX_TZ_PATH_CAP];
    snprintf(folder,sizeof(folder),"%s",path);
    for(char *cursor=folder;*cursor;) {
      char *sep=strchr(cursor,':');
      if(sep)*sep=0;
      if(*cursor) {
        snprintf(out,sizeof(out),"%s/python3",cursor);
        if(access(out,X_OK)==0)return out;
        snprintf(out,sizeof(out),"%s/python",cursor);
        if(access(out,X_OK)==0)return out;
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
  if(access(script,X_OK)==0) {
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
// the request path never forks more than that.
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
#endif // !_WIN32
#endif // CODEX_TZ_SPAWN_REFRESH_INC

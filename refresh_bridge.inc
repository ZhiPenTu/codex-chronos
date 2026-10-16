// Ask a host-side helper to refresh the actual egress timezone.
// TCP/network credentials never cross the CPA public plugin resource route.

// Build the 503 body for a failed egress refresh. helper_error is NULL when
// the helper could not be reached at all, or a short reason when the helper
// itself answered ERR (surfaced from refresh_egress.py).
static void refresh_failure(cliproxy_buffer *out, const char *path,
                            const char *helper_error) {
  char safe_path[CODEX_TZ_PATH_CAP*2];
  char safe_error[512];
  char reason[CODEX_TZ_PATH_CAP*4];
  tz_json_escape(safe_path,sizeof(safe_path),path);
  tz_json_escape(safe_error,sizeof(safe_error),helper_error);
  if (safe_error[0]) {
    snprintf(reason,sizeof(reason),
      "{\"ok\":false,\"reason\":\"出口检测失败：刷新服务返回错误（%s）。请查看 refresh_egress.py 运行日志；如经代理出口，请将 CODEX_TZ_PROXY 指向该代理（直连可设为 direct）\"}",
      safe_error);
  } else {
    snprintf(reason,sizeof(reason),
      "{\"ok\":false,\"reason\":\"出口检测失败：无法连接刷新服务 %s。请确认主机端 refresh_egress.py --serve 正在运行；如经代理出口，请将 CODEX_TZ_PROXY 指向该代理（直连可设为 direct）\"}",
      safe_path);
  }
  http_response(out,503,"application/json",reason);
}

// Extract a short helper error reason from an "ERR <reason>\n" reply.
static const char *helper_error_from(const char *response, char *buf, size_t cap) {
  if (!response || strncmp(response,"ERR",3)!=0) return NULL;
  size_t w=0;
  for (size_t i=4; response[i] && response[i]!='\n' && response[i]!='\r' && w+1<cap; i++) {
    if (response[i]!='\t') buf[w++]=(char)(unsigned char)response[i];
  }
  buf[w]=0;
  return w ? buf : "未知错误";
}

#ifdef _WIN32
// Windows' standard Python distribution has no AF_UNIX. Use a private,
// shared-directory request/reply mailbox consumed by refresh_egress.py --serve.
static plugin_mutex_t refresh_mutex=PLUGIN_MUTEX_INITIALIZER;
static void handle_probe_egress(cliproxy_buffer *out) {
  const char *path=tz_refresh_socket_path();
  if(strlen(path)>800) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"invalid refresh path\"}");return;
  }
  char request[1024],temporary[1024],response[1024],ready[1024],nonce[32];
  snprintf(request,sizeof(request),"%s.request",path);
  snprintf(ready,sizeof(ready),"%s.ready",path);
  if(GetFileAttributesA(ready)==INVALID_FILE_ATTRIBUTES) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"refresh helper unavailable\"}");return;
  }
  plugin_mutex_lock(&refresh_mutex);
  static LONG seq=0;
  unsigned long long unique=(unsigned long long)GetTickCount() ^
    ((unsigned long long)GetCurrentProcessId()<<32) ^
    (unsigned long long)InterlockedIncrement(&seq);
  snprintf(nonce,sizeof(nonce),"%016llx",unique);
  snprintf(temporary,sizeof(temporary),"%s.request.%s.tmp",path,nonce);
  snprintf(response,sizeof(response),"%s.%s.reply",path,nonce);
  FILE *f=fopen(temporary,"wb");
  int published=0;
  if(f) {
    int written=fprintf(f,"%s\n",nonce);
    if(fclose(f)==0 && written==17)
      published=MoveFileExA(temporary,request,
        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
  }
  if(!published)DeleteFileA(temporary);
  int ok=0;
  char helper_error[512]="";
  if(published)for(int i=0;i<160;i++) {
    FILE *reply=fopen(response,"rb");
    if(reply) {
      char line[256]={0};
      ok=fgets(line,sizeof(line),reply)!=NULL && line[0]=='O' && line[1]=='K' &&
        (line[2]=='\n'||line[2]=='\r');
      if(!ok) {
        const char *err=helper_error_from(line,helper_error,sizeof(helper_error));
        if(err && err!=helper_error)snprintf(helper_error,sizeof(helper_error),"%s",err);
      }
      fclose(reply);break;
    }
    Sleep(150);
  }
  if(!ok)DeleteFileA(request);
  DeleteFileA(response);
  plugin_mutex_unlock(&refresh_mutex);
  if(!ok) {
    refresh_failure(out,path,helper_error[0]?helper_error:NULL);return;
  }
  sync_state fresh;
  if(!load_state(&fresh)) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"出口检测后状态仍不可用\"}");return;
  }
  char json[320];
  snprintf(json,sizeof(json),"{\"ok\":true,\"timezone\":\"%s\",\"ip\":\"%s\",\"offset\":\"%s\"}",
    fresh.zone,fresh.ip,fresh.offset);
  http_response(out,200,"application/json",json);
}
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static void handle_probe_egress(cliproxy_buffer *out) {
  const char *path=tz_refresh_socket_path();
  struct sockaddr_un addr;
  memset(&addr,0,sizeof(addr));
  addr.sun_family=AF_UNIX;
  if(strlen(path)>=sizeof(addr.sun_path)) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"invalid refresh socket path\"}");return;
  }
  memcpy(addr.sun_path,path,strlen(path)+1);
  int fd=socket(AF_UNIX,SOCK_STREAM,0);
  if(fd<0) {http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"refresh helper unavailable\"}");return;}
#ifdef SO_NOSIGPIPE
  int no_sigpipe=1;
  setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
  struct timeval tv={.tv_sec=25,.tv_usec=0};
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  int connected=connect(fd,(struct sockaddr *)&addr,sizeof(addr))==0;
  int sent=connected && send(fd,"REFRESH\n",8,MSG_NOSIGNAL)==8;
  char response[256]={0};
  ssize_t got=sent?recv(fd,response,sizeof(response)-1,0):-1;
  close(fd);
  if(got<3 || memcmp(response,"OK\n",3)!=0) {
    char helper_error[512]="";
    const char *err=helper_error_from(response,helper_error,sizeof(helper_error));
    refresh_failure(out,path,err);return;
  }
  sync_state fresh;
  if(!load_state(&fresh)) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"出口检测后状态仍不可用\"}");return;
  }
  char json[320];
  snprintf(json,sizeof(json),"{\"ok\":true,\"timezone\":\"%s\",\"ip\":\"%s\",\"offset\":\"%s\"}",
    fresh.zone,fresh.ip,fresh.offset);
  http_response(out,200,"application/json",json);
}
#endif

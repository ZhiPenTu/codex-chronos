// Desktop-aware data path resolution shared by every persistent plugin file.
//
// Data directory resolution order:
//   1. CODEX_TZ_DATA_DIR environment variable
//   2. data_dir from plugins.configs (CPA config_yaml, editable in the
//      management UI; see ConfigFields in plugin.c)
//   3. /CLIProxyAPI/data when that directory exists (container default,
//      unchanged behavior for CPAMC-style deployments)
//   4. per-user fallback so desktop (non-container) runs work out of the box:
//        macOS   ~/.codex-chronos
//        Linux   ${XDG_DATA_HOME:-~/.local/share}/codex-chronos
//        Windows %LOCALAPPDATA%\codex-chronos
//
// Per-file environment variables (CODEX_TZ_STATE_FILE, CODEX_TZ_CONFIG_FILE,
// CODEX_TZ_LOG_FILE, CODEX_TZ_STATS_FILE, CODEX_TZ_FAILURE_FILE,
// CODEX_TZ_REFRESH_SOCKET) always win over the data directory.
// refresh_egress.py mirrors this resolution exactly; keep both in sync.
#ifndef CODEX_TZ_PATHS_INC
#define CODEX_TZ_PATHS_INC

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define CODEX_TZ_MKDIR(path) _mkdir(path)
#else
#define CODEX_TZ_MKDIR(path) mkdir((path), 0700)
#endif

#ifndef S_ISDIR
#ifdef _WIN32
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#else
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#endif

#define CODEX_TZ_CONTAINER_DIR "/CLIProxyAPI/data"
#define CODEX_TZ_DIRNAME "codex-chronos"
#define CODEX_TZ_PATH_CAP 1024

// data_dir received from CPA plugins.configs (config_yaml); empty when unset.
static char tz_config_data_dir[CODEX_TZ_PATH_CAP] = "";
static plugin_mutex_t tz_config_mutex = PLUGIN_MUTEX_INITIALIZER;

static void tz_set_config_data_dir(const char *dir) {
  plugin_mutex_lock(&tz_config_mutex);
  if (dir && *dir && strlen(dir) < sizeof(tz_config_data_dir)) {
    snprintf(tz_config_data_dir, sizeof(tz_config_data_dir), "%s", dir);
  } else {
    tz_config_data_dir[0] = 0;
  }
  plugin_mutex_unlock(&tz_config_mutex);
}

static void tz_copy_config_data_dir(char *out, size_t cap) {
  out[0] = 0;
  plugin_mutex_lock(&tz_config_mutex);
  snprintf(out, cap, "%s", tz_config_data_dir);
  plugin_mutex_unlock(&tz_config_mutex);
}

static int tz_dir_exists(const char *path) {
  struct stat st;
  return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p for a directory path; returns 0 when it exists afterwards.
static int tz_ensure_dir(const char *path) {
  if (!path || !*path) return -1;
  if (tz_dir_exists(path)) return 0;
  char buf[CODEX_TZ_PATH_CAP];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) return -1;
  memcpy(buf, path, n + 1);
  for (size_t i = 1; i < n; i++) {
    if (buf[i] != '/' && buf[i] != '\\') continue;
    buf[i] = 0;
    size_t len = i;
    int drive_only = (len == 2 && buf[1] == ':');
    if (!drive_only && !tz_dir_exists(buf)) {
      if (CODEX_TZ_MKDIR(buf) != 0 && errno != EEXIST) {
        buf[i] = path[i];
        return -1;
      }
    }
    buf[i] = path[i];
  }
  if (!tz_dir_exists(buf) && CODEX_TZ_MKDIR(buf) != 0 && errno != EEXIST) return -1;
  return tz_dir_exists(buf) ? 0 : -1;
}

// mkdir -p for the parent directory of a file path.
static int tz_ensure_parent(const char *path) {
  if (!path || !*path) return -1;
  char buf[CODEX_TZ_PATH_CAP];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) return -1;
  memcpy(buf, path, n + 1);
  char *cut = strrchr(buf, '/');
#ifdef _WIN32
  char *win = strrchr(buf, '\\');
  if (win && (!cut || win > cut)) cut = win;
#endif
  if (!cut || cut == buf) return 0; // no parent component, or parent is the root
  *cut = 0;
  return tz_ensure_dir(buf);
}

// Resolve the base data directory (no per-file override).
static const char *tz_data_dir(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_DATA_DIR");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  char configured[CODEX_TZ_PATH_CAP];
  tz_copy_config_data_dir(configured, sizeof(configured));
  if (configured[0]) {
    snprintf(out, sizeof(out), "%s", configured);
    return out;
  }
  if (tz_dir_exists(CODEX_TZ_CONTAINER_DIR)) {
    snprintf(out, sizeof(out), "%s", CODEX_TZ_CONTAINER_DIR);
    return out;
  }
#ifdef _WIN32
  const char *base = getenv("LOCALAPPDATA");
  if (!base || !*base) base = getenv("APPDATA");
  if (!base || !*base) base = getenv("USERPROFILE");
  if (!base || !*base) base = getenv("HOME");
  if (!base || !*base) {
    out[0] = 0;
    return out;
  }
  snprintf(out, sizeof(out), "%s\\%s", base, CODEX_TZ_DIRNAME);
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  if (!home || !*home) {
    out[0] = 0;
    return out;
  }
  snprintf(out, sizeof(out), "%s/.%s", home, CODEX_TZ_DIRNAME);
#else
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  if (xdg && *xdg) {
    snprintf(out, sizeof(out), "%s/%s", xdg, CODEX_TZ_DIRNAME);
  } else if (home && *home) {
    snprintf(out, sizeof(out), "%s/.local/share/%s", home, CODEX_TZ_DIRNAME);
  } else {
    out[0] = 0;
    return out;
  }
#endif
  return out;
}

// Join a data file name onto the resolved data directory.
static const char *tz_join(const char *filename) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *dir = tz_data_dir();
  if (!dir || !*dir) {
    out[0] = 0;
    return out;
  }
  snprintf(out, sizeof(out), "%s/%s", dir, filename);
  return out;
}

static const char *tz_state_path(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_STATE_FILE");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  snprintf(out, sizeof(out), "%s", tz_join("codex-chronos.state"));
  return out;
}

static const char *tz_config_path(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_CONFIG_FILE");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  snprintf(out, sizeof(out), "%s", tz_join("codex-chronos.config"));
  return out;
}

static const char *tz_log_path(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_LOG_FILE");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  snprintf(out, sizeof(out), "%s", tz_join("codex-chronos.events"));
  return out;
}

static const char *tz_stats_path(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_STATS_FILE");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  snprintf(out, sizeof(out), "%s", tz_join("codex-chronos.stats"));
  return out;
}

static const char *tz_failure_path(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_FAILURE_FILE");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  snprintf(out, sizeof(out), "%s", tz_join("codex-chronos.failures"));
  return out;
}

static const char *tz_refresh_socket_path(void) {
  static char out[CODEX_TZ_PATH_CAP];
  const char *env = getenv("CODEX_TZ_REFRESH_SOCKET");
  if (env && *env) {
    snprintf(out, sizeof(out), "%s", env);
    return out;
  }
  snprintf(out, sizeof(out), "%s", tz_join("codex-chronos-refresh.sock"));
  return out;
}

// Escape a path for embedding in a JSON string literal.
static void tz_json_escape(char *dst, size_t cap, const char *src) {
  if (!dst || !cap) return;
  if (!src) src = "";
  size_t w = 0;
  for (size_t i = 0; src[i] && w + 7 < cap; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\') {
      dst[w++] = '\\';
      dst[w++] = (char)c;
    } else if (c < 0x20) {
      continue; // drop control characters
    } else {
      dst[w++] = (char)c;
    }
  }
  dst[w] = 0;
}

#endif // CODEX_TZ_PATHS_INC

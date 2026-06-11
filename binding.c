#include <assert.h>
#include <bare.h>
#include <js.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>

typedef utf8_t sqlite3_native_path_t[4096];

typedef void (*sqlite3_native_dlsym_t)(void);

typedef struct {
  sqlite3 *handle;

  js_env_t *env;

  js_threadsafe_function_t *on_result;
} sqlite3_native_t;

typedef struct {
  sqlite3_vfs handle;

  char name[64];
  char dlerror[256];

  js_env_t *env;
  js_ref_t *ctx;

  js_threadsafe_function_t *on_access;
  js_threadsafe_function_t *on_size;
  js_threadsafe_function_t *on_read;
  js_threadsafe_function_t *on_write;
  js_threadsafe_function_t *on_delete;

  uv_sem_t done;
} sqlite3_native_vfs_t;

typedef struct {
  sqlite3_file handle;

  int type;

  sqlite3_native_vfs_t *vfs;
} sqlite3_native_file_t;

typedef struct {
  sqlite3_native_file_t *file;

  void *buf;
  int len;
  int64_t offset;

  int status;
} sqlite3_native_read_t;

typedef struct {
  sqlite3_native_file_t *file;

  const void *buf;
  int len;
  int64_t offset;

  int status;
} sqlite3_native_write_t;

typedef struct {
  sqlite3_native_file_t *file;

  int64_t size;

  int status;
} sqlite3_native_size_t;

typedef struct {
  sqlite3_native_vfs_t *vfs;

  const char *name;
  int flags;
  bool exists;

  int status;
} sqlite3_native_access_t;

typedef struct {
  sqlite3_native_vfs_t *vfs;

  const char *name;
  bool sync;

  int status;
} sqlite3_native_delete_t;

typedef struct {
  sqlite3_vfs handle;

  char name[64];
  char dlerror[256];

  // ^ the fields above must mirror sqlite3_native_vfs_t: open() reads ->name
  // and the shared dl* callbacks read ->dlerror through that type.

  js_env_t *env;
  js_ref_t *ctx;

  js_threadsafe_function_t *on_miss;

  uv_sem_t done;
  uv_mutex_t lock;

  sqlite3_native_path_t path;
  int page_size;

  uv_file fd;
  uv_file bitmap_fd;
  uint8_t *bitmap;
  size_t bitmap_len;
} sqlite3_native_cache_vfs_t;

typedef struct {
  sqlite3_file handle;

  int type;
  uv_file fd;
  bool delete_on_close;

  sqlite3_native_cache_vfs_t *vfs;
} sqlite3_native_cache_file_t;

typedef struct {
  sqlite3_native_cache_vfs_t *vfs;

  void *buf;
  int64_t index;

  int status;
} sqlite3_native_cache_miss_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  sqlite3_native_path_t name;
  sqlite3_native_vfs_t *vfs;
  bool extensions;
} sqlite3_native_open_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;
} sqlite3_native_close_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  utf8_t *query;

  js_ref_t *result;
  uint32_t i;

  int len;
  char **rows;
  char **columns;

  char *error;

  uv_sem_t done;
} sqlite3_native_exec_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  utf8_t *path;
  utf8_t *entry;

  char *error;
} sqlite3_native_load_extension_t;

static const size_t sqlite3_native__queue_limit = 64;

static bool
sqlite3_native__ends_with(const char *string, const char *suffix) {
  size_t string_len = strlen(string);
  size_t suffix_len = strlen(suffix);

  if (suffix_len > string_len) return false;

  return strncmp(string + string_len - suffix_len, suffix, suffix_len) == 0;
}

static bool
sqlite3_native__is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

static bool
sqlite3_native__is_super_journal(const char *name) {
  size_t len = strlen(name);

  if (len < 12) return false;

  const char *suffix = name + len - 12;

  if (suffix[0] != '-' || suffix[1] != 'm' || suffix[2] != 'j') return false;
  if (suffix[9] != '9') return false;

  for (size_t i = 3; i < 9; i++) {
    if (!sqlite3_native__is_hex_digit(suffix[i])) return false;
  }

  for (size_t i = 10; i < 12; i++) {
    if (!sqlite3_native__is_hex_digit(suffix[i])) return false;
  }

  return true;
}

enum {
  SQLITE3_NATIVE_FILE_MAIN_DB = 0,
  SQLITE3_NATIVE_FILE_MAIN_JOURNAL = 1,
  SQLITE3_NATIVE_FILE_WAL = 2,
  SQLITE3_NATIVE_FILE_TEMP_DB = 3,
  SQLITE3_NATIVE_FILE_TEMP_JOURNAL = 4,
  SQLITE3_NATIVE_FILE_TRANSIENT_DB = 5,
  SQLITE3_NATIVE_FILE_SUBJOURNAL = 6,
  SQLITE3_NATIVE_FILE_SUPER_JOURNAL = 7,
};

static int
sqlite3_native__get_file_type(int flags) {
  if (flags & SQLITE_OPEN_MAIN_DB) return SQLITE3_NATIVE_FILE_MAIN_DB;
  if (flags & SQLITE_OPEN_MAIN_JOURNAL) return SQLITE3_NATIVE_FILE_MAIN_JOURNAL;
  if (flags & SQLITE_OPEN_WAL) return SQLITE3_NATIVE_FILE_WAL;
  if (flags & SQLITE_OPEN_TEMP_DB) return SQLITE3_NATIVE_FILE_TEMP_DB;
  if (flags & SQLITE_OPEN_TEMP_JOURNAL) return SQLITE3_NATIVE_FILE_TEMP_JOURNAL;
  if (flags & SQLITE_OPEN_TRANSIENT_DB) return SQLITE3_NATIVE_FILE_TRANSIENT_DB;
  if (flags & SQLITE_OPEN_SUBJOURNAL) return SQLITE3_NATIVE_FILE_SUBJOURNAL;
  if (flags & SQLITE_OPEN_SUPER_JOURNAL) return SQLITE3_NATIVE_FILE_SUPER_JOURNAL;

  return -1;
}

static int
sqlite3_native__get_file_type_from_name(const char *name) {
  if (sqlite3_native__ends_with(name, "-journal")) return SQLITE3_NATIVE_FILE_MAIN_JOURNAL;
  if (sqlite3_native__ends_with(name, "-wal")) return SQLITE3_NATIVE_FILE_WAL;
  if (sqlite3_native__is_super_journal(name)) return SQLITE3_NATIVE_FILE_SUPER_JOURNAL;

  return SQLITE3_NATIVE_FILE_MAIN_DB;
}

static int
sqlite3_native__error_from(js_env_t *env, js_value_t *value, int code) {
  int err;

  js_value_type_t type;
  err = js_typeof(env, value, &type);
  assert(err == 0);

  if (type == js_null || type == js_undefined) return SQLITE_OK;

  return code;
}

static int
sqlite3_native__on_vfs_close(sqlite3_file *handle) {
  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_read_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_read_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_READ);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_read_call(js_env_t *env, js_value_t *on_read, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_read_t *data = (sqlite3_native_read_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[4];

  err = js_create_uint32(env, data->file->type, &args[0]);
  assert(err == 0);

  err = js_create_external_arraybuffer(env, data->buf, data->len, NULL, NULL, &args[1]);
  assert(err == 0);

  err = js_create_int64(env, data->offset, &args[2]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_read_done, (void *) data, &args[3]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_read, 4, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_read(sqlite3_file *handle, void *buf, int len, sqlite3_int64 offset) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_read_t data = {
    file,
    buf,
    len,
    offset
  };

  err = js_call_threadsafe_function(vfs->on_read, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  return data.status;
}

static js_value_t *
sqlite3_native__on_vfs_write_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_write_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_WRITE);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_write_call(js_env_t *env, js_value_t *on_write, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_write_t *data = (sqlite3_native_write_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[4];

  err = js_create_uint32(env, data->file->type, &args[0]);
  assert(err == 0);

  err = js_create_external_arraybuffer(env, (void *) data->buf, data->len, NULL, NULL, &args[1]);
  assert(err == 0);

  err = js_create_int64(env, data->offset, &args[2]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_write_done, (void *) data, &args[3]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_write, 4, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_write(sqlite3_file *handle, const void *buf, int len, sqlite_int64 offset) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_write_t data = {
    file,
    buf,
    len,
    offset
  };

  err = js_call_threadsafe_function(vfs->on_write, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  return data.status;
}

static int
sqlite3_native__on_vfs_truncate(sqlite3_file *handle, sqlite_int64 size) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sync(sqlite3_file *handle, int flags) {
  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_size_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_size_t *data;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc >= 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_FSTAT);

  if (data->status == SQLITE_OK) {
    assert(argc == 2);

    err = js_get_value_int64(env, argv[1], &data->size);
    assert(err == 0);
  }

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_size_call(js_env_t *env, js_value_t *on_size, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_size_t *data = (sqlite3_native_size_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[2];

  err = js_create_uint32(env, data->file->type, &args[0]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_size_done, (void *) data, &args[1]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_size, 2, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_size(sqlite3_file *handle, sqlite_int64 *size) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_size_t data = {
    file,
  };

  err = js_call_threadsafe_function(vfs->on_size, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  if (data.status != SQLITE_OK) return data.status;

  *size = data.size;

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_lock(sqlite3_file *handle, int eLock) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_unlock(sqlite3_file *sql_file, int eLock) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_check_reserved_lock(sqlite3_file *sql_file, int *pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_control(sqlite3_file *sql_file, int op, void *pArg) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sector_size(sqlite3_file *sql_file) {
  return 0;
}

static int
sqlite3_native__on_vfs_device_characteristics(sqlite3_file *sql_file) {
  return 0;
}

static int
sqlite3_native__on_vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *handle, int flags, int *pflags) {
  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  file->type = sqlite3_native__get_file_type(flags);

  if (file->type < 0) return SQLITE_CANTOPEN;

  file->vfs = (sqlite3_native_vfs_t *) vfs;

  static const sqlite3_io_methods methods = {
    1, // Version
    sqlite3_native__on_vfs_close,
    sqlite3_native__on_vfs_read,
    sqlite3_native__on_vfs_write,
    sqlite3_native__on_vfs_truncate,
    sqlite3_native__on_vfs_sync,
    sqlite3_native__on_vfs_size,
    sqlite3_native__on_vfs_lock,
    sqlite3_native__on_vfs_unlock,
    sqlite3_native__on_vfs_check_reserved_lock,
    sqlite3_native__on_vfs_control,
    sqlite3_native__on_vfs_sector_size,
    sqlite3_native__on_vfs_device_characteristics
  };

  file->handle.pMethods = &methods;

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_delete_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_delete_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_DELETE);

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_delete_call(js_env_t *env, js_value_t *on_delete, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_delete_t *data = (sqlite3_native_delete_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  int type = sqlite3_native__get_file_type_from_name(data->name);

  js_value_t *args[2];

  err = js_create_uint32(env, type, &args[0]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_delete_done, (void *) data, &args[1]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_delete, 2, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_delete(sqlite3_vfs *handle, const char *name, int sync) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  sqlite3_native_delete_t data = {
    vfs,
    name,
    sync
  };

  err = js_call_threadsafe_function(vfs->on_delete, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  return data.status;
}

static js_value_t *
sqlite3_native__on_vfs_access_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_access_t *data;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc >= 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_ACCESS);

  if (data->status == SQLITE_OK) {
    assert(argc == 2);

    err = js_get_value_bool(env, argv[1], &data->exists);
    assert(err == 0);
  }

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_access_call(js_env_t *env, js_value_t *on_access, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_access_t *data = (sqlite3_native_access_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  int type = sqlite3_native__get_file_type_from_name(data->name);

  js_value_t *args[2];

  err = js_create_uint32(env, type, &args[0]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_access_done, (void *) data, &args[1]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_access, 2, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_access(sqlite3_vfs *handle, const char *name, int flags, int *exists) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  sqlite3_native_access_t data = {
    vfs,
    name,
    flags
  };

  err = js_call_threadsafe_function(vfs->on_access, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  if (data.status != SQLITE_OK) return data.status;

  *exists = data.exists;

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_fullpathname(sqlite3_vfs *vfs, const char *name, int len, char *out) {
  if (strlen(name) >= len) return SQLITE_ERROR;

  strcpy(out, name);

  return SQLITE_OK;
}

static void *
sqlite3_native__on_vfs_dlopen(sqlite3_vfs *handle, const char *path) {
  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  uv_lib_t *lib = malloc(sizeof(uv_lib_t));

  if (uv_dlopen(path, lib) == 0) return (void *) lib;

  snprintf(vfs->dlerror, sizeof(vfs->dlerror), "%s", uv_dlerror(lib));

  uv_dlclose(lib);

  free(lib);

  return NULL;
}

static void
sqlite3_native__on_vfs_dlerror(sqlite3_vfs *handle, int len, char *out) {
  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  snprintf(out, (size_t) len, "%s", vfs->dlerror);
}

static sqlite3_native_dlsym_t
sqlite3_native__on_vfs_dlsym(sqlite3_vfs *handle, void *lib, const char *symbol) {
  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  union {
    void *ptr;
    sqlite3_native_dlsym_t sym;
  } cast;

  if (uv_dlsym((uv_lib_t *) lib, symbol, &cast.ptr) == 0) return cast.sym;

  snprintf(vfs->dlerror, sizeof(vfs->dlerror), "%s", uv_dlerror((uv_lib_t *) lib));

  return NULL;
}

static void
sqlite3_native__on_vfs_dlclose(sqlite3_vfs *handle, void *lib) {
  uv_dlclose((uv_lib_t *) lib);

  free(lib);
}

static int
sqlite3_native__on_vfs_randomness(sqlite3_vfs *vfs, int bytes, char *buf) {
  memset(buf, 0, bytes);

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sleep(sqlite3_vfs *vfs, int nMicro) {
  return 0;
}

static int
sqlite3_native__on_vfs_current_time(sqlite3_vfs *vfs, double *time) {
  int err;

  uv_timespec64_t ts;
  err = uv_clock_gettime(UV_CLOCK_REALTIME, &ts);
  assert(err == 0);

  *time = ts.tv_sec / 86400.0 + 2440587.5;

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_vfs_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 6;
  js_value_t *argv[6];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 6);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  js_value_t *handle;

  sqlite3_native_vfs_t *vfs;
  err = js_create_arraybuffer(env, sizeof(sqlite3_native_vfs_t), (void **) &vfs, &handle);
  assert(err == 0);

  err = uv_sem_init(&vfs->done, 0);
  assert(err == 0);

  uv_random_t req;
  err = uv_random(loop, &req, vfs->name, sizeof(vfs->name), 0, NULL);
  assert(err == 0);

  vfs->name[sizeof(vfs->name) - 1] = '\0';

  vfs->env = env;

  err = js_create_reference(env, argv[0], 1, &vfs->ctx);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[1], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_access_call, &vfs->on_access);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[2], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_size_call, &vfs->on_size);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[3], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_read_call, &vfs->on_read);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[4], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_write_call, &vfs->on_write);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[5], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_delete_call, &vfs->on_delete);
  assert(err == 0);

  vfs->handle = (sqlite3_vfs) {
    1, // Version
    sizeof(sqlite3_native_file_t),
    sizeof(sqlite3_native_path_t),
    NULL,
    vfs->name,
    NULL,
    sqlite3_native__on_vfs_open,
    sqlite3_native__on_vfs_delete,
    sqlite3_native__on_vfs_access,
    sqlite3_native__on_vfs_fullpathname,
    sqlite3_native__on_vfs_dlopen,
    sqlite3_native__on_vfs_dlerror,
    sqlite3_native__on_vfs_dlsym,
    sqlite3_native__on_vfs_dlclose,
    sqlite3_native__on_vfs_randomness,
    sqlite3_native__on_vfs_sleep,
    sqlite3_native__on_vfs_current_time,
  };

  err = sqlite3_vfs_register(&vfs->handle, false);
  assert(err == 0);

  return handle;
}

static js_value_t *
sqlite3_native_vfs_destroy(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &vfs, NULL);
  assert(err == 0);

  err = sqlite3_vfs_unregister(&vfs->handle);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_access, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_size, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_read, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_write, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_delete, js_threadsafe_function_release);
  assert(err == 0);

  err = js_delete_reference(env, vfs->ctx);
  assert(err == 0);

  uv_sem_destroy(&vfs->done);

  return NULL;
}

// Cache VFS: a native file-backed VFS. All IO is plain pread/pwrite on local
// files; the main database additionally keeps a presence bitmap (sidecar
// <path>.map) and pages whose bit is unset are fetched through a JS miss
// callback before the read is served. applyPages() lets JS atomically patch
// pages into the cache file (the checkpoint path) — it must only be called
// while no statement is executing.

static const char *sqlite3_native__cache_suffixes[8] = {
  "", "-journal", "-wal", "-temp3", "-temp4", "-temp5", "-temp6", "-temp7"
};

static void
sqlite3_native__cache_path(sqlite3_native_cache_vfs_t *vfs, int type, char *out, size_t len) {
  snprintf(out, len, "%s%s", (char *) vfs->path, sqlite3_native__cache_suffixes[type]);
}

static bool
sqlite3_native__cache_has_page(sqlite3_native_cache_vfs_t *vfs, int64_t index) {
  size_t byte = (size_t) (index >> 3);

  // beyond the tracked range means beyond EOF; nothing to fetch, the read
  // will come up short and zero fill
  if (byte >= vfs->bitmap_len) return true;

  return (vfs->bitmap[byte] >> (index & 7)) & 1;
}

static int
sqlite3_native__cache_grow_bitmap(sqlite3_native_cache_vfs_t *vfs, int64_t npages) {
  size_t len = (size_t) ((npages + 7) / 8);

  if (len <= vfs->bitmap_len) return 0;

  uint8_t *bitmap = realloc(vfs->bitmap, len);

  if (bitmap == NULL) return UV_ENOMEM;

  memset(bitmap + vfs->bitmap_len, 0, len - vfs->bitmap_len);

  vfs->bitmap = bitmap;
  vfs->bitmap_len = len;

  return 0;
}

static void
sqlite3_native__cache_shrink_bitmap(sqlite3_native_cache_vfs_t *vfs, int64_t npages) {
  size_t byte = (size_t) ((npages + 7) / 8);

  if (byte < vfs->bitmap_len) {
    memset(&vfs->bitmap[byte], 0, vfs->bitmap_len - byte);
  }

  if (npages & 7 && byte > 0 && byte <= vfs->bitmap_len) {
    vfs->bitmap[byte - 1] &= (1 << (npages & 7)) - 1;
  }
}

static int
sqlite3_native__cache_set_page(sqlite3_native_cache_vfs_t *vfs, int64_t index, bool persist) {
  int err = sqlite3_native__cache_grow_bitmap(vfs, index + 1);

  if (err < 0) return err;

  size_t byte = (size_t) (index >> 3);

  vfs->bitmap[byte] |= 1 << (index & 7);

  if (persist) {
    uv_fs_t req;
    uv_buf_t buf = uv_buf_init((char *) &vfs->bitmap[byte], 1);

    int res = uv_fs_write(NULL, &req, vfs->bitmap_fd, &buf, 1, byte, NULL);
    uv_fs_req_cleanup(&req);

    if (res < 0) return res;
  }

  return 0;
}

static int
sqlite3_native__cache_flush_bitmap(sqlite3_native_cache_vfs_t *vfs) {
  uv_fs_t req;
  int res;

  uv_buf_t buf = uv_buf_init((char *) vfs->bitmap, vfs->bitmap_len);

  res = uv_fs_write(NULL, &req, vfs->bitmap_fd, &buf, 1, 0, NULL);
  uv_fs_req_cleanup(&req);

  if (res < 0) return res;

  res = uv_fs_ftruncate(NULL, &req, vfs->bitmap_fd, vfs->bitmap_len, NULL);
  uv_fs_req_cleanup(&req);

  if (res < 0) return res;

  res = uv_fs_fsync(NULL, &req, vfs->bitmap_fd, NULL);
  uv_fs_req_cleanup(&req);

  return res < 0 ? res : 0;
}

static js_value_t *
sqlite3_native__on_cache_miss_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_cache_miss_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_READ);

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_cache_miss_call(js_env_t *env, js_value_t *on_miss, void *context, void *arg) {
  int err;

  sqlite3_native_cache_vfs_t *vfs = (sqlite3_native_cache_vfs_t *) context;

  sqlite3_native_cache_miss_t *data = (sqlite3_native_cache_miss_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[3];

  err = js_create_external_arraybuffer(env, data->buf, vfs->page_size, NULL, NULL, &args[0]);
  assert(err == 0);

  err = js_create_int64(env, data->index, &args[1]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_cache_miss_done, (void *) data, &args[2]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_miss, 3, args, NULL);
  assert(err == 0);
}

// fetch a missing page through JS, write it into the cache file and mark it
// present; runs on the SQLite worker thread with vfs->lock held
static int
sqlite3_native__cache_fetch(sqlite3_native_cache_vfs_t *vfs, int64_t index) {
  int err;

  void *buf = calloc(1, vfs->page_size);

  if (buf == NULL) return SQLITE_IOERR_NOMEM;

  sqlite3_native_cache_miss_t data = {
    vfs,
    buf,
    index,
    SQLITE_OK
  };

  err = js_call_threadsafe_function(vfs->on_miss, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  if (data.status == SQLITE_OK) {
    uv_fs_t req;
    uv_buf_t b = uv_buf_init(buf, vfs->page_size);

    int res = uv_fs_write(NULL, &req, vfs->fd, &b, 1, index * vfs->page_size, NULL);
    uv_fs_req_cleanup(&req);

    if (res >= 0) {
      // the page must be durable before its presence bit
      res = uv_fs_fsync(NULL, &req, vfs->fd, NULL);
      uv_fs_req_cleanup(&req);
    }

    if (res >= 0) res = sqlite3_native__cache_set_page(vfs, index, true);

    if (res < 0) data.status = SQLITE_IOERR_WRITE;
  }

  free(buf);

  return data.status;
}

static int
sqlite3_native__on_cache_vfs_close(sqlite3_file *handle) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) handle;

  // the main db fd belongs to the vfs and stays open for applyPages()
  if (file->type == SQLITE3_NATIVE_FILE_MAIN_DB) return SQLITE_OK;

  uv_fs_t req;

  uv_fs_close(NULL, &req, file->fd, NULL);
  uv_fs_req_cleanup(&req);

  if (file->delete_on_close) {
    char path[sizeof(sqlite3_native_path_t) + 16];
    sqlite3_native__cache_path(file->vfs, file->type, path, sizeof(path));

    uv_fs_unlink(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
  }

  return SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_read(sqlite3_file *handle, void *buf, int len, sqlite3_int64 offset) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) handle;

  sqlite3_native_cache_vfs_t *vfs = file->vfs;

  bool main_db = file->type == SQLITE3_NATIVE_FILE_MAIN_DB;

  if (main_db) {
    uv_mutex_lock(&vfs->lock);

    int64_t first = offset / vfs->page_size;
    int64_t last = (offset + len - 1) / vfs->page_size;

    for (int64_t i = first; i <= last; i++) {
      if (sqlite3_native__cache_has_page(vfs, i)) continue;

      int status = sqlite3_native__cache_fetch(vfs, i);

      if (status != SQLITE_OK) {
        uv_mutex_unlock(&vfs->lock);
        return status;
      }
    }
  }

  uv_fs_t req;
  uv_buf_t b = uv_buf_init(buf, len);

  int res = uv_fs_read(NULL, &req, file->fd, &b, 1, offset, NULL);
  uv_fs_req_cleanup(&req);

  if (main_db) uv_mutex_unlock(&vfs->lock);

  if (res < 0) return SQLITE_IOERR_READ;

  if (res < len) {
    memset((char *) buf + res, 0, len - res);
    return SQLITE_IOERR_SHORT_READ;
  }

  return SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_write(sqlite3_file *handle, const void *buf, int len, sqlite_int64 offset) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) handle;

  sqlite3_native_cache_vfs_t *vfs = file->vfs;

  bool main_db = file->type == SQLITE3_NATIVE_FILE_MAIN_DB;

  if (main_db) uv_mutex_lock(&vfs->lock);

  uv_fs_t req;
  uv_buf_t b = uv_buf_init((char *) buf, len);

  int res = uv_fs_write(NULL, &req, file->fd, &b, 1, offset, NULL);
  uv_fs_req_cleanup(&req);

  if (main_db && res >= 0) {
    // a locally written page is present by definition
    int64_t first = offset / vfs->page_size;
    int64_t last = (offset + len - 1) / vfs->page_size;

    for (int64_t i = first; i <= last && res >= 0; i++) {
      res = sqlite3_native__cache_set_page(vfs, i, true);
    }
  }

  if (main_db) uv_mutex_unlock(&vfs->lock);

  return res < 0 ? SQLITE_IOERR_WRITE : SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_truncate(sqlite3_file *handle, sqlite_int64 size) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) handle;

  sqlite3_native_cache_vfs_t *vfs = file->vfs;

  bool main_db = file->type == SQLITE3_NATIVE_FILE_MAIN_DB;

  if (main_db) uv_mutex_lock(&vfs->lock);

  uv_fs_t req;

  int res = uv_fs_ftruncate(NULL, &req, file->fd, size, NULL);
  uv_fs_req_cleanup(&req);

  if (main_db && res >= 0) {
    sqlite3_native__cache_shrink_bitmap(vfs, size / vfs->page_size);
    res = sqlite3_native__cache_flush_bitmap(vfs);
  }

  if (main_db) uv_mutex_unlock(&vfs->lock);

  return res < 0 ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_sync(sqlite3_file *handle, int flags) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) handle;

  uv_fs_t req;

  int res = uv_fs_fsync(NULL, &req, file->fd, NULL);
  uv_fs_req_cleanup(&req);

  return res < 0 ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_size(sqlite3_file *handle, sqlite_int64 *size) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) handle;

  uv_fs_t req;

  int res = uv_fs_fstat(NULL, &req, file->fd, NULL);

  if (res < 0) {
    uv_fs_req_cleanup(&req);
    return SQLITE_IOERR_FSTAT;
  }

  *size = req.statbuf.st_size;

  uv_fs_req_cleanup(&req);

  return SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_open(sqlite3_vfs *handle, const char *name, sqlite3_file *file_handle, int flags, int *pflags) {
  sqlite3_native_cache_file_t *file = (sqlite3_native_cache_file_t *) file_handle;

  file->type = sqlite3_native__get_file_type(flags);

  if (file->type < 0) return SQLITE_CANTOPEN;

  file->vfs = (sqlite3_native_cache_vfs_t *) handle;
  file->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;

  if (file->type == SQLITE3_NATIVE_FILE_MAIN_DB) {
    file->fd = file->vfs->fd;
  } else {
    char path[sizeof(sqlite3_native_path_t) + 16];
    sqlite3_native__cache_path(file->vfs, file->type, path, sizeof(path));

    uv_fs_t req;

    int res = uv_fs_open(NULL, &req, path, UV_FS_O_RDWR | UV_FS_O_CREAT, 0644, NULL);
    uv_fs_req_cleanup(&req);

    if (res < 0) return SQLITE_CANTOPEN;

    file->fd = res;
  }

  static const sqlite3_io_methods methods = {
    1, // Version
    sqlite3_native__on_cache_vfs_close,
    sqlite3_native__on_cache_vfs_read,
    sqlite3_native__on_cache_vfs_write,
    sqlite3_native__on_cache_vfs_truncate,
    sqlite3_native__on_cache_vfs_sync,
    sqlite3_native__on_cache_vfs_size,
    sqlite3_native__on_vfs_lock,
    sqlite3_native__on_vfs_unlock,
    sqlite3_native__on_vfs_check_reserved_lock,
    sqlite3_native__on_vfs_control,
    sqlite3_native__on_vfs_sector_size,
    sqlite3_native__on_vfs_device_characteristics
  };

  file->handle.pMethods = &methods;

  return SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_delete(sqlite3_vfs *handle, const char *name, int sync) {
  sqlite3_native_cache_vfs_t *vfs = (sqlite3_native_cache_vfs_t *) handle;

  int type = sqlite3_native__get_file_type_from_name(name);

  char path[sizeof(sqlite3_native_path_t) + 16];
  sqlite3_native__cache_path(vfs, type, path, sizeof(path));

  uv_fs_t req;

  int res = uv_fs_unlink(NULL, &req, path, NULL);
  uv_fs_req_cleanup(&req);

  if (res < 0 && res != UV_ENOENT) return SQLITE_IOERR_DELETE;

  return SQLITE_OK;
}

static int
sqlite3_native__on_cache_vfs_access(sqlite3_vfs *handle, const char *name, int flags, int *exists) {
  sqlite3_native_cache_vfs_t *vfs = (sqlite3_native_cache_vfs_t *) handle;

  int type = sqlite3_native__get_file_type_from_name(name);

  char path[sizeof(sqlite3_native_path_t) + 16];
  sqlite3_native__cache_path(vfs, type, path, sizeof(path));

  uv_fs_t req;

  int res = uv_fs_stat(NULL, &req, path, NULL);
  uv_fs_req_cleanup(&req);

  *exists = res >= 0;

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_cache_vfs_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 4);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  js_value_t *handle;

  sqlite3_native_cache_vfs_t *vfs;
  err = js_create_arraybuffer(env, sizeof(sqlite3_native_cache_vfs_t), (void **) &vfs, &handle);
  assert(err == 0);

  err = uv_sem_init(&vfs->done, 0);
  assert(err == 0);

  err = uv_mutex_init(&vfs->lock);
  assert(err == 0);

  uv_random_t random;
  err = uv_random(loop, &random, vfs->name, sizeof(vfs->name), 0, NULL);
  assert(err == 0);

  vfs->name[sizeof(vfs->name) - 1] = '\0';

  vfs->env = env;

  err = js_create_reference(env, argv[0], 1, &vfs->ctx);
  assert(err == 0);

  err = js_get_value_string_utf8(env, argv[1], vfs->path, sizeof(vfs->path), NULL);
  assert(err == 0);

  uint32_t page_size;
  err = js_get_value_uint32(env, argv[2], &page_size);
  assert(err == 0);

  vfs->page_size = (int) page_size;

  err = js_create_threadsafe_function(env, argv[3], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_cache_miss_call, &vfs->on_miss);
  assert(err == 0);

  uv_fs_t req;

  int res = uv_fs_open(NULL, &req, (char *) vfs->path, UV_FS_O_RDWR | UV_FS_O_CREAT, 0644, NULL);
  uv_fs_req_cleanup(&req);

  if (res < 0) {
    js_throw_errorf(env, NULL, "could not open %s: %s", vfs->path, uv_strerror(res));
    return NULL;
  }

  vfs->fd = res;

  res = uv_fs_fstat(NULL, &req, vfs->fd, NULL);
  int64_t size = res < 0 ? 0 : req.statbuf.st_size;
  uv_fs_req_cleanup(&req);

  int64_t npages = (size + vfs->page_size - 1) / vfs->page_size;

  vfs->bitmap_len = (size_t) ((npages + 7) / 8);
  vfs->bitmap = calloc(vfs->bitmap_len ? vfs->bitmap_len : 1, 1);

  char bitmap_path[sizeof(sqlite3_native_path_t) + 16];
  snprintf(bitmap_path, sizeof(bitmap_path), "%s.map", (char *) vfs->path);

  res = uv_fs_open(NULL, &req, bitmap_path, UV_FS_O_RDWR | UV_FS_O_CREAT, 0644, NULL);
  uv_fs_req_cleanup(&req);

  if (res < 0) {
    js_throw_errorf(env, NULL, "could not open %s: %s", bitmap_path, uv_strerror(res));
    return NULL;
  }

  vfs->bitmap_fd = res;

  if (vfs->bitmap_len > 0) {
    uv_buf_t buf = uv_buf_init((char *) vfs->bitmap, vfs->bitmap_len);

    uv_fs_read(NULL, &req, vfs->bitmap_fd, &buf, 1, 0, NULL);
    uv_fs_req_cleanup(&req);
  }

  vfs->handle = (sqlite3_vfs) {
    1, // Version
    sizeof(sqlite3_native_cache_file_t),
    sizeof(sqlite3_native_path_t),
    NULL,
    vfs->name,
    NULL,
    sqlite3_native__on_cache_vfs_open,
    sqlite3_native__on_cache_vfs_delete,
    sqlite3_native__on_cache_vfs_access,
    sqlite3_native__on_vfs_fullpathname,
    sqlite3_native__on_vfs_dlopen,
    sqlite3_native__on_vfs_dlerror,
    sqlite3_native__on_vfs_dlsym,
    sqlite3_native__on_vfs_dlclose,
    sqlite3_native__on_vfs_randomness,
    sqlite3_native__on_vfs_sleep,
    sqlite3_native__on_vfs_current_time,
  };

  err = sqlite3_vfs_register(&vfs->handle, false);
  assert(err == 0);

  return handle;
}

// applyPages(handle, indices: Uint32Array, pages: Buffer, size: int64)
//
// Atomically patches pages into the cache file and resizes it. Pages become
// present; on shrink, bits beyond the new size are cleared. Must only be
// called while no statement is executing on the database.
static js_value_t *
sqlite3_native_cache_vfs_apply(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 4);

  sqlite3_native_cache_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &vfs, NULL);
  assert(err == 0);

  uint32_t *indices;
  size_t count;
  err = js_get_typedarray_info(env, argv[1], NULL, (void **) &indices, &count, NULL, NULL);
  assert(err == 0);

  uint8_t *pages;
  size_t pages_len;
  err = js_get_typedarray_info(env, argv[2], NULL, (void **) &pages, &pages_len, NULL, NULL);
  assert(err == 0);

  int64_t size;
  err = js_get_value_int64(env, argv[3], &size);
  assert(err == 0);

  if (pages_len < count * (size_t) vfs->page_size) {
    js_throw_error(env, NULL, "pages buffer too small");
    return NULL;
  }

  uv_mutex_lock(&vfs->lock);

  uv_fs_t req;
  int res = 0;

  if (size >= 0) {
    res = uv_fs_ftruncate(NULL, &req, vfs->fd, size, NULL);
    uv_fs_req_cleanup(&req);

    if (res >= 0) {
      int64_t npages = (size + vfs->page_size - 1) / vfs->page_size;

      sqlite3_native__cache_shrink_bitmap(vfs, npages);
      res = sqlite3_native__cache_grow_bitmap(vfs, npages);
    }
  }

  for (size_t i = 0; i < count && res >= 0; i++) {
    int64_t index = indices[i];

    uv_buf_t buf = uv_buf_init((char *) &pages[i * vfs->page_size], vfs->page_size);

    res = uv_fs_write(NULL, &req, vfs->fd, &buf, 1, index * vfs->page_size, NULL);
    uv_fs_req_cleanup(&req);

    if (res >= 0) res = sqlite3_native__cache_set_page(vfs, index, false);
  }

  if (res >= 0) {
    res = uv_fs_fsync(NULL, &req, vfs->fd, NULL);
    uv_fs_req_cleanup(&req);
  }

  if (res >= 0) res = sqlite3_native__cache_flush_bitmap(vfs);

  uv_mutex_unlock(&vfs->lock);

  if (res < 0) {
    js_throw_errorf(env, NULL, "applyPages failed: %s", uv_strerror(res));
    return NULL;
  }

  return NULL;
}

static js_value_t *
sqlite3_native_cache_vfs_destroy(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  sqlite3_native_cache_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &vfs, NULL);
  assert(err == 0);

  err = sqlite3_vfs_unregister(&vfs->handle);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_miss, js_threadsafe_function_release);
  assert(err == 0);

  err = js_delete_reference(env, vfs->ctx);
  assert(err == 0);

  uv_fs_t req;

  uv_fs_close(NULL, &req, vfs->fd, NULL);
  uv_fs_req_cleanup(&req);

  uv_fs_close(NULL, &req, vfs->bitmap_fd, NULL);
  uv_fs_req_cleanup(&req);

  free(vfs->bitmap);
  vfs->bitmap = NULL;

  uv_sem_destroy(&vfs->done);
  uv_mutex_destroy(&vfs->lock);

  return NULL;
}

static void
sqlite3_native__on_result_call(js_env_t *env, js_value_t *on_result, void *context, void *arg) {
  int err;

  sqlite3_native_t *db = (sqlite3_native_t *) context;

  sqlite3_native_exec_t *data = (sqlite3_native_exec_t *) arg;

  js_value_t *result;
  err = js_get_reference_value(env, data->result, &result);
  assert(err == 0);

  js_value_t *rows;
  err = js_create_array_with_length(env, data->len, &rows);
  assert(err == 0);

  js_value_t *columns;
  err = js_create_array_with_length(env, data->len, &columns);
  assert(err == 0);

  for (int i = 0, n = data->len; i < n; i++) {
    js_value_t *row;

    if (data->rows[i] == NULL) {
      err = js_get_null(env, &row);
      assert(err == 0);
    } else {
      err = js_create_string_utf8(env, (const utf8_t *) data->rows[i], -1, &row);
      assert(err == 0);
    }

    err = js_set_element(env, rows, i, row);
    assert(err == 0);

    js_value_t *col;
    err = js_create_string_utf8(env, (const utf8_t *) data->columns[i], -1, &col);
    assert(err == 0);

    err = js_set_element(env, columns, i, col);
    assert(err == 0);
  }

  js_value_t *entry;
  err = js_create_object(env, &entry);
  assert(err == 0);

  err = js_set_named_property(env, entry, "rows", rows);
  assert(err == 0);

  err = js_set_named_property(env, entry, "columns", columns);
  assert(err == 0);

  err = js_set_element(env, result, data->i++, entry);
  assert(err == 0);

  uv_sem_post(&data->done);
}

static int
sqlite3_native__on_result(void *arg, int len, char **rows, char **columns) {
  int err;

  sqlite3_native_exec_t *data = (sqlite3_native_exec_t *) arg;

  data->len = len;
  data->rows = rows;
  data->columns = columns;

  err = js_call_threadsafe_function(data->db->on_result, (void *) data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&data->done);

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  js_value_t *handle;

  sqlite3_native_t *db;
  err = js_create_arraybuffer(env, sizeof(sqlite3_native_t), (void **) &db, &handle);
  assert(err == 0);

  db->env = env;

  err = js_create_threadsafe_function(env, NULL, sqlite3_native__queue_limit, 1, NULL, NULL, (void *) db, sqlite3_native__on_result_call, &db->on_result);
  assert(err == 0);

  return handle;
}

static void
sqlite3_native__on_after_open(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_open_t *req = (sqlite3_native_open_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;
  err = js_get_undefined(env, &result);
  assert(err == 0);

  err = js_resolve_deferred(env, req->deferred, result);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_open(uv_work_t *handle) {
  int err;

  sqlite3_native_open_t *req = (sqlite3_native_open_t *) handle->data;

  err = sqlite3_open_v2((char *) req->name, &req->db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, req->vfs->name);
  assert(err == 0);

  if (req->extensions) {
    err = sqlite3_enable_load_extension(req->db->handle, 1);
    assert(err == 0);
  }
}

static js_value_t *
sqlite3_native_open(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 4);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &vfs, NULL);
  assert(err == 0);

  sqlite3_native_path_t name;
  err = js_get_value_string_utf8(env, argv[2], name, sizeof(name), NULL);
  assert(err == 0);

  bool extensions;
  err = js_get_value_bool(env, argv[3], &extensions);
  assert(err == 0);

  sqlite3_native_open_t *req = malloc(sizeof(sqlite3_native_open_t));

  req->db = db;
  req->vfs = vfs;
  req->extensions = extensions;

  memcpy(req->name, name, sizeof(name));

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_open, sqlite3_native__on_after_open);
  assert(err == 0);

  return promise;
}

static void
sqlite3_native__on_after_close(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_close_t *req = (sqlite3_native_close_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;
  err = js_get_undefined(env, &result);
  assert(err == 0);

  err = js_resolve_deferred(env, req->deferred, result);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_release_threadsafe_function(db->on_result, js_threadsafe_function_release);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_close(uv_work_t *handle) {
  int err;

  sqlite3_native_close_t *req = (sqlite3_native_close_t *) handle->data;

  err = sqlite3_close_v2(req->db->handle);
  assert(err == 0);
}

static js_value_t *
sqlite3_native_close(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  sqlite3_native_close_t *req = malloc(sizeof(sqlite3_native_close_t));

  req->db = db;

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_close, sqlite3_native__on_after_close);
  assert(err == 0);

  return promise;
}

static void
sqlite3_native__on_after_exec(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_exec_t *req = (sqlite3_native_exec_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;

  if (req->error) {
    js_value_t *message;
    err = js_create_string_utf8(env, (utf8_t *) req->error, -1, &message);
    assert(err == 0);

    sqlite3_free(req->error);

    err = js_create_error(env, NULL, message, &result);
    assert(err == 0);

    err = js_reject_deferred(env, req->deferred, result);
    assert(err == 0);
  } else {
    err = js_get_reference_value(env, req->result, &result);
    assert(err == 0);

    err = js_resolve_deferred(env, req->deferred, result);
    assert(err == 0);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_delete_reference(env, req->result);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_exec(uv_work_t *handle) {
  int err;

  sqlite3_native_exec_t *req = (sqlite3_native_exec_t *) handle->data;

  err = uv_sem_init(&req->done, 0);
  assert(err == 0);

  sqlite3_exec(req->db->handle, (const char *) req->query, sqlite3_native__on_result, (void *) req, &req->error);

  free(req->query);

  uv_sem_destroy(&req->done);
}

static js_value_t *
sqlite3_native_exec(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  size_t query_len;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &query_len);
  assert(err == 0);

  query_len += 1 /* NULL */;

  utf8_t *query = (utf8_t *) malloc(query_len);

  err = js_get_value_string_utf8(env, argv[1], query, query_len, NULL);
  assert(err == 0);

  js_value_t *result;
  err = js_create_array(env, &result);
  assert(err == 0);

  sqlite3_native_exec_t *req = malloc(sizeof(sqlite3_native_exec_t));

  req->db = db;
  req->query = query;
  req->i = 0;

  req->handle.data = (void *) req;

  err = js_create_reference(env, result, 1, &req->result);
  assert(err == 0);

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_exec, sqlite3_native__on_after_exec);
  assert(err == 0);

  return promise;
}

static void
sqlite3_native__on_after_load_extension(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_load_extension_t *req = (sqlite3_native_load_extension_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;

  if (req->error) {
    js_value_t *message;
    err = js_create_string_utf8(env, (utf8_t *) req->error, -1, &message);
    assert(err == 0);

    sqlite3_free(req->error);

    err = js_create_error(env, NULL, message, &result);
    assert(err == 0);

    err = js_reject_deferred(env, req->deferred, result);
    assert(err == 0);
  } else {
    err = js_get_undefined(env, &result);
    assert(err == 0);

    err = js_resolve_deferred(env, req->deferred, result);
    assert(err == 0);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_load_extension(uv_work_t *handle) {
  sqlite3_native_load_extension_t *req = (sqlite3_native_load_extension_t *) handle->data;

  sqlite3_load_extension(req->db->handle, (const char *) req->path, (const char *) req->entry, &req->error);

  free(req->path);
  free(req->entry);
}

static js_value_t *
sqlite3_native_load_extension(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 3);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  size_t path_len;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &path_len);
  assert(err == 0);

  path_len += 1 /* NULL */;

  utf8_t *path = (utf8_t *) malloc(path_len);

  err = js_get_value_string_utf8(env, argv[1], path, path_len, NULL);
  assert(err == 0);

  utf8_t *entry = NULL;

  js_value_type_t entry_type;
  err = js_typeof(env, argv[2], &entry_type);
  assert(err == 0);

  if (entry_type == js_string) {
    size_t entry_len;
    err = js_get_value_string_utf8(env, argv[2], NULL, 0, &entry_len);
    assert(err == 0);

    entry_len += 1 /* NULL */;

    entry = (utf8_t *) malloc(entry_len);

    err = js_get_value_string_utf8(env, argv[2], entry, entry_len, NULL);
    assert(err == 0);
  }

  sqlite3_native_load_extension_t *req = malloc(sizeof(sqlite3_native_load_extension_t));

  req->db = db;
  req->path = path;
  req->entry = entry;
  req->error = NULL;

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_load_extension, sqlite3_native__on_after_load_extension);
  assert(err == 0);

  return promise;
}

static js_value_t *
sqlite3_native_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("vfsInit", sqlite3_native_vfs_init)
  V("vfsDestroy", sqlite3_native_vfs_destroy)

  V("cacheVfsInit", sqlite3_native_cache_vfs_init)
  V("cacheVfsApply", sqlite3_native_cache_vfs_apply)
  V("cacheVfsDestroy", sqlite3_native_cache_vfs_destroy)

  V("init", sqlite3_native_init)
  V("open", sqlite3_native_open)
  V("close", sqlite3_native_close)
  V("exec", sqlite3_native_exec)
  V("loadExtension", sqlite3_native_load_extension)
#undef V

  return exports;
}

BARE_MODULE(sqlite3_native, sqlite3_native_exports)

#include <assert.h>
#include <bare.h>
#include <js.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>

typedef utf8_t sqlite3_native_path_t[4096];

typedef struct {
  sqlite3 *handle;

  js_env_t *env;
  js_ref_t *ctx;

  js_threadsafe_function_t *on_result;
} sqlite3_native_t;

typedef struct {
  sqlite3_vfs handle;

  char name[64];

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
  sqlite3_int64 offset;
} sqlite3_native_read_t;

typedef struct {
  sqlite3_native_file_t *file;

  const void *buf;
  int len;
  sqlite3_int64 offset;
} sqlite3_native_write_t;

typedef struct {
  sqlite3_native_file_t *file;

  sqlite_int64 size;
} sqlite3_native_size_t;

typedef struct {
  sqlite3_native_vfs_t *vfs;

  const char *name;
  int flags;
  bool exists;
} sqlite3_native_access_t;

typedef struct {
  sqlite3_native_vfs_t *vfs;

  const char *name;
  bool sync;
} sqlite3_native_delete_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  sqlite3_native_path_t name;
  sqlite3_native_vfs_t *vfs;
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

  int len;
  char **rows;
  char **cols;

  char *error;

  uv_sem_t done;
} sqlite3_native_exec_t;

static const size_t sqlite3_native__queue_limit = 64;

static bool
sqlite3_native__ends_with (const char *string, const char *suffix) {
  size_t string_len = strlen(string);
  size_t suffix_len = strlen(suffix);

  if (suffix_len > string_len) return false;

  return strncmp(string + string_len - suffix_len, suffix, suffix_len) == 0;
}

static int
sqlite3_native__get_file_type (int flags) {
  if (flags & SQLITE_OPEN_MAIN_DB) return 0;
  if (flags & SQLITE_OPEN_MAIN_JOURNAL) return 1;
  if (flags & SQLITE_OPEN_WAL) return 2;

  return -1;
}

static int
sqlite3_native__get_file_type_from_name (const char *name) {
  if (sqlite3_native__ends_with(name, "-journal")) return 1;
  if (sqlite3_native__ends_with(name, "-wal")) return 2;

  return 0;
}

static int
sqlite3_native__on_vfs_close (sqlite3_file *handle) {
  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_read_done (js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_read_t *data;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &data);
  assert(err == 0);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_read_call (js_env_t *env, js_value_t *on_read, void *context, void *arg) {
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
sqlite3_native__on_vfs_read (sqlite3_file *handle, void *buf, int len, sqlite3_int64 offset) {
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

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_write_done (js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_write_t *data;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &data);
  assert(err == 0);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_write_call (js_env_t *env, js_value_t *on_write, void *context, void *arg) {
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
sqlite3_native__on_vfs_write (sqlite3_file *handle, const void *buf, int len, sqlite_int64 offset) {
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

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_truncate (sqlite3_file *handle, sqlite_int64 size) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sync (sqlite3_file *handle, int flags) {
  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_size_done (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  sqlite3_native_size_t *data;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  err = js_get_value_int64(env, argv[0], &data->size);
  assert(err == 0);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_size_call (js_env_t *env, js_value_t *on_size, void *context, void *arg) {
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
sqlite3_native__on_vfs_size (sqlite3_file *handle, sqlite_int64 *size) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_size_t data = {
    file,
  };

  err = js_call_threadsafe_function(vfs->on_size, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  *size = data.size;

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_lock (sqlite3_file *handle, int eLock) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_unlock (sqlite3_file *sql_file, int eLock) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_check_reserved_lock (sqlite3_file *sql_file, int *pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_control (sqlite3_file *sql_file, int op, void *pArg) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sector_size (sqlite3_file *sql_file) {
  return 0;
}

static int
sqlite3_native__on_vfs_device_characteristics (sqlite3_file *sql_file) {
  return 0;
}

static int
sqlite3_native__on_vfs_open (sqlite3_vfs *vfs, const char *name, sqlite3_file *handle, int flags, int *pflags) {
  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  file->type = sqlite3_native__get_file_type(flags);

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
sqlite3_native__on_vfs_delete_done (js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_delete_t *data;
  err = js_get_callback_info(env, info, NULL, NULL, NULL, (void **) &data);
  assert(err == 0);

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_delete_call (js_env_t *env, js_value_t *on_delete, void *context, void *arg) {
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
sqlite3_native__on_vfs_delete (sqlite3_vfs *handle, const char *name, int sync) {
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

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_access_done (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  sqlite3_native_access_t *data;
  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  err = js_get_value_bool(env, argv[0], &data->exists);
  assert(err == 0);

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_access_call (js_env_t *env, js_value_t *on_access, void *context, void *arg) {
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
sqlite3_native__on_vfs_access (sqlite3_vfs *handle, const char *name, int flags, int *exists) {
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

  *exists = data.exists;

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_fullpathname (sqlite3_vfs *vfs, const char *name, int len, char *out) {
  if (strlen(name) >= len) return SQLITE_ERROR;

  strcpy(out, name);

  return SQLITE_OK;
}

static void *
sqlite3_native__on_vfs_dlopen (sqlite3_vfs *vfs, const char *zPath) {
  return 0;
}

static void
sqlite3_native__on_vfs_dlerror (sqlite3_vfs *vfs, int nByte, char *zErrMsg) {
  zErrMsg[nByte - 1] = '\0';
}

static void (*(sqlite3_native__on_vfs_dlsym) (sqlite3_vfs *vfs, void *pH, const char *z))(void) {
  return 0;
}

static void
sqlite3_native__on_vfs_dlclose (sqlite3_vfs *vfs, void *pHandle) {
  return;
}

static int
sqlite3_native__on_vfs_randomness (sqlite3_vfs *vfs, int bytes, char *buf) {
  memset(buf, 0, bytes);

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sleep (sqlite3_vfs *vfs, int nMicro) {
  return 0;
}

static int
sqlite3_native__on_vfs_current_time (sqlite3_vfs *vfs, double *time) {
  int err;

  uv_timespec64_t ts;
  err = uv_clock_gettime(UV_CLOCK_REALTIME, &ts);
  assert(err == 0);

  *time = ts.tv_sec / 86400.0 + 2440587.5;

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_vfs_init (js_env_t *env, js_callback_info_t *info) {
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

  vfs->handle = (sqlite3_vfs){
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
sqlite3_native_vfs_destroy (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &vfs, NULL);
  assert(err == 0);

  uv_sem_destroy(&vfs->done);

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

  return NULL;
}

static void
sqlite3_native__on_result_call (js_env_t *env, js_value_t *on_result, void *context, void *arg) {
  int err;

  sqlite3_native_t *db = (sqlite3_native_t *) context;

  sqlite3_native_exec_t *data = (sqlite3_native_exec_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, db->ctx, &ctx);
  assert(err == 0);

  js_value_t *rows;
  err = js_create_array_with_length(env, data->len, &rows);
  assert(err == 0);

  js_value_t *cols;
  err = js_create_array_with_length(env, data->len, &cols);
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
    err = js_create_string_utf8(env, (const utf8_t *) data->cols[i], -1, &col);
    assert(err == 0);

    err = js_set_element(env, cols, i, col);
    assert(err == 0);
  }

  js_value_t *args[2] = {rows, cols};

  err = js_call_function(env, ctx, on_result, 2, args, NULL);
  assert(err == 0);

  uv_sem_post(&data->done);
}

static int
sqlite3_native__on_result (void *arg, int len, char **rows, char **cols) {
  int err;

  sqlite3_native_exec_t *data = (sqlite3_native_exec_t *) arg;

  data->len = len;
  data->rows = rows;
  data->cols = cols;

  err = js_call_threadsafe_function(data->db->on_result, (void *) data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&data->done);

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_init (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  js_value_t *handle;

  sqlite3_native_t *db;
  err = js_create_arraybuffer(env, sizeof(sqlite3_native_t), (void **) &db, &handle);
  assert(err == 0);

  db->env = env;

  err = js_create_reference(env, argv[0], 1, &db->ctx);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[1], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) db, sqlite3_native__on_result_call, &db->on_result);
  assert(err == 0);

  return handle;
}

static void
sqlite3_native__on_after_open (uv_work_t *handle, int status) {
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
sqlite3_native__on_before_open (uv_work_t *handle) {
  int err;

  sqlite3_native_open_t *req = (sqlite3_native_open_t *) handle->data;

  err = sqlite3_open_v2((char *) req->name, &req->db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, req->vfs->name);
  assert(err == 0);
}

static js_value_t *
sqlite3_native_open (js_env_t *env, js_callback_info_t *info) {
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

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &vfs, NULL);
  assert(err == 0);

  sqlite3_native_path_t name;
  err = js_get_value_string_utf8(env, argv[2], name, sizeof(name), NULL);
  assert(err == 0);

  sqlite3_native_open_t *req = malloc(sizeof(sqlite3_native_open_t));

  req->db = db;
  req->vfs = vfs;

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
sqlite3_native__on_after_close (uv_work_t *handle, int status) {
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

  err = js_delete_reference(env, db->ctx);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_close (uv_work_t *handle) {
  int err;

  sqlite3_native_close_t *req = (sqlite3_native_close_t *) handle->data;

  err = sqlite3_close_v2(req->db->handle);
  assert(err == 0);
}

static js_value_t *
sqlite3_native_close (js_env_t *env, js_callback_info_t *info) {
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
sqlite3_native__on_after_exec (uv_work_t *handle, int status) {
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
sqlite3_native__on_before_exec (uv_work_t *handle) {
  int err;

  sqlite3_native_exec_t *req = (sqlite3_native_exec_t *) handle->data;

  err = uv_sem_init(&req->done, 0);
  assert(err == 0);

  sqlite3_exec(req->db->handle, (const char *) req->query, sqlite3_native__on_result, (void *) req, &req->error);

  free(req->query);

  uv_sem_destroy(&req->done);
}

static js_value_t *
sqlite3_native_exec (js_env_t *env, js_callback_info_t *info) {
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

  size_t query_length;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &query_length);
  assert(err == 0);

  query_length += 1 /* NULL */;

  utf8_t *query = (utf8_t *) malloc(query_length);

  err = js_get_value_string_utf8(env, argv[1], query, query_length, NULL);
  assert(err == 0);

  sqlite3_native_exec_t *req = malloc(sizeof(sqlite3_native_exec_t));

  req->db = db;
  req->query = query;

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_exec, sqlite3_native__on_after_exec);
  assert(err == 0);

  return promise;
}

static js_value_t *
sqlite3_native_exports (js_env_t *env, js_value_t *exports) {
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

  V("init", sqlite3_native_init)
  V("open", sqlite3_native_open)
  V("close", sqlite3_native_close)
  V("exec", sqlite3_native_exec)
#undef V

  return exports;
}

BARE_MODULE(sqlite3_native, sqlite3_native_exports)

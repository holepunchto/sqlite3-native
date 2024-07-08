#include <assert.h>
#include <bare.h>
#include <js.h>
#include <sqlite3.h>
#include <string.h>
#include <utf.h>

typedef struct {
  sqlite3 *handle;

  js_env_t *env;
  js_ref_t *ctx;

  js_ref_t *on_exec;
} sqlite3_native_t;

typedef struct {
  sqlite3_vfs handle;

  char name[64];

  js_env_t *env;
  js_ref_t *ctx;

  js_ref_t *on_access;
  js_ref_t *on_size;
  js_ref_t *on_read;
  js_ref_t *on_write;
  js_ref_t *on_delete;
} sqlite3_native_vfs_t;

typedef struct {
  sqlite3_file handle;

  int type;

  sqlite3_native_vfs_t *vfs;
} sqlite3_native_file_t;

typedef utf8_t sqlite3_native_path_t[4096];

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
sqlite3_native__on_vfs_read (sqlite3_file *handle, void *buf, int len, sqlite3_int64 offset) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  js_env_t *env = vfs->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  js_get_reference_value(env, vfs->ctx, &ctx);

  js_value_t *on_read;
  err = js_get_reference_value(env, vfs->on_read, &on_read);
  assert(err == 0);

  js_value_t *args[3];

  err = js_create_uint32(env, file->type, &args[0]);
  assert(err == 0);

  err = js_create_external_arraybuffer(env, buf, len, NULL, NULL, &args[1]);
  assert(err == 0);

  err = js_create_int64(env, offset, &args[2]);
  assert(err == 0);

  err = js_call_function(vfs->env, ctx, on_read, 3, args, NULL);
  assert(err == 0);

  err = js_detach_arraybuffer(env, args[1]);
  assert(err == 0);

  err = js_close_handle_scope(vfs->env, scope);
  assert(err == 0);

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_write (sqlite3_file *handle, const void *buf, int len, sqlite_int64 offset) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  js_env_t *env = vfs->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_write;
  err = js_get_reference_value(env, vfs->on_write, &on_write);
  assert(err == 0);

  js_value_t *args[3];

  err = js_create_uint32(env, file->type, &args[0]);
  assert(err == 0);

  err = js_create_external_arraybuffer(env, (void *) buf, len, NULL, NULL, &args[1]);
  assert(err == 0);

  err = js_create_int64(env, offset, &args[2]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_write, 3, args, NULL);
  assert(err == 0);

  err = js_detach_arraybuffer(env, args[1]);
  assert(err == 0);

  err = js_close_handle_scope(vfs->env, scope);
  assert(err == 0);

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_close (sqlite3_file *handle) {
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

static int
sqlite3_native__on_vfs_size (sqlite3_file *handle, sqlite_int64 *psize) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  js_env_t *env = vfs->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(vfs->env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(vfs->env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_size;
  err = js_get_reference_value(vfs->env, vfs->on_size, &on_size);
  assert(err == 0);

  js_value_t *args[1];

  err = js_create_uint32(vfs->env, file->type, &args[0]);
  assert(err == 0);

  js_value_t *result;
  err = js_call_function(vfs->env, ctx, on_size, 1, args, &result);
  assert(err == 0);

  int64_t size;
  err = js_get_value_int64(env, result, &size);
  assert(err == 0);

  *psize = size;

  err = js_close_handle_scope(vfs->env, scope);
  assert(err == 0);

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

static int
sqlite3_native__on_vfs_access (sqlite3_vfs *handle, const char *name, int flags, int *pexists) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  js_env_t *env = vfs->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  js_get_reference_value(vfs->env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_access;
  js_get_reference_value(vfs->env, vfs->on_access, &on_access);
  assert(err == 0);

  int type = sqlite3_native__get_file_type_from_name(name);

  js_value_t *args[1];

  err = js_create_uint32(vfs->env, type, &args[0]);
  assert(err == 0);

  js_value_t *result;
  err = js_call_function(vfs->env, ctx, on_access, 1, args, &result);
  assert(err == 0);

  bool exists;
  err = js_get_value_bool(env, result, &exists);
  assert(err == 0);

  *pexists = exists;

  err = js_close_handle_scope(vfs->env, scope);
  assert(err == 0);

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_delete (sqlite3_vfs *handle, const char *name, int syncDir) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(vfs->env, &scope);
  assert(err == 0);

  js_env_t *env = vfs->env;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_delete;
  err = js_get_reference_value(env, vfs->on_delete, &on_delete);
  assert(err == 0);

  int type = sqlite3_native__get_file_type_from_name(name);

  js_value_t *args[1];

  err = js_create_uint32(env, type, &args[0]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_delete, 1, args, NULL);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

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

  uv_random_t req;
  err = uv_random(loop, &req, vfs->name, sizeof(vfs->name), 0, NULL);
  assert(err == 0);

  vfs->name[sizeof(vfs->name) - 1] = '\0';

  vfs->env = env;

  err = js_create_reference(env, argv[0], 1, &vfs->ctx);
  assert(err == 0);

  err = js_create_reference(env, argv[1], 1, &vfs->on_access);
  assert(err == 0);

  err = js_create_reference(env, argv[2], 1, &vfs->on_size);
  assert(err == 0);

  err = js_create_reference(env, argv[3], 1, &vfs->on_read);
  assert(err == 0);

  err = js_create_reference(env, argv[4], 1, &vfs->on_write);
  assert(err == 0);

  err = js_create_reference(env, argv[5], 1, &vfs->on_delete);
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

  err = sqlite3_vfs_unregister(&vfs->handle);
  assert(err == 0);

  err = js_delete_reference(env, vfs->on_access);
  assert(err == 0);

  err = js_delete_reference(env, vfs->on_size);
  assert(err == 0);

  err = js_delete_reference(env, vfs->on_read);
  assert(err == 0);

  err = js_delete_reference(env, vfs->on_write);
  assert(err == 0);

  err = js_delete_reference(env, vfs->on_delete);
  assert(err == 0);

  err = js_delete_reference(env, vfs->ctx);
  assert(err == 0);

  return NULL;
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

  err = js_create_reference(env, argv[1], 1, &db->on_exec);
  assert(err == 0);

  return handle;
}

static js_value_t *
sqlite3_native_open (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 3);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  utf8_t name[1024];
  err = js_get_value_string_utf8(env, argv[1], name, 1024, NULL);
  assert(err == 0);

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[2], (void **) &vfs, NULL);
  assert(err == 0);

  err = sqlite3_open_v2((char *) name, &db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs->name);
  assert(err == 0);

  return NULL;
}

static js_value_t *
sqlite3_native_close (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  err = sqlite3_close_v2(db->handle);
  assert(err == 0);

  err = js_delete_reference(env, db->on_exec);
  assert(err == 0);

  err = js_delete_reference(env, db->ctx);
  assert(err == 0);

  return NULL;
}

static int
sqlite3_native__on_exec (void *data, int len, char **rows, char **cols) {
  int err;

  sqlite3_native_t *db = (sqlite3_native_t *) data;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *ctx;
  err = js_get_reference_value(env, db->ctx, &ctx);
  assert(err == 0);

  js_value_t *on_exec;
  err = js_get_reference_value(env, db->on_exec, &on_exec);
  assert(err == 0);

  js_value_t *args[2];

  err = js_create_array_with_length(env, len, &args[0]);
  assert(err == 0);

  err = js_create_array_with_length(env, len, &args[1]);
  assert(err == 0);

  for (int i = 0; i < len; i++) {
    js_value_t *row;

    if (rows[i] == NULL) {
      err = js_get_null(env, &row);
      assert(err == 0);
    } else {
      err = js_create_string_utf8(env, (const utf8_t *) rows[i], -1, &row);
      assert(err == 0);
    }

    err = js_set_element(env, args[0], i, row);
    assert(err == 0);

    js_value_t *col;
    err = js_create_string_utf8(env, (const utf8_t *) cols[i], -1, &col);
    assert(err == 0);

    err = js_set_element(env, args[1], i, col);
    assert(err == 0);
  }

  js_call_function(env, ctx, on_exec, 2, args, NULL);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return 0;
}

static js_value_t *
sqlite3_native_exec (js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  size_t query_length;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &query_length);
  assert(err == 0);

  query_length += 1 /* NULL */;

  utf8_t *query = (utf8_t *) sqlite3_malloc(query_length);

  err = js_get_value_string_utf8(env, argv[1], query, query_length, NULL);
  assert(err == 0);

  char *error;
  err = sqlite3_exec(db->handle, (const char *) query, sqlite3_native__on_exec, db, &error);

  sqlite3_free(query);

  if (err < 0) {
    err = js_throw_error(env, NULL, error);
    assert(err == 0);

    sqlite3_free(error);
  }

  return NULL;
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

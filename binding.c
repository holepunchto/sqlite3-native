#include <assert.h>
#include <bare.h>
#include <js.h>
#include <utf.h>
#include <string.h>
#include <sqlite3.h>

typedef struct {
  sqlite3 *db;  
  sqlite3_vfs *vfs;
  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_vfs_size;
  js_ref_t *on_vfs_read;
  js_ref_t *on_vfs_write;
  js_ref_t *on_vfs_delete;
} bare_sqlite3_t;

typedef struct {
  sqlite3_file base;
  sqlite3_vfs *vfs;
  int type;
} bare_sqlite3_vfs_file_t;

static void
noop_finalizer (js_env_t *env, void *data, void *hint) {}

#define bare_sqlite3_warning(...) fprintf(stderr, "warning: [bare_sqlite3] " __VA_ARGS__)

static int
get_file_type (int flags) {
  if (flags & SQLITE_OPEN_MAIN_DB) return 0;
  if (flags & SQLITE_OPEN_MAIN_JOURNAL) return 1;
  if (flags & SQLITE_OPEN_WAL) return 2;

  // TODO: if we wanna support these, we need to map them in get_file_type_from_name also
  // if (flags & SQLITE_OPEN_TEMP_DB) return 3;
  // if (flags & SQLITE_OPEN_TEMP_JOURNAL) return 4;
  // if (flags & SQLITE_OPEN_TRANSIENT_DB) return 5;
  // if (flags & SQLITE_OPEN_SUBJOURNAL) return 6;
  // if (flags & SQLITE_OPEN_SUPER_JOURNAL) return 7;

  bare_sqlite3_warning("get_file_type, unknown flag: %i\n", flags);

  return -1;
}

static bool
filename_ends_with (const char *filename, const char *suffix) {
  size_t n_filename = strlen(filename);
  size_t n_suffix = strlen(suffix);
  if (n_suffix > n_filename) return false;
  return strncmp(filename + n_filename - n_suffix, suffix, n_suffix) == 0;
}

static int
get_file_type_from_name (const char *name) {
  if (filename_ends_with(name, "-journal")) return 1;
  if (filename_ends_with(name, "-wal")) return 2;
  return 0;
}

static int
vfs_file_read (sqlite3_file *sql_file, void *buf, int len, sqlite3_int64 offset) {
  bare_sqlite3_vfs_file_t *file = (bare_sqlite3_vfs_file_t*) sql_file;
  bare_sqlite3_t *self = (bare_sqlite3_t*) file->vfs->pAppData;

  js_handle_scope_t *scope;
  js_open_handle_scope(self->env, &scope);

  js_value_t *ctx;
  js_value_t *callback;
  js_get_reference_value(self->env, self->ctx, &ctx);
  js_get_reference_value(self->env, self->on_vfs_read, &callback);

  js_value_t *args[3];
  js_create_uint32(self->env, file->type, &args[0]);
  js_create_external_arraybuffer(self->env, buf, len, noop_finalizer, NULL, &args[1]);
  // Need something larger
  js_create_uint32(self->env, offset, &args[2]);

  js_call_function(self->env, ctx, callback, 3, args, NULL);

  js_close_handle_scope(self->env, scope);

  return SQLITE_OK;  
}

static int
vfs_file_write (sqlite3_file *sql_file, const void *buf, int len, sqlite_int64 offset) {
  bare_sqlite3_vfs_file_t *file = (bare_sqlite3_vfs_file_t*) sql_file;
  bare_sqlite3_t *self = (bare_sqlite3_t*) file->vfs->pAppData;

  js_handle_scope_t *scope;
  js_open_handle_scope(self->env, &scope);

  js_value_t *ctx;
  js_value_t *callback;
  js_get_reference_value(self->env, self->ctx, &ctx);
  js_get_reference_value(self->env, self->on_vfs_write, &callback);

  js_value_t *args[3];
  js_create_uint32(self->env, file->type, &args[0]);
  js_create_external_arraybuffer(self->env, (void*) buf, len, noop_finalizer, NULL, &args[1]);
  // Need something larger
  js_create_uint32(self->env, offset, &args[2]);

  js_call_function(self->env, ctx, callback, 3, args, NULL);

  js_close_handle_scope(self->env, scope);

  return SQLITE_OK;  
} 

static int
vfs_file_close (sqlite3_file *sql_file) {
  return SQLITE_OK;
}

static int
vfs_file_truncate (sqlite3_file *sql_file, sqlite_int64 size) {
  printf("TODO binding.c: in vfs_file_truncate\n");
  return SQLITE_OK;
}

static int
vfs_file_sync (sqlite3_file *sql_file, int flags) {
  return SQLITE_OK;
}

static int
vfs_file_size (sqlite3_file *sql_file, sqlite_int64 *pSize) {
  bare_sqlite3_vfs_file_t *file = (bare_sqlite3_vfs_file_t*) sql_file;
  bare_sqlite3_t *self = (bare_sqlite3_t*) file->vfs->pAppData;

  js_handle_scope_t *scope;
  js_open_handle_scope(self->env, &scope);

  js_value_t *ctx;
  js_value_t *callback;
  js_get_reference_value(self->env, self->ctx, &ctx);
  js_get_reference_value(self->env, self->on_vfs_size, &callback);

  js_value_t *args[2];
  js_create_uint32(self->env, file->type, &args[0]);
  js_create_external_arraybuffer(self->env, (void *) pSize, 8, noop_finalizer, NULL, &args[1]);

  js_call_function(self->env, ctx, callback, 2, args, NULL);

  js_close_handle_scope(self->env, scope);

  return SQLITE_OK;  
}

static int
vfs_file_lock (sqlite3_file *sql_file, int eLock) {
  return SQLITE_OK;
}

static int
vfs_file_unlock (sqlite3_file *sql_file, int eLock) {
  return SQLITE_OK;
}

static int
vfs_file_check_reserved_lock (sqlite3_file *sql_file, int *pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}

static int
vfs_file_control (sqlite3_file *sql_file, int op, void *pArg) {
  return SQLITE_OK;
}

static int
vfs_file_sector_size (sqlite3_file *sql_file) {
  return 0;
}

static int
vfs_file_device_characteristics (sqlite3_file *sql_file) {
  return 0;
}

static int
vfs_open (sqlite3_vfs *vfs, const char *name, sqlite3_file *sql_file, int flags, int *pOutFlags) {
  static const sqlite3_io_methods io = {
    1,                               /* iVersion */
    vfs_file_close,                  /* xClose */
    vfs_file_read,                   /* xRead */
    vfs_file_write,                  /* xWrite */
    vfs_file_truncate,               /* xTruncate */
    vfs_file_sync,                   /* xSync */
    vfs_file_size,                   /* xFileSize */
    vfs_file_lock,                   /* xLock */
    vfs_file_unlock,                 /* xUnlock */
    vfs_file_check_reserved_lock,    /* xCheckReservedLock */
    vfs_file_control,                /* xFileControl */
    vfs_file_sector_size,            /* xSectorSize */
    vfs_file_device_characteristics  /* xDeviceCharacteristics */
  };

  bare_sqlite3_vfs_file_t *file = (bare_sqlite3_vfs_file_t*) sql_file;

  memset(file, 0, sizeof(bare_sqlite3_vfs_file_t));
  file->base.pMethods = &io;
  file->vfs = vfs;
  file->type = get_file_type(flags);

  if (name == NULL) return SQLITE_IOERR;
  return SQLITE_OK;
}

static int
vfs_access (sqlite3_vfs *vfs, const char *name, int flags, int *pResOut) {
  *pResOut = 1; // all files are already exists for now at least...
  return SQLITE_OK;
}

static int
vfs_delete (sqlite3_vfs *vfs, const char *name, int syncDir) {
  bare_sqlite3_t *self = (bare_sqlite3_t*) vfs->pAppData;

  js_handle_scope_t *scope;
  js_open_handle_scope(self->env, &scope);

  js_value_t *ctx;
  js_value_t *callback;
  js_get_reference_value(self->env, self->ctx, &ctx);
  js_get_reference_value(self->env, self->on_vfs_delete, &callback);

  js_value_t *args[1];
  js_create_uint32(self->env, get_file_type_from_name(name), &args[0]);

  js_call_function(self->env, ctx, callback, 1, args, NULL);

  js_close_handle_scope(self->env, scope);

  return SQLITE_OK;
}

static int
vfs_fullpathname (sqlite3_vfs *vfs, const char *name, int nOut, char *zOut) {
  if (strlen(name) >= nOut) {
    return SQLITE_ERROR;
  }
  strcpy(zOut, name);
  return SQLITE_OK;
}

static void*
vfs_dlopen (sqlite3_vfs *vfs, const char *zPath) {
  return 0;
}

static void
vfs_dlerror (sqlite3_vfs *vfs, int nByte, char *zErrMsg) {
  sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
  zErrMsg[nByte-1] = '\0';
}

static void 
(*vfs_dlsym(sqlite3_vfs *vfs, void *pH, const char *z))(void) {
  return 0;
}

static void
vfs_dlclose (sqlite3_vfs *vfs, void *pHandle) {
  return;
}

/*
** Parameter zByte points to a buffer nByte bytes in size. Populate this
** buffer with pseudo-random data.
*/
static int
vfs_randomness (sqlite3_vfs *vfs, int nByte, char *zByte) {
  bare_sqlite3_warning("vfs_randomness\n");
  return SQLITE_OK;
}

/*
** Sleep for at least nMicro microseconds. Return the (approximate) number 
** of microseconds slept for.
*/
static int
vfs_sleep (sqlite3_vfs *vfs, int nMicro) {
  bare_sqlite3_warning("vfs_sleep\n");
  uv_sleep(nMicro / 1000);
  if (nMicro % 1000 > 500) {
    uv_sleep(1);
  }
  return nMicro;
}

/*
** Set *pTime to the current UTC time expressed as a Julian day. Return
** SQLITE_OK if successful, or an error code otherwise.
**
**   http://en.wikipedia.org/wiki/Julian_day
**
** This implementation is not very good. The current time is rounded to
** an integer number of seconds. Also, assuming time_t is a signed 32-bit 
** value, it will stop working some time in the year 2038 AD (the so-called
** "year 2038" problem that afflicts systems that store time this way). 
*/
static int
vfs_current_time (sqlite3_vfs *vfs, double *pTime) {
  bare_sqlite3_warning("vfs_current_time\n");
  time_t t = time(0);
  *pTime = t/86400.0 + 2440587.5; 
  return SQLITE_OK;
}

static int
vfs_get_last_error (sqlite3_vfs *vfs, int i, char *c) {
  printf("TODO binding.c: get last error\n");
  return SQLITE_OK;
}

static js_value_t *
bare_sqlite3_open (js_env_t *env, js_callback_info_t *info) {
  size_t argc = 7;
  js_value_t *argv[7];

  js_get_callback_info(env, info, &argc, argv, NULL, NULL);

  bare_sqlite3_t *self;
  size_t self_len;

  js_get_typedarray_info(env, argv[0], NULL, (void **) &self, &self_len, NULL, NULL);
  js_create_reference(env, argv[1], 1, &(self->ctx));

  utf8_t name[1024];
  js_get_value_string_utf8(env, argv[2], name, 1024, NULL);

  js_create_reference(env, argv[3], 1, &(self->on_vfs_size));
  js_create_reference(env, argv[4], 1, &(self->on_vfs_read));
  js_create_reference(env, argv[5], 1, &(self->on_vfs_write));
  js_create_reference(env, argv[6], 1, &(self->on_vfs_delete));

  self->env = env;
  self->vfs = sqlite3_malloc(sizeof(*self->vfs));
  memset(self->vfs, 0, sizeof(*self->vfs));

  self->vfs->pAppData = self;
  self->vfs->iVersion = 1;
  self->vfs->szOsFile = sizeof(bare_sqlite3_vfs_file_t);
  self->vfs->mxPathname = 1024;
  self->vfs->zName = "bare_sqlite3_vfs";
  self->vfs->xOpen = vfs_open;
  self->vfs->xDelete = vfs_delete;
  self->vfs->xAccess = vfs_access;
  self->vfs->xFullPathname = vfs_fullpathname;
  self->vfs->xDlOpen = vfs_dlopen;
  self->vfs->xDlSym = vfs_dlsym;
  self->vfs->xDlError = vfs_dlerror;
  self->vfs->xDlClose = vfs_dlclose;
  self->vfs->xRandomness = vfs_randomness;
  self->vfs->xSleep = vfs_sleep;
  self->vfs->xCurrentTime = vfs_current_time;
  self->vfs->xGetLastError = vfs_get_last_error;

  sqlite3_vfs_register(self->vfs, 1);
  sqlite3_open((char*) name, &self->db);

  return NULL;
}

static js_value_t *
bare_sqlite3_close (js_env_t *env, js_callback_info_t *info) {
  return NULL;
}

static int
on_sql_exec_callback (void *data, int cols_len, char **cols, char **rows) {
  printf("oncb\n");
  return 0;
}

static js_value_t *
bare_sqlite3_exec (js_env_t *env, js_callback_info_t *info) {
  size_t argc = 2;
  js_value_t *argv[2];  

  js_get_callback_info(env, info, &argc, argv, NULL, NULL);

  bare_sqlite3_t *self;
  size_t self_len;

  js_get_typedarray_info(env, argv[0], NULL, (void **) &self, &self_len, NULL, NULL);

  size_t query_length;
  js_get_value_string_utf8(env, argv[1], NULL, 0, &query_length);

  utf8_t *query = (utf8_t *) sqlite3_malloc(sizeof(utf8_t) * query_length + 1);

  js_get_value_string_utf8(env, argv[1], query, query_length, NULL);

  char *err = NULL;
  sqlite3_exec(self->db, (const char *) query, NULL, NULL, &err);
  sqlite3_free(query);

  if (err != NULL) {
    js_throw_error(env, "SQLITE_EXEC_ERROR", err);
    sqlite3_free(err);
  }

  return NULL;  
}

static js_value_t *
init (js_env_t *env, js_value_t *exports) {
  int err;

  {
    js_value_t *val;
    js_create_int32(env, sizeof(bare_sqlite3_t), &val);
    js_set_named_property(env, exports, "sizeof_bare_sqlite3_t", val);
  }

#define V(fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, #fn, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, #fn, val); \
    assert(err == 0); \
  }

  V(bare_sqlite3_open)
  V(bare_sqlite3_close)
  V(bare_sqlite3_exec)
#undef V

  return exports;
}

BARE_MODULE(bare_sqlite3, init)

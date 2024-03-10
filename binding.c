#include <assert.h>
#include <bare.h>
#include <js.h>
#include <utf.h>
#include <pthread.h>

#include <sqlite3.h>

typedef struct {
  sqlite3 *db;  
  sqlite3_vfs *vfs;
  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *on_vfs_read;
  js_ref_t *on_vfs_write;
} bare_sqlite3_t;

typedef struct {
  sqlite3_file base;
  sqlite3_vfs *vfs;
  int filetype;
} vfs_file_t;

static void
noop_finalizer (js_env_t *env, void *data, void *hint) {
}

static int
vfs_file_read (sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst) {
  vfs_file_t *file = (vfs_file_t*) pFile;
  bare_sqlite3_t *self = (bare_sqlite3_t*) file->vfs->pAppData;

  js_value_t *ctx;
  js_value_t *callback;
  js_get_reference_value(self->env, self->ctx, &ctx);
  js_get_reference_value(self->env, self->on_vfs_read, &callback);

  js_handle_scope_t *scope;
  js_open_handle_scope(self->env, &scope);

  js_value_t *args[3];
  js_create_uint32(self->env, file->filetype, &args[0]);
  js_create_external_arraybuffer(self->env, zBuf, iAmt, noop_finalizer, NULL, &args[1]);
  // Need something larger
  js_create_uint32(self->env, iOfst, &args[2]);

  js_call_function(self->env, ctx, callback, 3, args, NULL);

  js_close_handle_scope(self->env, scope);

  return SQLITE_OK;  
}

static int
vfs_file_write (sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite_int64 iOfst) {
  printf("in vfs_file_write here\n");
  vfs_file_t *file = (vfs_file_t*) pFile;
  bare_sqlite3_t *self = (bare_sqlite3_t*) file->vfs->pAppData;

  js_value_t *ctx;
  js_value_t *callback;
  js_get_reference_value(self->env, self->ctx, &ctx);
  js_get_reference_value(self->env, self->on_vfs_write, &callback);

  js_handle_scope_t *scope;
  js_open_handle_scope(self->env, &scope);

  js_value_t *args[3];
  js_create_uint32(self->env, file->filetype, &args[0]);
  js_create_external_arraybuffer(self->env, (void*) zBuf, iAmt, noop_finalizer, NULL, &args[1]);
  // Need something larger
  js_create_uint32(self->env, iOfst, &args[2]);

  js_call_function(self->env, ctx, callback, 3, args, NULL);

  js_close_handle_scope(self->env, scope);

  return SQLITE_OK;  
} 

static int
vfs_file_close (sqlite3_file *pFile) {
  printf("in vfs_file_close\n");
  return SQLITE_OK;
}

static int
vfs_file_truncate (sqlite3_file *pFile, sqlite_int64 size) {
  printf("in vfs_file_truncate\n");  
  return SQLITE_OK;
}

static int
vfs_file_sync (sqlite3_file *pFile, int flags) {
  printf("in vfs_file_sync\n");  
  return SQLITE_OK;
}

static int
vfs_file_size (sqlite3_file *pFile, sqlite_int64 *pSize) {
  printf("in vfs_file_size\n");
  *pSize = 0;
  return SQLITE_OK;  
}

static int
vfs_file_lock (sqlite3_file *pFile, int eLock) {
  return SQLITE_OK;
}

static int
vfs_file_unlock (sqlite3_file *pFile, int eLock) {
  return SQLITE_OK;
}

static int
vfs_file_check_reserved_lock (sqlite3_file *pFile, int *pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}

static int
vfs_file_control (sqlite3_file *pFile, int op, void *pArg) {
  return SQLITE_NOTFOUND;
}

static int
vfs_file_sector_size (sqlite3_file *pFile) {
  printf("in vfs_file_sector_size\n");
  return 0;
}

static int
vfs_file_device_characteristics (sqlite3_file *pFile) {
  printf("in vfs_file_device_characteristics\n");
  return 0;
}

static int
vfs_open (sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile, int flags, int *pOutFlags) {
  printf("in vfs_open with name: %s\n", zName);
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

  vfs_file_t *file = (vfs_file_t*) pFile; 

  memset(file, 0, sizeof(vfs_file_t));
  file->base.pMethods = &io;
  file->vfs = pVfs;

  int filetype = 0;
  if (flags & SQLITE_OPEN_MAIN_DB)       filetype = 1;
  if (flags & SQLITE_OPEN_MAIN_JOURNAL)  filetype = 2;
  if (flags & SQLITE_OPEN_TEMP_DB)       filetype = 3;
  if (flags & SQLITE_OPEN_TEMP_JOURNAL)  filetype = 4;
  if (flags & SQLITE_OPEN_TRANSIENT_DB)  filetype = 5;
  if (flags & SQLITE_OPEN_SUBJOURNAL)    filetype = 6;
  if (flags & SQLITE_OPEN_SUPER_JOURNAL) filetype = 7;
  if (flags & SQLITE_OPEN_WAL)           filetype = 8;
  file->filetype = filetype;

  if (zName == 0) {
    return SQLITE_IOERR;
  }
  return SQLITE_OK;
}

static int
vfs_access (sqlite3_vfs *pVfs, const char *zName, int flags, int *pResOut) {
  printf("in vfs_access here\n");  
  return SQLITE_OK;
}

static int
vfs_delete (sqlite3_vfs *pVfs, const char *zName, int syncDir) {
  printf("in vfs_delete here\n");
  return SQLITE_OK;
}

static int
vfs_fullpathname (sqlite3_vfs *pVfs, const char *zName, int nOut, char *zOut) {
  if (strlen(zName) >= nOut) {
    return SQLITE_ERROR;
  }
  strcpy(zOut, zName);  
  return SQLITE_OK;
}

static void*
vfs_dlopen (sqlite3_vfs *pVfs, const char *zPath) {
  return 0;
}

static void
vfs_dlerror (sqlite3_vfs *pVfs, int nByte, char *zErrMsg) {
  sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
  zErrMsg[nByte-1] = '\0';
}

static void 
(*vfs_dlsym(sqlite3_vfs *pVfs, void *pH, const char *z))(void) {
  return 0;
}

static void
vfs_dlclose (sqlite3_vfs *pVfs, void *pHandle) {
  return;
}

/*
** Parameter zByte points to a buffer nByte bytes in size. Populate this
** buffer with pseudo-random data.
*/
static int
vfs_randomness (sqlite3_vfs *pVfs, int nByte, char *zByte) {
  return SQLITE_OK;
}

/*
** Sleep for at least nMicro microseconds. Return the (approximate) number 
** of microseconds slept for.
*/
static int
vfs_sleep (sqlite3_vfs *pVfs, int nMicro) {
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
vfs_current_time (sqlite3_vfs *pVfs, double *pTime) {
  time_t t = time(0);
  *pTime = t/86400.0 + 2440587.5; 
  return SQLITE_OK;
}

static js_value_t *
bare_sqlite3_open (js_env_t *env, js_callback_info_t *info) {
  size_t argc = 5;
  js_value_t *argv[5];  

  js_get_callback_info(env, info, &argc, argv, NULL, NULL);

  bare_sqlite3_t *self;
  size_t self_len;

  js_get_typedarray_info(env, argv[0], NULL, (void **) &self, &self_len, NULL, NULL);
  js_create_reference(env, argv[1], 1, &(self->ctx));

  utf8_t name[1024];
  js_get_value_string_utf8(env, argv[2], name, 1024, NULL);

  js_create_reference(env, argv[3], 1, &(self->on_vfs_read));
  js_create_reference(env, argv[4], 1, &(self->on_vfs_write));

  self->env = env;
  self->vfs = sqlite3_malloc(sizeof(*self->vfs));
  memset(self->vfs, 0, sizeof(*self->vfs));

  self->vfs->pAppData = self;
  self->vfs->iVersion = 1;
  self->vfs->szOsFile = sizeof(sqlite3_file);
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

  int ret = 0;

  ret = sqlite3_vfs_register(self->vfs, 1);
  printf("registered vfs with code: %d\n", ret);

  printf("opening db with name %s\n", name);
  ret = sqlite3_open((char*) name, &self->db);
  printf("opened db with code: %d\n", ret);

  return NULL;
}

static js_value_t *
bare_sqlite3_close (js_env_t *env, js_callback_info_t *info) {
  return NULL;
}

static js_value_t *
bare_sqlite3_exec (js_env_t *env, js_callback_info_t *info) {
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

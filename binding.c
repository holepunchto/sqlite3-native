#include <assert.h>
#include <bare.h>
#include <js.h>
#include <utf.h>
#include <pthread.h>

#include <sqlite3.h>

#define DB_FILE "example.db"
#define TABLE_NAME "records"

static js_value_t *global_cb = NULL;
static js_env_t *global_env = NULL;

int vfstrace_register(
   const char *zTraceName,           /* Name of the newly constructed VFS */
   const char *zOldVfsName,          /* Name of the underlying VFS */
   int (*xOut)(const char*,void*),   /* Output routine.  ex: fputs */
   void *pOutArg,                    /* 2nd argument to xOut.  ex: stderr */
   int makeDefault                   /* True to make the new VFS the default */
);

// Callback function for executing SQL commands
static int callback(void *NotUsed, int argc, char **argv, char **azColName){
    int i;
    for(i=0; i<argc; i++){
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0;
}

static void my_finalizer () {
  
}

static void tracing_write_callback (const char *filename, const void *buf, int length, int offset) {
  js_handle_scope_t *scope;
  int res;

  res = js_open_handle_scope(global_env, &scope);

  js_value_t *ctx;
  res = js_get_global(global_env, &ctx);

  js_value_t *args[3];
  res = js_create_string_utf8(global_env, (utf8_t *) filename, -1, &args[0]);
  res = js_create_external_arraybuffer(global_env, (void*) buf, length, my_finalizer, NULL, &args[1]);
  res = js_create_uint32(global_env, offset, &args[2]);

  res = js_call_function(global_env, ctx, global_cb, 3, args, NULL);

  js_close_handle_scope(global_env, scope);
}

static int tracing_callback (const char *zMessage, void *pAppData) {
  return 0;
}

static js_value_t *
generate_database (js_env_t *env, js_callback_info_t *info) {
  size_t argc = 1;
  js_value_t *argv[1];

  js_get_callback_info(env, info, &argc, argv, NULL, NULL);

  global_cb = argv[0];
  global_env = env;
  
  sqlite3 *db;
  char *zErrMsg = 0;
  int rc;

  vfstrace_register(
    "tracing-vfs",
    NULL,
    tracing_callback,
    tracing_write_callback,
    true
  );

  rc = sqlite3_open(DB_FILE, &db);
  if (rc) {
      fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
      return(0);
  } else {
      fprintf(stderr, "Opened database successfully\n");
  }

  char *sql = "CREATE TABLE " TABLE_NAME " (" \
             "ID INTEGER PRIMARY KEY AUTOINCREMENT," \
             "NAME           TEXT    NOT NULL," \
             "VALUE          INT     NOT NULL);";

  // Create the test table
  rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
  if (rc != SQLITE_OK) {
      fprintf(stderr, "SQL error: %s\n", zErrMsg);
      sqlite3_free(zErrMsg);
  } else {
      fprintf(stdout, "Table created successfully\n");
  }

  int inserts = 250;
  int updates = 0;

  // Insert N records into the table
  for (int i = inserts - 1; i >= 0; i--) {
      printf("writing record %d\n", i);
      char insert_sql[1024];
      sprintf(insert_sql, "INSERT INTO " TABLE_NAME " (NAME, VALUE) VALUES ('Record %d', %d);", i, i*10);
      rc = sqlite3_exec(db, insert_sql, callback, 0, &zErrMsg);
      if (rc != SQLITE_OK) {
          fprintf(stderr, "SQL error: %s\n", zErrMsg);
          sqlite3_free(zErrMsg);
      }
      printf("## wrote record %d\n", i);
  }

  // Update M records in the table
  for (int i = 1; i < updates + 1; i++) {
      printf("updating record %d\n", i);
      char update_sql[1024];
      sprintf(update_sql, "UPDATE " TABLE_NAME " SET NAME = '* Record %d', VALUE = %d WHERE ID = %d ;", i * 2, i * 2, i);
      rc = sqlite3_exec(db, update_sql, callback, 0, &zErrMsg);
      if (rc != SQLITE_OK) {
          fprintf(stderr, "SQL error: %s\n", zErrMsg);
          sqlite3_free(zErrMsg);
      }
      printf("## updated record %d\n", i);
  }
    
  int err;

  js_value_t *result;
  err = js_create_string_utf8(env, (utf8_t *) "Hello addon", -1, &result);
  if (err < 0) return NULL;

  return result;
}

static js_value_t *
init (js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("generate_database", generate_database)
#undef V

  return exports;
}

BARE_MODULE(sqlite_vfs_testing, init)

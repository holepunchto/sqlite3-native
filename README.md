# sqlite3-native

Asynchronous SQLite3 bindings for JavaScript with VFS support.

```
npm i sqlite3-native
```

## Usage

```js
const SQLite3 = require('sqlite3-native')

const sql = new SQLite3()

await sql.exec(`CREATE TABLE records (NAME TEXT NOT NULL);`)

await sql.exec(`INSERT INTO records (NAME) values ('Jane'), ('John');`)

await sql.exec(`SELECT NAME FROM records;`)

// [
//   { rows: [ 'Jane' ], columns: [ 'NAME' ] },
//   { rows: [ 'John' ], columns: [ 'NAME' ] }
// ]
```

## License

Apache-2.0

const test = require('brittle')

const MemoryVFS = require('../memory')
const BareSQLite3 = require('..')

test('can open a db', async t => {
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  t.pass('opened the db without throwing')
})

test('can open a db and create many tables', async t => {
  const sql = create(t)

  for (let i = 0; i < 10; i++) {
    await sql.exec(`CREATE TABLE records${i} (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);`)
  }

  t.pass('opened the db without throwing')
})

test('can open a db, insert and select', async t => {
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec(`INSERT INTO records (NAME) values ('mathias'), ('andrew');`)
  const result = await sql.exec(`SELECT ID, NAME FROM records;`)
  t.is(result.length, 2)
  t.alike(result[0].columns, ['ID', 'NAME'])
  t.alike(result[0].rows, ['1', 'mathias'])
  t.alike(result[1].rows, ['2', 'andrew'])
})

function create (t) {
  const db = new BareSQLite3(() => new MemoryVFS())
  t.teardown(() => db.close())
  return db
}

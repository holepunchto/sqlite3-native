const test = require('brittle')

const BareSQLite3 = require('..')

test('can open a db', async t => {
  const sql = new BareSQLite3('hello-world')
  await sql.ready()
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  t.pass('opened the db without throwing')
  await sql.close()
})

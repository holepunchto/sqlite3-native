const test = require('brittle')

const BareSQLite3 = require('..')

test('can open a db', async t => {
  const sql = new BareSQLite3('hello-world')
  await sql.ready()
  t.pass('opened the db without throwing')
})

const test = require('brittle')
const { create } = require('./helpers')

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
  await sql.exec("INSERT INTO records (NAME) values ('mathias'), ('andrew');")
  const result = await sql.exec('SELECT ID, NAME FROM records;')
  t.is(result.length, 2)
  t.alike(result[0].columns, ['ID', 'NAME'])
  t.alike(result[0].rows, ['1', 'mathias'])
  t.alike(result[1].rows, ['2', 'andrew'])
})

test('big values', async t => {
  const big = Buffer.alloc(4096).fill('big').toString()
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec("INSERT INTO records (NAME) values ('" + big + "'), ('short');")
  const result = await sql.exec('SELECT ID, NAME FROM records;')
  t.is(result.length, 2)
  t.alike(result[0].columns, ['ID', 'NAME'])
  t.alike(result[0].rows, ['1', big])
  t.alike(result[1].rows, ['2', 'short'])
})

test('basic index', async (t) => {
  const sql = create(t)

  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec('CREATE UNIQUE INDEX idx_name ON records (NAME);')
  await sql.exec("INSERT INTO records (NAME) values ('mathias'), ('andrew');")
  const result = await sql.exec("SELECT NAME FROM records WHERE NAME = 'mathias';")
  t.is(result.length, 1)
  t.alike(result[0].columns, ['NAME'])
  t.alike(result[0].rows, ['mathias'])
})

test('bigger index', async (t) => {
  const sql = create(t)

  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec('CREATE UNIQUE INDEX idx_name ON records (NAME);')

  for (let i = 0; i < 1000; i++) {
    await sql.exec(`INSERT INTO records (NAME) values ('mr-${i}');`)
  }

  const result = await sql.exec("SELECT NAME FROM records WHERE NAME = 'mr-10';")
  t.is(result.length, 1)
  t.alike(result[0].columns, ['NAME'])
  t.alike(result[0].rows, ['mr-10'])
})

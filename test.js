const test = require('brittle')
const { create } = require('./test/helpers')

test('can open a db', async (t) => {
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  t.pass('opened the db without throwing')
})

test('can open a db and create many tables', async (t) => {
  const sql = create(t)

  for (let i = 0; i < 10; i++) {
    await sql.exec(
      `CREATE TABLE records${i} (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);`
    )
  }

  t.pass('opened the db without throwing')
})

test('can open a db, insert and select', async (t) => {
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec("INSERT INTO records (NAME) values ('mathias'), ('andrew');")
  const result = await sql.exec('SELECT ID, NAME FROM records;')
  t.is(result.length, 2)
  t.alike(result[0].columns, ['ID', 'NAME'])
  t.alike(result[0].rows, ['1', 'mathias'])
  t.alike(result[1].rows, ['2', 'andrew'])
})

test('big values', async (t) => {
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

const SQLite3 = require('.')
const { CacheVFS } = SQLite3

const fs = typeof Bare !== 'undefined' ? require('bare-fs') : require('fs')

const PAGE_SIZE = 4096

function tmp(t) {
  const dir = `test/sandbox/${Date.now()}-${Math.random().toString(36).slice(2)}`
  fs.mkdirSync(dir, { recursive: true })
  t.teardown(() => fs.rmSync(dir, { recursive: true, force: true }))
  return dir
}

test('cache vfs: native file backing persists across sessions', async (t) => {
  const path = tmp(t) + '/db'

  const a = new SQLite3({ vfs: new CacheVFS(path) })
  await a.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await a.exec("INSERT INTO records (NAME) values ('mathias'), ('andrew');")
  await a.close()

  t.ok(fs.statSync(path).size > 0, 'cache file written')
  t.ok(fs.existsSync(path + '.map'), 'bitmap written')

  const b = new SQLite3({ vfs: new CacheVFS(path) })
  const result = await b.exec('SELECT NAME FROM records;')
  t.alike(result[0].rows, ['mathias'])
  t.alike(result[1].rows, ['andrew'])
  await b.close()
})

test('cache vfs: missing pages are fetched through the miss handler', async (t) => {
  const dir = tmp(t)

  // build a source database, its cache file is a plain SQLite file
  const source = new SQLite3({ vfs: new CacheVFS(dir + '/source') })
  await source.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  for (let i = 0; i < 500; i++) {
    await source.exec(`INSERT INTO records (NAME) values ('mr-${i}');`)
  }
  await source.close()

  const image = fs.readFileSync(dir + '/source')

  // sparse replica: right size, no pages present (no bitmap)
  fs.writeFileSync(dir + '/replica', Buffer.alloc(0))
  fs.truncateSync(dir + '/replica', image.byteLength)

  const misses = []

  const replica = new SQLite3({
    vfs: new CacheVFS(dir + '/replica', {
      miss(buffer, index) {
        misses.push(index)
        image.copy(buffer, 0, index * PAGE_SIZE, Math.min((index + 1) * PAGE_SIZE, image.byteLength))
      }
    })
  })

  const result = await replica.exec("SELECT NAME FROM records WHERE NAME = 'mr-499';")
  t.alike(result[0].rows, ['mr-499'])
  t.ok(misses.length > 0, `fetched ${misses.length} pages`)
  await replica.close()

  // fetched pages are now locally present: no handler needed for the same read
  const again = new SQLite3({ vfs: new CacheVFS(dir + '/replica') })
  const cached = await again.exec("SELECT NAME FROM records WHERE NAME = 'mr-499';")
  t.alike(cached[0].rows, ['mr-499'])
  await again.close()
})

test('cache vfs: applyPages bootstraps and updates a live connection', async (t) => {
  const dir = tmp(t)

  const source = new SQLite3({ vfs: new CacheVFS(dir + '/source') })
  await source.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await source.exec("INSERT INTO records (NAME) values ('mathias');")
  await source.close()

  const pages = (image) => {
    const updates = []
    for (let i = 0; i * PAGE_SIZE < image.byteLength; i++) {
      const page = Buffer.alloc(PAGE_SIZE)
      image.copy(page, 0, i * PAGE_SIZE, Math.min((i + 1) * PAGE_SIZE, image.byteLength))
      updates.push({ index: i, page })
    }
    return updates
  }

  // bootstrap an empty replica entirely through applyPages
  const vfs = new CacheVFS(dir + '/replica')
  let image = fs.readFileSync(dir + '/source')
  vfs.applyPages(pages(image), image.byteLength)

  const replica = new SQLite3({ vfs })
  const before = await replica.exec('SELECT NAME FROM records;')
  t.is(before.length, 1)
  t.alike(before[0].rows, ['mathias'])

  // a remote writer commits; checkpoint the replica while it stays open
  const writer = new SQLite3({ vfs: new CacheVFS(dir + '/source') })
  await writer.exec("INSERT INTO records (NAME) values ('andrew');")
  await writer.close()

  image = fs.readFileSync(dir + '/source')
  vfs.applyPages(pages(image), image.byteLength)

  const after = await replica.exec('SELECT NAME FROM records;')
  t.is(after.length, 2, 'live connection sees checkpointed rows')
  t.alike(after[1].rows, ['2', 'andrew'].slice(1))
  await replica.close()
})

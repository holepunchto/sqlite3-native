const test = require('brittle')

test('select 1', async (t) => {
  const ops = 1000

  await t.test('sqlite3-native', async (t) => {
    const SQLite = require('.')

    const db = new SQLite()

    await db.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')

    for (let i = 0; i < 1000; i++) {
      await db.exec(`INSERT INTO records (NAME) values ('${i}');`)
    }

    const elapsed = await t.execution(async () => {
      for (let i = 0; i < ops; i++) {
        await db.exec("SELECT NAME FROM records WHERE NAME = '500';")
      }
    })

    await db.close()

    t.comment(Math.round(ops / elapsed * 1e3) + ' ops/s')
  })
})

test('select 100', async (t) => {
  const ops = 1000

  await t.test('sqlite3-native', async (t) => {
    const SQLite = require('.')

    const db = new SQLite()

    await db.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')

    for (let i = 0; i < 1000; i++) {
      await db.exec(`INSERT INTO records (NAME) values ('${i}');`)
    }

    const elapsed = await t.execution(async () => {
      for (let i = 0; i < ops; i++) {
        await db.exec('SELECT NAME FROM records LIMIT 100')
      }
    })

    await db.close()

    t.comment(Math.round(ops / elapsed * 1e3) + ' ops/s')
  })
})

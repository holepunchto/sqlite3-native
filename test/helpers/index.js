const SQLite3 = require('../..')

exports.create = function create(t) {
  const db = new SQLite3()
  t.teardown(() => db.close())
  return db
}

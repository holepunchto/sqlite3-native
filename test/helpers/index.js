const PAGE_SIZE = 4096

const SQLite3 = require('../..')

exports.create = function create(t) {
  const db = new SQLite3()
  t.teardown(() => db.close())
  return db
}

exports.pageify = function pageify(mem) {
  const pages = []

  for (let i = 0; i < mem.buffer.byteLength; i += PAGE_SIZE) {
    pages.push(mem.buffer.subarray(i, i + PAGE_SIZE))
  }

  return pages
}

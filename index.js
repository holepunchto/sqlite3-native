const ReadyResource = require('ready-resource')
const b4a = require('b4a')
const binding = require.addon()

module.exports = class BareSQLite3 extends ReadyResource {
  constructor (name) {
    super()
    this.name = name

    this._handle = b4a.allocUnsafe(binding.sizeof_bare_sqlite3_t)
  }

  _open () {
    binding.bare_sqlite3_open(
      this._handle,
      this,
      this.name,
      this._onVFSRead,
      this._onVFSWrite
    )
  }

  _close () {
    binding.bare_sqlite3_close(this._handle)
  }

  _onVFSRead (filetype, buf, offset) {
    console.log('JS READ:', { filetype, buf, offset })
  }

  _onVFSWrite (filetype, buf, offset) {
    console.log('JS WRITE:', { filetype, buf, offset })
  }

  _exec (query) {

  }
}

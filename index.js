const ReadyResource = require('ready-resource')
const binding = require('./binding')
const VFS = require('./lib/vfs')
const MemoryVFS = require('./lib/memory-vfs')

module.exports = exports = class SQLite3 extends ReadyResource {
  constructor (opts = {}) {
    const {
      name = 'sqlite3.db',
      vfs = new MemoryVFS()
    } = opts

    super()

    this.name = name
    this.vfs = vfs

    this._result = null

    this._handle = binding.init(this, this._onexec)
  }

  _open () {
    binding.open(this._handle, this.name, this.vfs._handle)
  }

  _close () {
    if (this.opened) binding.close(this._handle)
    this.vfs.destroy()
  }

  _onexec (rows, columns) {
    this._result.push({ rows, columns })
  }

  async exec (query) {
    if (this.opened === false) await this.ready()
    this._result = []
    binding.exec(this._handle, query)
    return this._result
  }
}

exports.VFS = VFS
exports.MemoryVFS = MemoryVFS

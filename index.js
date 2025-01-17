const ReadyResource = require('ready-resource')
const binding = require('./binding')
const VFS = require('./lib/vfs')
const MemoryVFS = require('./lib/memory-vfs')

module.exports = exports = class SQLite3 extends ReadyResource {
  constructor(opts = {}) {
    const { name = 'sqlite3.db', vfs = new MemoryVFS() } = opts

    super()

    this.name = name

    this._vfs = vfs

    this._handle = binding.init(this)
  }

  async exec(query) {
    if (this.opened === false) await this.ready()

    return binding.exec(this._handle, query)
  }

  async _open() {
    await binding.open(this._handle, this._vfs._handle, this.name)
  }

  async _close() {
    if (this.opened) await binding.close(this._handle)

    this._vfs.destroy()
  }
}

exports.VFS = VFS
exports.MemoryVFS = MemoryVFS

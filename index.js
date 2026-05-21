const ReadyResource = require('ready-resource')
const binding = require('./binding')
const VFS = require('./lib/vfs')
const MemoryVFS = require('./lib/memory-vfs')
const constants = require('./lib/constants')

module.exports = exports = class SQLite3 extends ReadyResource {
  constructor(opts = {}) {
    const { name = 'sqlite3.db', vfs = new MemoryVFS(), extensions = false } = opts

    super()

    this._name = name
    this._vfs = vfs
    this._extensions = extensions

    this._handle = binding.init(this)
  }

  get name() {
    return this._name
  }

  async exec(query) {
    if (this.opened === false) await this.ready()

    return binding.exec(this._handle, query)
  }

  async loadExtension(path, entry = null) {
    if (this._extensions === false) throw new Error('Extension loading is disabled')

    if (this.opened === false) await this.ready()

    return binding.loadExtension(this._handle, path, entry)
  }

  async _open() {
    await binding.open(this._handle, this._vfs._handle, this._name, this._extensions)
  }

  async _close() {
    if (this.opened) await binding.close(this._handle)

    this._vfs.destroy()
  }
}

exports.VFS = VFS
exports.MemoryVFS = MemoryVFS
exports.constants = constants

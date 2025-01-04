const binding = require('../binding')

module.exports = class VFS {
  constructor(opts = {}) {
    const { open } = opts

    if (open) this._open = open

    this._files = [null, null, null]

    this._handle = binding.vfsInit(
      this,
      this._access,
      this._size,
      this._read,
      this._write,
      this._delete
    )
  }

  destroy() {
    if (this._handle === null) return
    binding.vfsDestroy(this._handle)
    this._handle = null
  }

  async _open(type) {}

  async _access(type, cb) {
    cb(null, this._files[type] !== null)
  }

  async _size(type, cb) {
    const file = this._files[type]

    cb(null, file ? file.size : 0)
  }

  async _read(type, arrayBuffer, offset, cb) {
    const buffer = Buffer.from(arrayBuffer)

    let file = this._files[type]
    if (file === null) file = this._files[type] = await this._open(type)

    let stored = await file.read(offset, offset + buffer.byteLength)
    if (stored < buffer.byteLength)
      stored = Buffer.concat([
        stored,
        Buffer.alloc(buffer.byteLength - stored.byteLength)
      ])

    buffer.set(stored, 0)

    cb(null)
  }

  async _write(type, arrayBuffer, offset, cb) {
    const buffer = Buffer.from(arrayBuffer)

    let file = this._files[type]
    if (file === null) file = this._files[type] = await this._open(type)

    await file.write(offset, buffer)

    cb(null)
  }

  async _delete(type, cb) {
    const file = this._files[type]
    if (file !== null && file.unlink) await file.unlink()

    this._files[type] = null

    cb(null)
  }
}

const binding = require('../binding')

module.exports = class VFS {
  constructor (opts = {}) {
    const {
      open
    } = opts

    if (open) this._open = open

    this._files = [null, null, null]

    this._handle = binding.vfsInit(this,
      this._access,
      this._size,
      this._read,
      this._write,
      this._delete
    )
  }

  destroy () {
    if (this._handle === null) return
    binding.vfsDestroy(this._handle)
    this._handle = null
  }

  _open (type) {}

  _access (type) {
    return this._files[type] !== null
  }

  _size (type) {
    const file = this._files[type]
    return file ? file.size : 0
  }

  _read (type, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)

    let file = this._files[type]
    if (file === null) file = this._files[type] = this._open(type)

    let stored = file.read(offset, offset + buffer.byteLength)
    if (stored < buffer.byteLength) stored = Buffer.concat([stored, Buffer.alloc(buffer.byteLength - stored.byteLength)])

    buffer.set(stored, 0)
  }

  _write (type, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)

    let file = this._files[type]
    if (file === null) file = this._files[type] = this._open(type)

    file.write(offset, buffer)
  }

  _delete (type) {
    const file = this._files[type]
    if (file && file.unlink) file.unlink()
    this._files[type] = null
  }
}

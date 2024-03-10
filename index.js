const ReadyResource = require('ready-resource')
const b4a = require('b4a')
const binding = require.addon()

const EMPTY = Buffer.alloc(0)

module.exports = class BareSQLite3 extends ReadyResource {
  constructor (name) {
    super()
    this.name = name

    this._handle = b4a.allocUnsafe(binding.sizeof_bare_sqlite3_t)
    this._files = new Array(8)
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

  _onVFSRead (filetype, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)

    let stored = (this._files[filetype - 1] || EMPTY).subarray(offset, offset + buffer.byteLength)
    if (stored < buffer.byteLength) stored = Buffer.concat([stored, Buffer.alloc(buffer.byteLength - stored.byteLength)])

    buffer.set(stored, 0)

    console.log('JS READ:', { filetype, offset, byteLength: buffer.byteLength })
  }

  _onVFSWrite (filetype, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)
    const size = buffer.byteLength + offset

    let stored = this._files[filetype - 1] || Buffer.alloc(4096)

    let storedSize = stored.byteLength
    while (storedSize < size) storedSize *= 2

    if (stored.byteLength < storedSize) {
      stored = Buffer.concat([stored, Buffer.alloc(storedSize - stored.byteLength)])
    }

    this._files[filetype - 1] = stored
    stored.set(buffer, offset)

    console.log('JS WRITE:', { filetype, offset, byteLength: buffer.byteLength })
  }

  exec (query) {
    binding.bare_sqlite3_exec(this._handle, b4a.byteLength(query), query)
  }
}

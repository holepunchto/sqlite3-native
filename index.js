const ReadyResource = require('ready-resource')
const b4a = require('b4a')
const binding = require.addon()

const EMPTY = Buffer.alloc(0)
const LE = (new Uint8Array(new Uint16Array([255]).buffer))[0] === 0xff

module.exports = class BareSQLite3 extends ReadyResource {
  constructor (name) {
    super()
    this.name = name

    this._handle = b4a.allocUnsafe(binding.sizeof_bare_sqlite3_t)
    this._files = new Array(3)
  }

  _open () {
    binding.bare_sqlite3_open(
      this._handle,
      this,
      this.name,
      this._onVFSSize,
      this._onVFSRead,
      this._onVFSWrite,
      this._onVFSDelete
    )
  }

  _close () {
    if (this.opened === false) return
    binding.bare_sqlite3_close(this._handle)
  }

  _onVFSSize (type, arrayBuffer) {
    const stored = this._files[type] || EMPTY
    writeUint64(arrayBuffer, stored.byteLength)
    console.log('JS SIZE', { type, byteLength: stored.byteLength })
  }

  _onVFSRead (type, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)

    let stored = (this._files[type] || EMPTY).subarray(offset, offset + buffer.byteLength)
    if (stored < buffer.byteLength) stored = Buffer.concat([stored, Buffer.alloc(buffer.byteLength - stored.byteLength)])

    buffer.set(stored, 0)

    console.log('JS READ:', { type, offset, byteLength: buffer.byteLength })
  }

  _onVFSWrite (type, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)
    const size = buffer.byteLength + offset

    let stored = this._files[type] || Buffer.alloc(4096)

    let storedSize = stored.byteLength
    while (storedSize < size) storedSize *= 2

    if (stored.byteLength < storedSize) {
      stored = Buffer.concat([stored, Buffer.alloc(storedSize - stored.byteLength)])
    }

    this._files[type] = stored
    stored.set(buffer, offset)

    console.log('JS WRITE:', { type, offset, byteLength: buffer.byteLength })
  }

  _onVFSDelete (type) {
    this._files[type] = undefined
    console.log('JS DELETE', { type })
  }

  async exec (query) {
    if (this.opened === false) await thsi.ready()
    binding.bare_sqlite3_exec(this._handle, query)
  }
}

function writeUint64 (arrayBuffer, n) {
  const uint32s = new Uint32Array(arrayBuffer)

  const l = (n & 0xffffffff) >>> 0
  const h = (n - l) / 0x100000000

  if (LE) {
    uint32s[0] = l
    uint32s[1] = h
  } else {
    uint32s[0] = h
    uint32s[1] = l
  }
}

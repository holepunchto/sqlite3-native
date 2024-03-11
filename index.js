const ReadyResource = require('ready-resource')
const binding = require.addon()

const LE = (new Uint8Array(new Uint16Array([255]).buffer))[0] === 0xff

module.exports = class BareSQLite3 extends ReadyResource {
  constructor (name, open) {
    super()

    this.name = name
    this.open = open

    this._handle = Buffer.allocUnsafe(binding.sizeof_bare_sqlite3_t)
    this._files = [null, null, null]
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
    const file = this._files[type]
    writeUint64(arrayBuffer, file ? file.size : 0)
  }

  _onVFSRead (type, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)

    let file = this._files[type]
    if (file === null) file = this._files[type] = this.open(type)

    let stored = file.read(offset, offset + buffer.byteLength)
    if (stored < buffer.byteLength) stored = Buffer.concat([stored, Buffer.alloc(buffer.byteLength - stored.byteLength)])

    buffer.set(stored, 0)
  }

  _onVFSWrite (type, arrayBuffer, offset) {
    const buffer = Buffer.from(arrayBuffer)

    let file = this._files[type]
    if (file === null) file = this._files[type] = this.open(type)

    file.write(offset, buffer)
  }

  _onVFSDelete (type) {
    const file = this._files[type]
    if (file && file.unlink) file.unlink()
    this._files[type] = null
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

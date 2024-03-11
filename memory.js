const EMPTY = Buffer.alloc(0)

module.exports = class MemoryVFS {
  constructor () {
    this.buffer = EMPTY
    this.size = 0
  }

  read (start, end) {
    return this.buffer.subarray(start, end)
  }

  write (start, buffer) {
    let size = this.buffer.byteLength || 4096
    while (size < start + buffer.byteLength) size *= 2

    if (size > this.buffer.byteLength) {
      this.size = start + buffer.byteLength
      const buf = Buffer.alloc(size)
      buf.set(this.buffer, 0)
      this.buffer = buf
    }

    this.buffer.set(buffer, start)
  }

  unlink () {
    this.buffer = EMPTY
    this.size = 0
  }
}

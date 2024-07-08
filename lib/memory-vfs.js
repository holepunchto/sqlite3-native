const VFS = require('./vfs')

const EMPTY = Buffer.alloc(0)

module.exports = class MemoryVFS extends VFS {
  _open () {
    return new MemoryVFSFile()
  }
}

class MemoryVFSFile {
  constructor () {
    this.buffer = EMPTY
    this.size = 0
  }

  pages ({ copy = true } = {}) {
    const all = []
    for (let i = 0; i < this.buffer.byteLength; i += 4096) {
      const value = this.buffer.subarray(i, i + 4096)
      all.push({
        index: i / 4096,
        value: copy ? Buffer.concat([value]) : value
      })
    }
    return all
  }

  read (start, end) {
    return this.buffer.subarray(start, end)
  }

  write (start, buffer) {
    const end = start + buffer.byteLength

    let size = this.buffer.byteLength || 4096
    while (size < end) size *= 2

    if (size > this.buffer.byteLength) {
      const buf = Buffer.alloc(size)
      buf.set(this.buffer, 0)
      this.buffer = buf
    }
    if (end > this.size) {
      this.size = end
    }

    this.buffer.set(buffer, start)
  }

  unlink () {
    this.buffer = EMPTY
    this.size = 0
  }
}

const binding = require('../binding')

const DEFAULT_PAGE_SIZE = 4096

// A VFS backed by native file IO on a local cache file. The main database
// keeps a presence bitmap (sidecar <path>.map); reads of pages that are not
// locally present invoke _miss(buffer, index) so a subclass can fetch the
// page from elsewhere before the read is served natively.
//
// applyPages(updates, size) patches pages into the cache file and marks them
// present — the checkpoint path for keeping the cache in sync with a remote
// source of truth. It must only be called while no statement is executing.
module.exports = class CacheVFS {
  constructor(path, opts = {}) {
    const { pageSize = DEFAULT_PAGE_SIZE, miss } = opts

    if (miss) this._miss = miss

    this.path = path
    this.pageSize = pageSize

    this._handle = binding.cacheVfsInit(this, path, pageSize, this._onmiss)
  }

  destroy() {
    if (this._handle === null) return
    binding.cacheVfsDestroy(this._handle)
    this._handle = null
  }

  // fill buffer with the contents of page index; zeros are kept as is
  async _miss(buffer, index) {
    throw new Error('Page ' + index + ' is not locally present and no miss handler is set')
  }

  _onmiss(arrayBuffer, index, done) {
    Promise.resolve(this._miss(Buffer.from(arrayBuffer), index)).then(
      () => done(null),
      (err) => done(err)
    )
  }

  // updates: iterable of { index, page }, size: logical file size afterwards
  // (-1 keeps the current size)
  applyPages(updates, size = -1) {
    const batch = Array.from(updates)

    const indices = new Uint32Array(batch.length)
    const pages = Buffer.alloc(batch.length * this.pageSize)

    for (let i = 0; i < batch.length; i++) {
      indices[i] = batch[i].index
      batch[i].page.copy(pages, i * this.pageSize)
    }

    binding.cacheVfsApply(this._handle, indices, pages, size)
  }
}

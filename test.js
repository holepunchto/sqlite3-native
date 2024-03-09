const test = require('brittle')
const addon = require('.')
const c = require('compact-encoding')

function makeUintBe (n) {
  const start = Math.pow(256, n - 1)

  return {
    decode (state) {
      let res = 0
      let factor = start
      for (let i = 0; i < n; i++) {
        res += factor * state.buffer[state.start++]
        factor /= 256
      }
      return res
    }
  }
}

const uint16be = makeUintBe(2)
const uint24be = makeUintBe(3)
const uint32be = makeUintBe(4)
const uint40be = makeUintBe(5)
const uint48be = makeUintBe(6)
const uint56be = makeUintBe(7)
const uint64be = makeUintBe(8)

const varint = {
  decode (state) {
    const b = state.buffer[state.start++]
    if (b <= 240) return b
    if (b <= 248) return 240 + 256 * (b - 241) + state.buffer[state.start++]
    if (b === 249) return 2288 + uint16be.decode(state)
    if (b === 250) return uint24be.decode(state)
    if (b === 251) return uint32be.decode(state)
    if (b === 252) return uint40be.decode(state)
    if (b === 253) return uint48be.decode(state)
    if (b === 254) return uint56be.decode(state)
    return uint64be.decode(state)
  }
}

const BTreePage = {
  decode (state) {
    const type = c.uint8.decode(state)
    const firstFreeBlock = uint16be.decode(state)
    const numberOfCells = uint16be.decode(state)
    const startOfCellContent = uint16be.decode(state)
    const fragmentedFreeBytes = c.uint8.decode(state)
    const rightMostPointer = (type === 2 || type === 5) ? uint32be.decode(state) : 0

    const cells = new Array(numberOfCells)

    if (type === 13) {
      let end = state.buffer.byteLength

      for (let i = 0; i < cells.length; i++) {
        const start = uint16be.decode(state)
        const buffer = state.buffer.subarray(start, end)
        const cellState = { start, end, buffer: state.buffer }
        const length = varint.decode(cellState)
        const key = varint.decode(cellState)
        const free = state.end - start.start
        const overflow = free < length ? length - free - 4 : 0
        const initialPortion = state.buffer.subarray(cellState.start, cellState.end - (overflow ? 4 : 0))
        if (overflow) cellState.start = cellState.end - 4
        const overflowPage = overflow ? uint32be.decode(cellState) : 0

        cells[i] = {
          byteOffset: start,
          byteLength: cellState.end,
          buffer,
          length,
          key,
          overflow,
          overflowPage,
          initialPortion,
          initialPortionString: initialPortion.toString()
        }

        end = start
      }
    }

    return {
      type,
      firstFreeBlock,
      numberOfCells,
      startOfCellContent,
      fragmentedFreeBytes,
      rightMostPointer,
      cells
    }
  }
}

const pages = new Map()

function diff (n, o) {
  const changes = []

  if (!o) {
    for (const e of n.cells) {
      changes.push({ type: 'put', key: e.key, buffer: e.buffer })
    }
    return changes
  }

  const touched = new Set()
  for (const e of n.cells) {
    if (hasCell(o, e)) continue
    touched.add(e.key)
    changes.push({ type: 'put', key: e.key, buffer: e.buffer })
  }

  for (const e of o.cells) {
    if (touched.has(e.key)) continue
    if (hasCell(n, e)) continue
    changes.push({ type: 'del', key: e.key, buffer: null })
  }

  return changes
}

function hasCell (page, e) {
  // TODO: bisect
  for (const c of page.cells) {
    if (c.key === e.key && c.buffer.equals(e.buffer)) return true
  }

  return false
}

function parseBTree (index, buf) {
  const state = { start: 0, end: buf.byteLength, buffer: Buffer.from(buf) } // copy

  const bt = BTreePage.decode(state)

  const old = pages.get(index)

  pages.set(index, bt)

  console.log('btree', index, diff(bt, old))
}

function trace (filename, arrayBuf, offset) {
  if (filename === 'example.db-journal') return

  const buf = Buffer.from(arrayBuf)

  if (offset === 0) {
    console.log('page 0 stuff', buf.slice(32, 36))
    return
  }

  parseBTree(offset / 4096, buf)

  // console.log('filename:', filename, 'length:', buf.byteLength, 'offset:', offset / 4096)
}

test('can generate database', () => {
  addon.generateDatabase(trace)
})

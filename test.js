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

    let end = state.buffer.byteLength

    for (let i = 0; i < cells.length; i++) {
      const start = uint16be.decode(state)
      const buffer = state.buffer.subarray(start, end)
      const cellState = { start, end, buffer: state.buffer }

      if (type === 13) {
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
          tableLeaf: {
            key,
            length,
            overflow,
            overflowPage,
            initialPortion,
            initialPortionString: initialPortion.toString()
          }
        }
      } else {
        cells[i] = {
          byteOffset: start,
          byteLength: cellState.end,
          buffer,
          tableLeaf: null
        }
      }

      end = start
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
      changes.push({ type: 'insert', index: changes.length, buffer: e.buffer })
    }
    return changes
  }

  const cells = o.cells.slice(0)

  for (let i = 0; i < cells.length; i++) {
    const e = cells[i]
    const j = indexOfCell(n.cells, e)

    if (j === -1) {
      cells.splice(i, 1)
      changes.push({ type: 'delete', index: i })
      i--
    }
  }

  for (let i = 0; i < n.cells.length; i++) {
    const e = n.cells[i]
    const j = indexOfCell(cells, e)

    if (j === i) continue

    if (j === -1) {
      cells.splice(i, 0, e)
      changes.push({ type: 'insert', index: i, buffer: e.buffer })
      continue
    }

    changes.push({ type: 'move', index: i, from: j })

    for (; j > i; j--) {
      cells[j] = cells[--j]
      cells[j] = e
    }
  }

  // quick dedup thing, can prop be a lot better...
  if (changes.length >= 2) {
    const a = changes[0]
    const b = changes[1]

    if (a.type === 'delete' && b.type === 'insert') {
      if (a.index === b.index) {
        changes.shift()
        b.type = 'overwrite'
      }
    }
  }

  return changes
}

function indexOfCell (cells, e) {
  // TODO: bisect
  for (let i = 0; i < cells.length; i++) {
    const c = cells[i]
    if (c === null) continue
    if (c.buffer.equals(e.buffer)) return i
  }

  return -1
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

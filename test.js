const test = require('brittle')
const addon = require('.')

function trace (filename, arrayBuf, offset) {
  if (filename === 'example.db-journal') return
  const buf = Buffer.from(arrayBuf)
  console.log('filename:', filename, 'length:', buf.byteLength, 'offset:', offset / 4096)
}

test('can generate database', () => {
  addon.generateDatabase(trace)
})

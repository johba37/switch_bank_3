// Navigation lights logic (custom Niovi plugin, no upstream).
//
// Lights and where they are switched:
//   - red over green (masthead all-round, COLREG 25(c)): C-Zone bank 1 switch 4
//   - deck level sidelights: "Nav lights" in the kiosk = switch bank 4 switch 1
//   - stern light: Switch Bank 3 (repo switch_bank_3) switch 3, logic only
//
// Red over green may only be shown in addition to sidelights and stern light,
// and sidelights always need the stern light. So this plugin
//   - switches the sidelights on when red over green goes on, and back to
//     how they were before when it goes off
//   - keeps the sidelights on while red over green is on
//   - keeps the stern light on while sidelights or red over green are on
// It re-sends a command until the bank's 127501 status agrees, so lights come
// back after a switch bank reboots.
//
// C-Zone status isn't mapped by Signal K: PGN 65284 is decoded by canboatjs as
// "CZone Circuit Status" (dipswitch = bank, type 30 = switch summary, bitmap
// bit n-1 = switch n), so it is read from the raw N2K stream here.

const PLUGIN_ID = 'nav-lights'
const OUT_PATH = 'electrical.navigationLights'
const CZONE_SWITCH_SUMMARY = 30
const RESEND_MS = 2000

module.exports = function (app) {
  let timer = null
  let onN2k = null
  let options = {}
  let redOverGreen = null        // null until the first C-Zone status
  let sidelightsBefore = false   // sidelights state when red over green went on
  let lastSent = {}              // key -> ms, to not flood while status catches up
  let lastStatusLog = ''

  const plugin = {
    id: PLUGIN_ID,
    name: 'Navigation lights',
    description:
      'Couples stern light and deck sidelights to the red over green (C-Zone) ' +
      'and the kiosk "Nav lights" switch, as COLREG requires.',
    schema: {
      type: 'object',
      properties: {
        czoneBank: { type: 'number', title: 'C-Zone bank of red over green', default: 1 },
        czoneSwitch: { type: 'number', title: 'C-Zone switch of red over green', default: 4 },
        sidelightsBank: { type: 'number', title: 'Sidelights switch bank', default: 4 },
        sidelightsSwitch: { type: 'number', title: 'Sidelights switch', default: 1 },
        sternBank: { type: 'number', title: 'Stern light switch bank', default: 3 },
        sternSwitch: { type: 'number', title: 'Stern light switch', default: 3 },
        checkInterval: { type: 'number', title: 'Check interval (s)', default: 5 }
      }
    },
    start: start,
    stop: stop
  }

  function start (opts) {
    options = Object.assign(defaults(), opts)
    redOverGreen = null
    sidelightsBefore = false
    lastSent = {}

    app.handleMessage(PLUGIN_ID, {
      updates: [{
        meta: [
          { path: OUT_PATH + '.redOverGreen', value: { description: 'Red over green on (C-Zone), null until first seen' } },
          { path: OUT_PATH + '.sidelights', value: { description: 'Deck sidelights as reported by their switch bank' } },
          { path: OUT_PATH + '.sternLight', value: { description: 'Stern light wanted by this plugin' } }
        ]
      }]
    })

    onN2k = handleN2k
    app.on('N2KAnalyzerOut', onN2k)
    timer = setInterval(evaluate, options.checkInterval * 1000)
    evaluate()
  }

  function stop () {
    if (timer) {
      clearInterval(timer)
      timer = null
    }
    if (onN2k) {
      app.removeListener('N2KAnalyzerOut', onN2k)
      onN2k = null
    }
  }

  function defaults () {
    const d = {}
    for (const [key, prop] of Object.entries(plugin.schema.properties)) {
      d[key] = prop.default
    }
    return d
  }

  function setStatus (msg) {
    if (msg === lastStatusLog) return
    lastStatusLog = msg
    if (typeof app.setPluginStatus === 'function') {
      app.setPluginStatus(msg)
    }
  }

  function handleN2k (n2k) {
    if (!n2k.fields) return
    if (n2k.pgn === 65284) {
      const f = n2k.fields
      if (f.dipswitch !== options.czoneBank || f.type !== CZONE_SWITCH_SUMMARY || typeof f.bitmap !== 'number') return
      setRedOverGreen(Boolean((f.bitmap >> (options.czoneSwitch - 1)) & 1))
    } else if (n2k.pgn === 127501) {
      const instance = n2k.fields.instance
      // Let Signal K map this status into the model before reading it back
      if (instance === options.sidelightsBank || instance === options.sternBank) setTimeout(evaluate, 200)
    }
  }

  function setRedOverGreen (on) {
    if (on === redOverGreen) return
    const first = redOverGreen === null
    redOverGreen = on
    if (first) {
      // Plugin (re)start: only enforce, don't treat as a switch event
      app.debug('red over green initially %s', on ? 'on' : 'off')
    } else if (on) {
      sidelightsBefore = switchState(options.sidelightsBank, options.sidelightsSwitch) === true
      app.debug('red over green on, sidelights were %s', sidelightsBefore ? 'on' : 'off')
    } else {
      app.debug('red over green off, sidelights back to %s', sidelightsBefore ? 'on' : 'off')
      sendSwitch(options.sidelightsBank, options.sidelightsSwitch, sidelightsBefore)
    }
    evaluate()
  }

  function switchState (bank, index) {
    const node = app.getSelfPath('electrical.switches.bank.' + bank + '.' + index + '.state')
    if (!node || node.value === null || node.value === undefined) return undefined
    return node.value === 1 || node.value === true
  }

  function evaluate () {
    const rog = redOverGreen === true

    // Red over green needs the sidelights
    let sidelights = switchState(options.sidelightsBank, options.sidelightsSwitch)
    if (rog && sidelights !== true) {
      sendSwitch(options.sidelightsBank, options.sidelightsSwitch, true)
      sidelights = true // as commanded, the status will follow
    }

    // Sidelights or red over green need the stern light
    const stern = rog || sidelights === true
    if (switchState(options.sternBank, options.sternSwitch) !== stern) {
      sendSwitch(options.sternBank, options.sternSwitch, stern)
    }

    publish({ redOverGreen: redOverGreen, sidelights: sidelights === undefined ? null : sidelights, sternLight: stern })

    const text = (v) => (v === true ? 'on' : v === false ? 'off' : '?')
    setStatus('Red over green ' + text(redOverGreen) + ', sidelights ' + text(sidelights) + ' -> stern light ' + text(stern))
  }

  function sendSwitch (bank, index, on) {
    const key = bank + '.' + index + '.' + on
    if (Date.now() - (lastSent[key] || 0) < RESEND_MS) return
    lastSent[key] = Date.now()
    // Switch Bank Control: instance, then 2 bits per switch (01 on, 00 off, 11 no change)
    const bytes = [bank, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]
    const bit = (index - 1) * 2
    const byte = 1 + Math.floor(bit / 8)
    const shift = bit % 8
    bytes[byte] = (bytes[byte] & ~(0x03 << shift)) | ((on ? 1 : 0) << shift)
    const hex = bytes.map((b) => b.toString(16).padStart(2, '0')).join(',')
    // Actisense format; canboatjs replaces the source with our own address
    app.emit('nmea2000out', `${new Date().toISOString()},3,127502,0,255,8,${hex}`)
    app.debug('127502 bank %d switch %d -> %s', bank, index, on ? 'on' : 'off')
  }

  function publish (values) {
    app.handleMessage(PLUGIN_ID, {
      updates: [{
        values: Object.entries(values).map(([key, value]) => ({ path: OUT_PATH + '.' + key, value }))
      }]
    })
  }

  return plugin
}

// Engine room fan control (custom Niovi plugin, no upstream).
//
// Two PWM fans in series cool the motor compartment. They hang off the
// "Switch Bank 3" ESP32 (repo switch_bank_3, N2K source 169):
//   - power:  relay 1 of switch bank 3, PGN 127502 / status 127501
//   - speed:  PGN 127500 Load Controller, Connection ID 1, byte 5 = duty 0-100 %
//   - RPM:    PGN 127488 engine instances 2 and 3 -> propulsion.2/3.revolutions
//
// This plugin is the brain: it watches the (electric) motor temperature and
//   - switches the fans on above `fansOnTemp`, off below `fansOffTemp`
//   - while on, sends a duty every few seconds: `minDuty` at `fansOnTemp`
//     rising linearly to 100 % at `fullSpeedTemp`
//   - keeps re-sending the relay command until the bank's 127501 status agrees,
//     so the fans come back after the board reboots (relay 1 boots off)
//
// Manual mode: switching the fans on from elsewhere (the kiosk "Blower"
// button sends 127502 bank 3 switch 1 ON) while they are off starts manual
// mode: fans stay on at `manualDuty`, or faster if the curve asks for more,
// until an OFF command for the switch arrives. The curve still wins on safety:
// above `fansOnTemp` an OFF ends manual mode, but the fans stay on.
//
// Failsafes: the board runs the fans at 100 % when it gets no duty for 30 s.
// If the temperature goes stale, this plugin switches the fans on and stops
// sending duty, so the board falls back to full speed.
//
// The board's own 127500 status (applied duty, failsafe flag) isn't mapped by
// Signal K, so it is decoded here and published under electrical.fans.engineRoom.

const PLUGIN_ID = 'engine-room-fans'
const OUT_PATH = 'electrical.fans.engineRoom'
const KELVIN = 273.15

module.exports = function (app) {
  let timer = null
  let onN2k = null
  let options = {}
  let fansOn = false        // relay wanted on
  let autoOn = false        // temperature curve wants the fans on (with hysteresis)
  let manual = false        // switched on by hand, until an OFF command
  let lastTemp = null       // °C
  let lastTempAt = 0        // ms
  let lastStatusLog = ''

  const plugin = {
    id: PLUGIN_ID,
    name: 'Engine room fans',
    description:
      'Switches and speed-controls the engine room fans on Switch Bank 3 ' +
      'from the engine oil temperature (PGN 127502 power, PGN 127500 duty).',
    schema: {
      type: 'object',
      properties: {
        temperaturePath: { type: 'string', title: 'Temperature path', default: 'propulsion.port.oilTemperature' },
        fansOnTemp: { type: 'number', title: 'Fans on at (°C)', default: 25 },
        fansOffTemp: { type: 'number', title: 'Fans off below (°C)', default: 22 },
        fullSpeedTemp: { type: 'number', title: 'Full speed at (°C)', default: 45 },
        minDuty: { type: 'number', title: 'Minimum duty while on (%)', default: 30 },
        manualDuty: { type: 'number', title: 'Manual mode duty (%), when switched on by hand', default: 50 },
        staleTimeout: { type: 'number', title: 'Temperature stale after (s): fans on at full speed', default: 60 },
        sendInterval: { type: 'number', title: 'Send interval (s), must be well below the board\'s 30 s timeout', default: 5 },
        switchBank: { type: 'number', title: 'Switch bank instance', default: 3 },
        switchIndex: { type: 'number', title: 'Fan power switch', default: 1 },
        connectionId: { type: 'number', title: 'PGN 127500 Connection ID', default: 1 }
      }
    },
    start: start,
    stop: stop
  }

  function start (opts) {
    options = Object.assign(defaults(), opts)
    fansOn = false
    autoOn = false
    manual = false
    lastTemp = null
    lastTempAt = Date.now() // grace period before declaring the temperature stale

    app.handleMessage(PLUGIN_ID, {
      updates: [{
        meta: [
          { path: OUT_PATH + '.commandedDutyCycle', value: { units: 'ratio', description: 'Fan duty sent to the board, null when none is sent' } },
          { path: OUT_PATH + '.dutyCycle', value: { units: 'ratio', description: 'Fan duty applied by the board' } },
          { path: OUT_PATH + '.failsafe', value: { description: 'Board runs fans at full speed because no duty was received' } },
          { path: OUT_PATH + '.powered', value: { description: 'Fan power relay as reported by the board' } },
          { path: OUT_PATH + '.state', value: { description: 'Fan power wanted by this plugin (on/off)' } },
          { path: OUT_PATH + '.mode', value: { description: 'auto: temperature curve, manual: switched on by hand until switched off' } }
        ]
      }]
    })

    onN2k = handleN2k
    app.on('N2KAnalyzerOut', onN2k)
    timer = setInterval(tick, options.sendInterval * 1000)
    tick()
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

  function readTemperature () {
    const node = app.getSelfPath(options.temperaturePath)
    if (!node || typeof node.value !== 'number') return
    const ts = node.timestamp ? Date.parse(node.timestamp) : NaN
    if (!isNaN(ts) && ts <= lastTempAt && lastTemp !== null) return // nothing new
    lastTemp = node.value - KELVIN
    lastTempAt = isNaN(ts) ? Date.now() : ts
  }

  function dutyFor (temp) {
    const span = options.fullSpeedTemp - options.fansOnTemp
    const ratio = span > 0 ? (temp - options.fansOnTemp) / span : 1
    const clamped = Math.min(1, Math.max(0, ratio))
    return Math.round(options.minDuty + (100 - options.minDuty) * clamped)
  }

  function tick () {
    readTemperature()
    const stale = Date.now() - lastTempAt > options.staleTimeout * 1000

    if (!stale) {
      if (lastTemp >= options.fansOnTemp) autoOn = true
      else if (lastTemp < options.fansOffTemp) autoOn = false
    }
    fansOn = stale || autoOn || manual

    let duty = null // stale: no duty sent, the board falls back to 100 %
    if (!stale && fansOn) {
      duty = Math.max(autoOn ? dutyFor(lastTemp) : 0, manual ? options.manualDuty : 0)
    }

    syncRelay()
    if (duty !== null) sendDuty(duty)

    publish({
      commandedDutyCycle: duty === null ? null : duty / 100,
      state: fansOn ? 'on' : 'off',
      mode: manual ? 'manual' : 'auto'
    })

    const tempText = lastTemp === null ? 'no temperature' : lastTemp.toFixed(1) + ' °C'
    const modeText = manual ? ' (manual)' : ''
    if (stale) setStatus('Temperature stale (' + tempText + '): fans on, board failsafe full speed' + modeText)
    else if (fansOn) setStatus(tempText + ': fans on at ' + duty + ' %' + modeText)
    else setStatus(tempText + ': fans off')
  }

  // Re-send the relay command until the bank reports the wanted state
  function syncRelay () {
    const node = app.getSelfPath('electrical.switches.bank.' + options.switchBank + '.' + options.switchIndex + '.state')
    const reported = node ? node.value : undefined
    const wanted = fansOn ? 1 : 0
    if (reported === wanted || reported === Boolean(wanted)) return
    sendSwitch(fansOn)
  }

  function sendSwitch (on) {
    // Switch Bank Control: instance, then 2 bits per switch (01 on, 00 off, 11 no change)
    const bytes = [options.switchBank, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]
    const bit = (options.switchIndex - 1) * 2
    const byte = 1 + Math.floor(bit / 8)
    const shift = bit % 8
    bytes[byte] = (bytes[byte] & ~(0x03 << shift)) | ((on ? 1 : 0) << shift)
    send(3, 127502, bytes)
    app.debug('127502 bank %d switch %d -> %s', options.switchBank, options.switchIndex, on ? 'on' : 'off')
  }

  function sendDuty (duty) {
    // Load Controller Connection State/Control: only Connection ID and PWM duty are used
    send(6, 127500, [0xff, options.connectionId, 0xff, 0xff, 0xff, duty, 0xff, 0xff])
    app.debug('127500 duty %d %%', duty)
  }

  function send (prio, pgn, bytes) {
    // Actisense format; canboatjs replaces the source with our own address
    const hex = bytes.map((b) => b.toString(16).padStart(2, '0')).join(',')
    app.emit('nmea2000out', `${new Date().toISOString()},${prio},${pgn},0,255,${bytes.length},${hex}`)
  }

  function handleN2k (n2k) {
    if (!n2k.fields) return
    if (n2k.pgn === 127502) handleSwitchCommand(n2k.fields)
    else if (n2k.pgn === 127500) handleBoardStatus(n2k.fields)
  }

  // Manual mode from 127502 commands by others (kiosk, MFD). Our own and the
  // board's echoed commands never contradict what we want, so they're no-ops.
  function handleSwitchCommand (f) {
    if (f.instance !== options.switchBank) return
    const cmd = f['switch' + options.switchIndex]
    if (cmd === 'On' && !manual && !fansOn) {
      manual = true
      app.debug('switched on by hand: manual mode')
      tick()
    } else if (cmd === 'Off' && manual) {
      manual = false
      app.debug('switched off by hand: back to auto')
      tick()
    }
  }

  // The board's 127500 status has state/status set; our own commands don't
  function handleBoardStatus (f) {
    if (f.connectionId !== options.connectionId || f.state === undefined) return
    publish({
      dutyCycle: typeof f.pwmDutyCycle === 'number' ? f.pwmDutyCycle / 100 : null,
      failsafe: f.status === 1,
      powered: f.state === 1
    })
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

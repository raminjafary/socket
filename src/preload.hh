#ifndef PRELOAD_HH
#define PRELOAD_HH
//
// This file contains JavaScipt that are injected
// into the webview before any user code is executed.
//

constexpr auto gPreload = R"JS(
  const IPC = window._ipc = { nextSeq: 1, streams: {} }

  window._ipc.resolve = async (seq, status, value) => {
    try {
      value = decodeURIComponent(value)
    } catch (err) {
      console.error(`${err.message} (${value})`)
      return
    }

    try {
      value = JSON.parse(value)
    } catch (err) {
      console.error(`${err.message} (${value})`)
      return
    }

    if (!window._ipc[seq]) {
      console.error('inbound IPC message with unknown sequence:', seq, value)
      return
    }

    if (status === 0) {
      await window._ipc[seq].resolve(value)
    } else {
      const err = new Error(typeof value === 'string' ? value : JSON.stringify(value))
      await window._ipc[seq].reject(err);
    }
    delete window._ipc[seq];
  }

  window._ipc.send = (name, o) => {
    const seq = window._ipc.nextSeq++
    let serialized = ''

    const promise = new Promise((resolve, reject) => {
      window._ipc[seq] = {
        resolve: resolve,
        reject: reject
      }
    })

    try {
      if (({}).toString.call(o) !== '[object Object]') {
        o = { value: o }
      }

      const params = {
        ...o,
        index: window.process.index,
        seq
      }

      serialized = new URLSearchParams(params).toString()

      serialized = serialized.replace(/\+/g, '%20')

    } catch (err) {
      console.error(`${err.message} (${serialized})`)
      return Promise.reject(err.message)
    }

    window.external.invoke(`ipc://${name}?${serialized}`)
    return promise
  }

  window._ipc.emit = (name, value, target, options) => {
    let detail = value

    if (typeof value === 'string') {
      try {
        detail = decodeURIComponent(value)
        detail = JSON.parse(detail)
      } catch (err) {
        // consider okay here because if detail is defined then
        // `decodeURIComponent(value)` was successful and `JSON.parse(value)`
        // was not: there could be bad/unsupported unicode in `value`
        if (!detail) {
          console.error(`${err.message} (${value})`)
          return
        }
      }

      if (detail.event && detail.data && (detail.serverId || detail.clientId)) {
        const stream = window._ipc.streams[detail.serverId || detail.clientId]
        if (!stream) {
          console.error('inbound IPC message with unknown serverId/clientId:', detail)
          return
        }

        const value = detail.event === 'data' ? atob(detail.data) : detail.data

        if (detail.event === 'data') {
          stream.__write(detail.event, value)
        } else if (detail.event) {
          stream.emit(detail.event, value)
        }
      }
    }

    const event = new window.CustomEvent(name, { detail, ...options })
    if (target) {
      target.dispatchEvent(event)
    } else {
      window.dispatchEvent(event)
    }
  }

  window.parent.log = s => {
    window.external.invoke(`ipc://log?value=${s}`)
  }

  window.parent.getConfig = async o => {
    const config = await window._ipc.send('getConfig', o)
    if (!config || typeof config !== 'string') return null

    return decodeURIComponent(config)
      .split('\n')
      .map(trim)
      .filter(filterOutComments)
      .filter(filterOutEmptyLine)
      .map(splitTuple)
      .reduce(reduceTuple, {})

    function trim (line) { return line.trim() }
    function filterOutComments (line) { return !/^\s*#/.test(line) }
    function filterOutEmptyLine (line) { return line && line.length }

    function splitTuple (line) {
      return line.split(/:(.*)/).filter(filterOutEmptyLine).map(trim)
    }

    function reduceTuple (object, tuple) {
      try {
        return Object.assign(object, { [tuple[0]]: JSON.parse(tuple[1]) })
      } catch {
        return Object.assign(object, { [tuple[0]]: tuple[1] })
      }
    }
  }
)JS";

constexpr auto gPreloadDesktop = R"JS(
  window.system.rand64 = () => {
    return window.crypto.getRandomValues(new BigUint64Array(1))[0].toString()
  }

  window.system.send = o => {
    let value = ''

    try {
      value = JSON.stringify(o)
    } catch (err) {
      return Promise.reject(err.message)
    }

    return window._ipc.send('send', { value })
  }

  window.system.exit = o => window._ipc.send('exit', o)
  window.system.openExternal = o => window._ipc.send('external', o)
  window.system.setTitle = o => window._ipc.send('title', o)
  window.system.inspect = o => window.external.invoke(`ipc://inspect`)
  window.system.bootstrap = o => window.external.invoke(`ipc://bootstrap`)
  window.system.reload = o => window.external.invoke(`ipc://reload`)

  window.system.show = (index = 0) => {
    return window._ipc.send('show', { index })
  }

  window.system.hide = (index = 0) => {
    return window._ipc.send('hide', { index })
  }

  window.resizeTo = (width, height) => {
    const index = window.process.index
    const o = new URLSearchParams({ width, height, index }).toString()
    window.external.invoke(`ipc://size?${o}`)
  }

  window.system.udpBind = async (port) => {
    const serverId = window.system.rand64()
    const { err } = await window._ipc.send('udpBind', { port, serverId })
    return { err, data: serverId }
  }

  window.system.udpReadStart = async (serverId) => {
    window.external.invoke(`ipc://udpReadStart?serverId=${serverId}`)
  }

  window.system.setBackgroundColor = opts => {
    opts.index = window.process.index
    const o = new URLSearchParams(opts).toString()
    window.external.invoke(`ipc://background?${o}`)
  }

  window.system.setSystemMenuItemEnabled = value => {
    return window._ipc.send('systemMenuItemEnabled', value)
  }

  Object.defineProperty(window.document, 'title', {
    get () { return window.process.title },
    set (value) {
      const index = window.process.index
      const o = new URLSearchParams({ value, index }).toString()
      window.external.invoke(`ipc://title?${o}`)
    }
  })

  window.system.dialog = async o => {
    const files = await window._ipc.send('dialog', o);
    return typeof files === 'string' ? files.split('\n') : [];
  }

  window.system.setContextMenu = o => {
    o = Object
      .entries(o)
      .flatMap(a => a.join(':'))
      .join('_')
    return window._ipc.send('context', o)
  };

  window.parent = window.system

  if (window?.process?.port > 0) {
    window.addEventListener('menuItemSelected', e => {
      window.location.reload()
    })
  }
)JS";

constexpr auto gPreloadMobile = R"JS(
  window.system.getNetworkInterfaces = o => window._ipc.send('getNetworkInterfaces', o)
  window.system.openExternal = o => {
    window.external.invoke(`ipc://external?value=${encodeURIComponent(o)}`)
  }

  window.system.fs = new class FileSystem {
    async [Symbol.for('system.fs.id')] () {
      return await window._ipc.send('fs.id')
    }

    async request (type, data, opts) {
      const params = { ...data }

      if (!params.id && opts?.id === true) {
        params.id = await this[Symbol.for('system.fs.id')]()
      }

      for (const key in params) {
        if (params[key] === undefined) {
          delete params[key]
        }
      }

      const { value } = await window._ipc.send(type, params)

      if (value.err) {
        throw Object.assign(new Error(value.err.message), value.err)
      }

      return value.data
    }

    async open (path, flags, mode) {
      if (typeof flags === 'object' && flags !== null) {
        mode = flags.mode
        flags = flags.flags
      }

      // TODO(jwerle): discuss fs.read instead of fsOpen
      return await this.request('fsOpen', { path, flags, mode }, { id: true })
    }

    async close (id) {
      // TODO(jwerle): discuss fd.close instead of fsClose
      return await this.request('fsClose', { id })
    }

    async read (id, size, offset = 0) {
      if (typeof size === 'object' && size !== null) {
        offset = size.offset || 0
        size = size.size
      }

      // TODO(jwerle): discuss fs.read instead of fsRead
      return await this.request('fsRead', { id, size, offset })
    }
  }
)JS";

#endif

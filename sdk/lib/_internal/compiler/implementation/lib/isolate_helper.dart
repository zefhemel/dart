// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library _isolate_helper;

import 'dart:async';
import 'dart:collection' show Queue, HashMap;
import 'dart:isolate';
import 'dart:_js_helper' show convertDartClosureToJS,
                              Null;
import 'dart:_foreign_helper' show DART_CLOSURE_TO_JS,
                                   JS,
                                   JS_CREATE_ISOLATE,
                                   JS_CURRENT_ISOLATE,
                                   JS_SET_CURRENT_ISOLATE,
                                   IsolateContext;

ReceivePort lazyPort;

class CloseToken {
  /// This token is sent from [IsolateSink]s to [IsolateStream]s to ask them to
  /// close themselves.
  const CloseToken();
}

class JsIsolateSink extends EventSink<dynamic> implements IsolateSink {
  bool _isClosed = false;
  final SendPort _port;
  JsIsolateSink.fromPort(this._port);

  void add(dynamic message) {
    _port.send(message);
  }

  void addError(errorEvent) {
    throw new UnimplementedError("addError on isolate streams");
  }

  void close() {
    if (_isClosed) return;
    add(const CloseToken());
    _isClosed = true;
  }

  bool operator==(var other) {
    return other is IsolateSink && _port == other._port;
  }

  int get hashCode => _port.hashCode + 499;
}

/**
 * Called by the compiler to support switching
 * between isolates when we get a callback from the DOM.
 */
_callInIsolate(_IsolateContext isolate, Function function) {
  var result = isolate.eval(function);
  _globalState.topEventLoop.run();
  return result;
}

/**
 * Called by the compiler to fetch the current isolate context.
 */
_IsolateContext _currentIsolate() => _globalState.currentContext;

/**
 * Wrapper that takes the dart entry point and runs it within an isolate. The
 * dart2js compiler will inject a call of the form
 * [: startRootIsolate(main); :] when it determines that this wrapping
 * is needed. For single-isolate applications (e.g. hello world), this
 * call is not emitted.
 */
void startRootIsolate(entry) {
  _globalState = new _Manager();

  // Don't start the main loop again, if we are in a worker.
  if (_globalState.isWorker) return;
  final rootContext = new _IsolateContext();
  _globalState.rootContext = rootContext;

  // BUG(5151491): Setting currentContext should not be necessary, but
  // because closures passed to the DOM as event handlers do not bind their
  // isolate automatically we try to give them a reasonable context to live in
  // by having a "default" isolate (the first one created).
  _globalState.currentContext = rootContext;

  rootContext.eval(entry);
  _globalState.topEventLoop.run();
}

/********************************************************
  Inserted from lib/isolate/dart2js/isolateimpl.dart
 ********************************************************/

/**
 * Concepts used here:
 *
 * "manager" - A manager contains one or more isolates, schedules their
 * execution, and performs other plumbing on their behalf.  The isolate
 * present at the creation of the manager is designated as its "root isolate".
 * A manager may, for example, be implemented on a web Worker.
 *
 * [_Manager] - State present within a manager (exactly once, as a global).
 *
 * [_ManagerStub] - A handle held within one manager that allows interaction
 * with another manager.  A target manager may be addressed by zero or more
 * [_ManagerStub]s.
 *
 */

/**
 * A native object that is shared across isolates. This object is visible to all
 * isolates running under the same manager (either UI or background web worker).
 *
 * This is code that is intended to 'escape' the isolate boundaries in order to
 * implement the semantics of isolates in JavaScript. Without this we would have
 * been forced to implement more code (including the top-level event loop) in
 * JavaScript itself.
 */
// TODO(eub, sigmund): move the "manager" to be entirely in JS.
// Running any Dart code outside the context of an isolate gives it
// the chance to break the isolate abstraction.
_Manager get _globalState => JS("_Manager", r"$globalState");

set _globalState(_Manager val) {
  JS("void", r"$globalState = #", val);
}

/** State associated with the current manager. See [globalState]. */
// TODO(sigmund): split in multiple classes: global, thread, main-worker states?
class _Manager {

  /** Next available isolate id within this [_Manager]. */
  int nextIsolateId = 0;

  /** id assigned to this [_Manager]. */
  int currentManagerId = 0;

  /**
   * Next available manager id. Only used by the main manager to assign a unique
   * id to each manager created by it.
   */
  int nextManagerId = 1;

  /** Context for the currently running [Isolate]. */
  _IsolateContext currentContext = null;

  /** Context for the root [Isolate] that first run in this [_Manager]. */
  _IsolateContext rootContext = null;

  /** The top-level event loop. */
  _EventLoop topEventLoop;

  /** Whether this program is running from the command line. */
  bool fromCommandLine;

  /** Whether this [_Manager] is running as a web worker. */
  bool isWorker;

  /** Whether we support spawning web workers. */
  bool supportsWorkers;

  /**
   * Whether to use web workers when implementing isolates. Set to false for
   * debugging/testing.
   */
  bool get useWorkers => supportsWorkers;

  /**
   * Whether to use the web-worker JSON-based message serialization protocol. By
   * default this is only used with web workers. For debugging, you can force
   * using this protocol by changing this field value to [true].
   */
  bool get needSerialization => useWorkers;

  /**
   * Registry of isolates. Isolates must be registered if, and only if, receive
   * ports are alive.  Normally no open receive-ports means that the isolate is
   * dead, but DOM callbacks could resurrect it.
   */
  Map<int, _IsolateContext> isolates;

  /** Reference to the main [_Manager].  Null in the main [_Manager] itself. */
  _ManagerStub mainManager;

  /** Registry of active [_ManagerStub]s.  Only used in the main [_Manager]. */
  Map<int, _ManagerStub> managers;

  _Manager() {
    _nativeDetectEnvironment();
    topEventLoop = new _EventLoop();
    isolates = new Map<int, _IsolateContext>();
    managers = new Map<int, _ManagerStub>();
    if (isWorker) {  // "if we are not the main manager ourself" is the intent.
      mainManager = new _MainManagerStub();
      _nativeInitWorkerMessageHandler();
    }
  }

  void _nativeDetectEnvironment() {
    bool isWindowDefined = globalWindow != null;
    bool isWorkerDefined = globalWorker != null;

    isWorker = !isWindowDefined && globalPostMessageDefined;
    supportsWorkers = isWorker
       || (isWorkerDefined && IsolateNatives.thisScript != null);
    fromCommandLine = !isWindowDefined && !isWorker;
  }

  void _nativeInitWorkerMessageHandler() {
    var function = JS('',
                      "function (e) { #(#, e); }",
                      DART_CLOSURE_TO_JS(IsolateNatives._processWorkerMessage),
                      mainManager);
    JS("void", r"#.onmessage = #", globalThis, function);
    // We define dartPrint so that the implementation of the Dart
    // print method knows what to call.
    // TODO(ngeoffray): Should we forward to the main isolate? What if
    // it exited?
    JS('void', r'#.dartPrint = function (object) {}', globalThis);
  }


  /**
   * Close the worker running this code if all isolates are done and
   * there is no active timer.
   */
  void maybeCloseWorker() {
    if (isWorker
        && isolates.isEmpty
        && topEventLoop.activeTimerCount == 0) {
      mainManager.postMessage(_serializeMessage({'command': 'close'}));
    }
  }
}

/** Context information tracked for each isolate. */
class _IsolateContext implements IsolateContext {
  /** Current isolate id. */
  int id;

  /** Registry of receive ports currently active on this isolate. */
  Map<int, ReceivePort> ports;

  /** Holds isolate globals (statics and top-level properties). */
  var isolateStatics; // native object containing all globals of an isolate.

  _IsolateContext() {
    id = _globalState.nextIsolateId++;
    ports = new Map<int, ReceivePort>();
    isolateStatics = JS_CREATE_ISOLATE();
  }

  /**
   * Run [code] in the context of the isolate represented by [this].
   */
  dynamic eval(Function code) {
    var old = _globalState.currentContext;
    _globalState.currentContext = this;
    this._setGlobals();
    var result = null;
    try {
      result = code();
    } finally {
      _globalState.currentContext = old;
      if (old != null) old._setGlobals();
    }
    return result;
  }

  void _setGlobals() {
    JS_SET_CURRENT_ISOLATE(isolateStatics);
  }

  /** Lookup a port registered for this isolate. */
  ReceivePort lookup(int portId) => ports[portId];

  /** Register a port on this isolate. */
  void register(int portId, ReceivePort port)  {
    if (ports.containsKey(portId)) {
      throw new Exception("Registry: ports must be registered only once.");
    }
    ports[portId] = port;
    _globalState.isolates[id] = this; // indicate this isolate is active
  }

  /** Unregister a port on this isolate. */
  void unregister(int portId) {
    ports.remove(portId);
    if (ports.isEmpty) {
      _globalState.isolates.remove(id); // indicate this isolate is not active
    }
  }
}

/** Represent the event loop on a javascript thread (DOM or worker). */
class _EventLoop {
  final Queue<_IsolateEvent> events = new Queue<_IsolateEvent>();
  int activeTimerCount = 0;

  _EventLoop();

  void enqueue(isolate, fn, msg) {
    events.addLast(new _IsolateEvent(isolate, fn, msg));
  }

  _IsolateEvent dequeue() {
    if (events.isEmpty) return null;
    return events.removeFirst();
  }

  void checkOpenReceivePortsFromCommandLine() {
    if (_globalState.rootContext != null
        && _globalState.isolates.containsKey(_globalState.rootContext.id)
        && _globalState.fromCommandLine
        && _globalState.rootContext.ports.isEmpty) {
      // We want to reach here only on the main [_Manager] and only
      // on the command-line.  In the browser the isolate might
      // still be alive due to DOM callbacks, but the presumption is
      // that on the command-line, no future events can be injected
      // into the event queue once it's empty.  Node has setTimeout
      // so this presumption is incorrect there.  We think(?) that
      // in d8 this assumption is valid.
      throw new Exception("Program exited with open ReceivePorts.");
    }
  }

  /** Process a single event, if any. */
  bool runIteration() {
    final event = dequeue();
    if (event == null) {
      checkOpenReceivePortsFromCommandLine();
      _globalState.maybeCloseWorker();
      return false;
    }
    event.process();
    return true;
  }

  /**
   * Runs multiple iterations of the run-loop. If possible, each iteration is
   * run asynchronously.
   */
  void _runHelper() {
    if (globalWindow != null) {
      // Run each iteration from the browser's top event loop.
      void next() {
        if (!runIteration()) return;
        Timer.run(next);
      }
      next();
    } else {
      // Run synchronously until no more iterations are available.
      while (runIteration()) {}
    }
  }

  /**
   * Call [_runHelper] but ensure that worker exceptions are propragated.
   */
  void run() {
    if (!_globalState.isWorker) {
      _runHelper();
    } else {
      try {
        _runHelper();
      } catch (e, trace) {
        _globalState.mainManager.postMessage(_serializeMessage(
            {'command': 'error', 'msg': '$e\n$trace' }));
      }
    }
  }
}

/** An event in the top-level event queue. */
class _IsolateEvent {
  _IsolateContext isolate;
  Function fn;
  String message;

  _IsolateEvent(this.isolate, this.fn, this.message);

  void process() {
    isolate.eval(fn);
  }
}

/** An interface for a stub used to interact with a manager. */
abstract class _ManagerStub {
  get id;
  void set id(int i);
  void set onmessage(Function f);
  void postMessage(msg);
  void terminate();
}

/** A stub for interacting with the main manager. */
class _MainManagerStub implements _ManagerStub {
  get id => 0;
  void set id(int i) { throw new UnimplementedError(); }
  void set onmessage(f) {
    throw new Exception("onmessage should not be set on MainManagerStub");
  }
  void postMessage(msg) {
    JS("void", r"#.postMessage(#)", globalThis, msg);
  }
  void terminate() {}  // Nothing useful to do here.
}

/**
 * A stub for interacting with a manager built on a web worker. This
 * definition uses a 'hidden' type (* prefix on the native name) to
 * enforce that the type is defined dynamically only when web workers
 * are actually available.
 */
// @Native("*Worker");
class _WorkerStub implements _ManagerStub {
  get id => JS("", "#.id", this);
  void set id(i) { JS("void", "#.id = #", this, i); }
  void set onmessage(f) { JS("void", "#.onmessage = #", this, f); }
  void postMessage(msg) { JS("void", "#.postMessage(#)", this, msg); }
  void terminate() { JS("void", "#.terminate()", this); }
}

const String _SPAWNED_SIGNAL = "spawned";

var globalThis = IsolateNatives.computeGlobalThis();
var globalWindow = JS('', "#.window", globalThis);
var globalWorker = JS('', "#.Worker", globalThis);
bool globalPostMessageDefined =
    JS('', "#.postMessage !== (void 0)", globalThis);

class IsolateNatives {

  static String thisScript = computeThisScript();

  /**
   * The src url for the script tag that loaded this code. Used to create
   * JavaScript workers.
   */
  static String computeThisScript() {
    var currentScript = JS('', r'$.$currentScript');
    if (currentScript != null) {
      return JS('String', 'String(#.src)', currentScript);
    }

    // TODO(ahe): The following is for supporting command-line engines
    // such as d8 and jsshell. We should move this code to a helper
    // library that is only loaded when testing on those engines.

    var stack = JS('String|Null', 'new Error().stack');
    if (stack == null) {
      // According to Internet Explorer documentation, the stack
      // property is not set until the exception is thrown. The stack
      // property was not provided until IE10.
      stack = JS('String',
                 '(function() {'
                 'try { throw new Error() } catch(e) { return e.stack }'
                 '})()');
    }
    var pattern, matches;

    // This pattern matches V8, Chrome, and Internet Explorer stack
    // traces that look like this:
    // Error
    //     at methodName (URI:LINE:COLUMN)
    pattern = JS('',
                 r'new RegExp("^ *at [^(]*\\((.*):[0-9]*:[0-9]*\\)$", "m")');


    matches = JS('=List|Null', '#.match(#)', stack, pattern);
    if (matches != null) return JS('String', '#[1]', matches);

    // This pattern matches Firefox stack traces that look like this:
    // methodName@URI:LINE
    pattern = JS('', r'new RegExp("^[^@]*@(.*):[0-9]*$", "m")');

    matches = JS('=List|Null', '#.match(#)', stack, pattern);
    if (matches != null) return JS('String', '#[1]', matches);

    throw new UnsupportedError('Cannot extract URI from "$stack"');
  }

  static computeGlobalThis() => JS('', 'function() { return this; }()');

  /** Starts a new worker with the given URL. */
  static _WorkerStub _newWorker(url) => JS("_WorkerStub", r"new Worker(#)", url);

  /**
   * Assume that [e] is a browser message event and extract its message data.
   * We don't import the dom explicitly so, when workers are disabled, this
   * library can also run on top of nodejs.
   */
  static _getEventData(e) => JS("", "#.data", e);

  /**
   * Process messages on a worker, either to control the worker instance or to
   * pass messages along to the isolate running in the worker.
   */
  static void _processWorkerMessage(sender, e) {
    var msg = _deserializeMessage(_getEventData(e));
    switch (msg['command']) {
      case 'start':
        _globalState.currentManagerId = msg['id'];
        Function entryPoint = _getJSFunctionFromName(msg['functionName']);
        var replyTo = _deserializeMessage(msg['replyTo']);
        var context = new _IsolateContext();
        _globalState.topEventLoop.enqueue(context, () {
          _startIsolate(entryPoint, replyTo);
        }, 'worker-start');
        // Make sure we always have a current context in this worker.
        // TODO(7907): This is currently needed because we're using
        // Timers to implement Futures, and this isolate library
        // implementation uses Futures. We should either stop using
        // Futures in this library, or re-adapt if Futures get a
        // different implementation.
        _globalState.currentContext = context;
        _globalState.topEventLoop.run();
        break;
      case 'spawn-worker':
        _spawnWorker(msg['functionName'], msg['uri'], msg['replyPort']);
        break;
      case 'message':
        SendPort port = msg['port'];
        // If the port has been closed, we ignore the message.
        if (port != null) {
          msg['port'].send(msg['msg'], msg['replyTo']);
        }
        _globalState.topEventLoop.run();
        break;
      case 'close':
        _log("Closing Worker");
        _globalState.managers.remove(sender.id);
        sender.terminate();
        _globalState.topEventLoop.run();
        break;
      case 'log':
        _log(msg['msg']);
        break;
      case 'print':
        if (_globalState.isWorker) {
          _globalState.mainManager.postMessage(
              _serializeMessage({'command': 'print', 'msg': msg}));
        } else {
          print(msg['msg']);
        }
        break;
      case 'error':
        throw msg['msg'];
    }
  }

  /** Log a message, forwarding to the main [_Manager] if appropriate. */
  static _log(msg) {
    if (_globalState.isWorker) {
      _globalState.mainManager.postMessage(
          _serializeMessage({'command': 'log', 'msg': msg }));
    } else {
      try {
        _consoleLog(msg);
      } catch (e, trace) {
        throw new Exception(trace);
      }
    }
  }

  static void _consoleLog(msg) {
    JS("void", r"#.console.log(#)", globalThis, msg);
  }

  /** Find a constructor given its name. */
  static dynamic _getJSConstructorFromName(String factoryName) {
    return JS("", r"$[#]", factoryName);
  }

  static dynamic _getJSFunctionFromName(String functionName) {
    return JS("", r"$[#]", functionName);
  }

  /**
   * Get a string name for the function, if possible.  The result for
   * anonymous functions is browser-dependent -- it may be "" or "anonymous"
   * but you should probably not count on this.
   */
  static String _getJSFunctionName(Function f) {
    return JS("String|Null", r"(#['$name'] || #)", f, null);
  }

  /** Create a new JavaScript object instance given its constructor. */
  static dynamic _allocate(var ctor) {
    return JS("", "new #()", ctor);
  }

  static SendPort spawnFunction(void topLevelFunction()) {
    final name = _getJSFunctionName(topLevelFunction);
    if (name == null) {
      throw new UnsupportedError(
          "only top-level functions can be spawned.");
    }
    return spawn(name, null, false);
  }

  static SendPort spawnDomFunction(void topLevelFunction()) {
    final name = _getJSFunctionName(topLevelFunction);
    if (name == null) {
      throw new UnsupportedError(
          "only top-level functions can be spawned.");
    }
    return spawn(name, null, true);
  }

  // TODO(sigmund): clean up above, after we make the new API the default:

  static spawn(String functionName, String uri, bool isLight) {
    Completer<SendPort> completer = new Completer<SendPort>();
    ReceivePort port = new ReceivePort();
    port.receive((msg, SendPort replyPort) {
      port.close();
      assert(msg == _SPAWNED_SIGNAL);
      completer.complete(replyPort);
    });

    SendPort signalReply = port.toSendPort();

    if (_globalState.useWorkers && !isLight) {
      _startWorker(functionName, uri, signalReply);
    } else {
      _startNonWorker(functionName, uri, signalReply);
    }
    return new _BufferingSendPort(
        _globalState.currentContext.id, completer.future);
  }

  static SendPort _startWorker(
      String functionName, String uri, SendPort replyPort) {
    if (_globalState.isWorker) {
      _globalState.mainManager.postMessage(_serializeMessage({
          'command': 'spawn-worker',
          'functionName': functionName,
          'uri': uri,
          'replyPort': replyPort}));
    } else {
      _spawnWorker(functionName, uri, replyPort);
    }
  }

  static SendPort _startNonWorker(
      String functionName, String uri, SendPort replyPort) {
    // TODO(eub): support IE9 using an iframe -- Dart issue 1702.
    if (uri != null) throw new UnsupportedError(
            "Currently spawnUri is not supported without web workers.");
    _globalState.topEventLoop.enqueue(new _IsolateContext(), () {
      final func = _getJSFunctionFromName(functionName);
      _startIsolate(func, replyPort);
    }, 'nonworker start');
  }

  static void _startIsolate(Function topLevel, SendPort replyTo) {
    lazyPort = new ReceivePort();
    replyTo.send(_SPAWNED_SIGNAL, port.toSendPort());
    topLevel();
  }

  /**
   * Spawns an isolate in a worker. [factoryName] is the Javascript constructor
   * name for the isolate entry point class.
   */
  static void _spawnWorker(functionName, uri, replyPort) {
    if (functionName == null) functionName = 'main';
    if (uri == null) uri = thisScript;
    final worker = _newWorker(uri);
    worker.onmessage = JS('',
                          'function(e) { #(#, e); }',
                          DART_CLOSURE_TO_JS(_processWorkerMessage),
                          worker);
    var workerId = _globalState.nextManagerId++;
    // We also store the id on the worker itself so that we can unregister it.
    worker.id = workerId;
    _globalState.managers[workerId] = worker;
    worker.postMessage(_serializeMessage({
      'command': 'start',
      'id': workerId,
      // Note: we serialize replyPort twice because the child worker needs to
      // first deserialize the worker id, before it can correctly deserialize
      // the port (port deserialization is sensitive to what is the current
      // workerId).
      'replyTo': _serializeMessage(replyPort),
      'functionName': functionName }));
  }
}

/********************************************************
  Inserted from lib/isolate/dart2js/ports.dart
 ********************************************************/

/** Common functionality to all send ports. */
class _BaseSendPort implements SendPort {
  /** Id for the destination isolate. */
  final int _isolateId;

  const _BaseSendPort(this._isolateId);

  void _checkReplyTo(SendPort replyTo) {
    if (replyTo != null
        && replyTo is! _NativeJsSendPort
        && replyTo is! _WorkerSendPort
        && replyTo is! _BufferingSendPort) {
      throw new Exception("SendPort.send: Illegal replyTo port type");
    }
  }

  Future call(var message) {
    final completer = new Completer();
    final port = new ReceivePortImpl();
    send(message, port.toSendPort());
    port.receive((value, ignoreReplyTo) {
      port.close();
      if (value is Exception) {
        completer.completeError(value);
      } else {
        completer.complete(value);
      }
    });
    return completer.future;
  }

  void send(var message, [SendPort replyTo]);
  bool operator ==(var other);
  int get hashCode;
}

/** A send port that delivers messages in-memory via native JavaScript calls. */
class _NativeJsSendPort extends _BaseSendPort implements SendPort {
  final ReceivePortImpl _receivePort;

  const _NativeJsSendPort(this._receivePort, int isolateId) : super(isolateId);

  void send(var message, [SendPort replyTo = null]) {
    _waitForPendingPorts([message, replyTo], () {
      _checkReplyTo(replyTo);
      // Check that the isolate still runs and the port is still open
      final isolate = _globalState.isolates[_isolateId];
      if (isolate == null) return;
      if (_receivePort._callback == null) return;

      // We force serialization/deserialization as a simple way to ensure
      // isolate communication restrictions are respected between isolates that
      // live in the same worker. [_NativeJsSendPort] delivers both messages
      // from the same worker and messages from other workers. In particular,
      // messages sent from a worker via a [_WorkerSendPort] are received at
      // [_processWorkerMessage] and forwarded to a native port. In such cases,
      // here we'll see [_globalState.currentContext == null].
      final shouldSerialize = _globalState.currentContext != null
          && _globalState.currentContext.id != _isolateId;
      var msg = message;
      var reply = replyTo;
      if (shouldSerialize) {
        msg = _serializeMessage(msg);
        reply = _serializeMessage(reply);
      }
      _globalState.topEventLoop.enqueue(isolate, () {
        if (_receivePort._callback != null) {
          if (shouldSerialize) {
            msg = _deserializeMessage(msg);
            reply = _deserializeMessage(reply);
          }
          _receivePort._callback(msg, reply);
        }
      }, 'receive $message');
    });
  }

  bool operator ==(var other) => (other is _NativeJsSendPort) &&
      (_receivePort == other._receivePort);

  int get hashCode => _receivePort._id;
}

/** A send port that delivers messages via worker.postMessage. */
// TODO(eub): abstract this for iframes.
class _WorkerSendPort extends _BaseSendPort implements SendPort {
  final int _workerId;
  final int _receivePortId;

  const _WorkerSendPort(this._workerId, int isolateId, this._receivePortId)
      : super(isolateId);

  void send(var message, [SendPort replyTo = null]) {
    _waitForPendingPorts([message, replyTo], () {
      _checkReplyTo(replyTo);
      final workerMessage = _serializeMessage({
          'command': 'message',
          'port': this,
          'msg': message,
          'replyTo': replyTo});

      if (_globalState.isWorker) {
        // Communication from one worker to another go through the
        // main worker.
        _globalState.mainManager.postMessage(workerMessage);
      } else {
        // Deliver the message only if the worker is still alive.
        _ManagerStub manager = _globalState.managers[_workerId];
        if (manager != null) {
          manager.postMessage(workerMessage);
        }
      }
    });
  }

  bool operator ==(var other) {
    return (other is _WorkerSendPort) &&
        (_workerId == other._workerId) &&
        (_isolateId == other._isolateId) &&
        (_receivePortId == other._receivePortId);
  }

  int get hashCode {
    // TODO(sigmund): use a standard hash when we get one available in corelib.
    return (_workerId << 16) ^ (_isolateId << 8) ^ _receivePortId;
  }
}

/** A port that buffers messages until an underlying port gets resolved. */
class _BufferingSendPort extends _BaseSendPort implements SendPort {
  /** Internal counter to assign unique ids to each port. */
  static int _idCount = 0;

  /** For implementing equals and hashcode. */
  final int _id;

  /** Underlying port, when resolved. */
  SendPort _port;

  /**
   * Future.sync the underlying port, so that we can detect when this port can be
   * sent on messages.
   */
  Future<SendPort> _futurePort;

  /** Pending messages (and reply ports). */
  List pending;

  _BufferingSendPort(isolateId, this._futurePort)
      : super(isolateId), _id = _idCount, pending = [] {
    _idCount++;
    _futurePort.then((p) {
      _port = p;
      for (final item in pending) {
        p.send(item['message'], item['replyTo']);
      }
      pending = null;
    });
  }

  _BufferingSendPort.fromPort(isolateId, this._port)
      : super(isolateId), _id = _idCount {
    _idCount++;
  }

  void send(var message, [SendPort replyTo]) {
    if (_port != null) {
      _port.send(message, replyTo);
    } else {
      pending.add({'message': message, 'replyTo': replyTo});
    }
  }

  bool operator ==(var other) =>
      other is _BufferingSendPort && _id == other._id;
  int get hashCode => _id;
}

/** Implementation of a multi-use [ReceivePort] on top of JavaScript. */
class ReceivePortImpl implements ReceivePort {
  int _id;
  Function _callback;
  static int _nextFreeId = 1;

  ReceivePortImpl()
      : _id = _nextFreeId++ {
    _globalState.currentContext.register(_id, this);
  }

  void receive(void onMessage(var message, SendPort replyTo)) {
    _callback = onMessage;
  }

  void close() {
    _callback = null;
    _globalState.currentContext.unregister(_id);
  }

  SendPort toSendPort() {
    return new _NativeJsSendPort(this, _globalState.currentContext.id);
  }
}

/** Wait until all ports in a message are resolved. */
_waitForPendingPorts(var message, void callback()) {
  final finder = new _PendingSendPortFinder();
  finder.traverse(message);
  Future.wait(finder.ports).then((_) => callback());
}


/** Visitor that finds all unresolved [SendPort]s in a message. */
class _PendingSendPortFinder extends _MessageTraverser {
  List<Future<SendPort>> ports;
  _PendingSendPortFinder() : super(), ports = [] {
    _visited = new _JsVisitedMap();
  }

  visitPrimitive(x) {}

  visitList(List list) {
    final seen = _visited[list];
    if (seen != null) return;
    _visited[list] = true;
    // TODO(sigmund): replace with the following: (bug #1660)
    // list.forEach(_dispatch);
    list.forEach((e) => _dispatch(e));
  }

  visitMap(Map map) {
    final seen = _visited[map];
    if (seen != null) return;

    _visited[map] = true;
    // TODO(sigmund): replace with the following: (bug #1660)
    // map.values.forEach(_dispatch);
    map.values.forEach((e) => _dispatch(e));
  }

  visitSendPort(SendPort port) {
    if (port is _BufferingSendPort && port._port == null) {
      ports.add(port._futurePort);
    }
  }

  visitIsolateSink(IsolateSink sink) {
    visitSendPort(sink._port);
  }

  visitCloseToken(CloseToken token) {
    // Do nothing.
  }
}

/********************************************************
  Inserted from lib/isolate/dart2js/messages.dart
 ********************************************************/

// Defines message visitors, serialization, and deserialization.

/** Serialize [message] (or simulate serialization). */
_serializeMessage(message) {
  if (_globalState.needSerialization) {
    return new _JsSerializer().traverse(message);
  } else {
    return new _JsCopier().traverse(message);
  }
}

/** Deserialize [message] (or simulate deserialization). */
_deserializeMessage(message) {
  if (_globalState.needSerialization) {
    return new _JsDeserializer().deserialize(message);
  } else {
    // Nothing more to do.
    return message;
  }
}

class _JsSerializer extends _Serializer {

  _JsSerializer() : super() { _visited = new _JsVisitedMap(); }

  visitSendPort(SendPort x) {
    if (x is _NativeJsSendPort) return visitNativeJsSendPort(x);
    if (x is _WorkerSendPort) return visitWorkerSendPort(x);
    if (x is _BufferingSendPort) return visitBufferingSendPort(x);
    throw "Illegal underlying port $x";
  }

  visitNativeJsSendPort(_NativeJsSendPort port) {
    return ['sendport', _globalState.currentManagerId,
        port._isolateId, port._receivePort._id];
  }

  visitWorkerSendPort(_WorkerSendPort port) {
    return ['sendport', port._workerId, port._isolateId, port._receivePortId];
  }

  visitBufferingSendPort(_BufferingSendPort port) {
    if (port._port != null) {
      return visitSendPort(port._port);
    } else {
      // TODO(floitsch): Use real exception (which one?).
      throw
          "internal error: must call _waitForPendingPorts to ensure all"
          " ports are resolved at this point.";
    }
  }

  visitIsolateSink(IsolateSink sink) {
    SendPort port = sink._port;
    bool isClosed = sink._isClosed;
    return ['isolateSink', visitSendPort(port), isClosed];
  }

  visitCloseToken(CloseToken token) {
    return ['closeToken'];
  }
}


class _JsCopier extends _Copier {

  _JsCopier() : super() { _visited = new _JsVisitedMap(); }

  visitSendPort(SendPort x) {
    if (x is _NativeJsSendPort) return visitNativeJsSendPort(x);
    if (x is _WorkerSendPort) return visitWorkerSendPort(x);
    if (x is _BufferingSendPort) return visitBufferingSendPort(x);
    throw "Illegal underlying port $p";
  }

  SendPort visitNativeJsSendPort(_NativeJsSendPort port) {
    return new _NativeJsSendPort(port._receivePort, port._isolateId);
  }

  SendPort visitWorkerSendPort(_WorkerSendPort port) {
    return new _WorkerSendPort(
        port._workerId, port._isolateId, port._receivePortId);
  }

  SendPort visitBufferingSendPort(_BufferingSendPort port) {
    if (port._port != null) {
      return visitSendPort(port._port);
    } else {
      // TODO(floitsch): Use real exception (which one?).
      throw
          "internal error: must call _waitForPendingPorts to ensure all"
          " ports are resolved at this point.";
    }
  }

  IsolateSink visitIsolateSink(IsolateSink sink) {
    SendPort port = sink._port;
    bool isClosed = sink._isClosed;
    IsolateSink result = new JsIsolateSink.fromPort(visitSendPort(port));
    result._isClosed = isClosed;
    return result;
  }

  CloseToken visitCloseToken(CloseToken token) {
    return token;  // Can be shared.
  }
}

class _JsDeserializer extends _Deserializer {

  SendPort deserializeSendPort(List list) {
    int managerId = list[1];
    int isolateId = list[2];
    int receivePortId = list[3];
    // If two isolates are in the same manager, we use NativeJsSendPorts to
    // deliver messages directly without using postMessage.
    if (managerId == _globalState.currentManagerId) {
      var isolate = _globalState.isolates[isolateId];
      if (isolate == null) return null; // Isolate has been closed.
      var receivePort = isolate.lookup(receivePortId);
      if (receivePort == null) return null; // Port has been closed.
      return new _NativeJsSendPort(receivePort, isolateId);
    } else {
      return new _WorkerSendPort(managerId, isolateId, receivePortId);
    }
  }

  IsolateSink deserializeIsolateSink(List list) {
    SendPort port = deserializeSendPort(list[1]);
    bool isClosed = list[2];
    IsolateSink result = new JsIsolateSink.fromPort(port);
    result._isClosed = isClosed;
    return result;
  }

  CloseToken deserializeCloseToken(List list) {
    return const CloseToken();
  }
}

class _JsVisitedMap implements _MessageTraverserVisitedMap {
  List tagged;

  /** Retrieves any information stored in the native object [object]. */
  operator[](var object) {
    return _getAttachedInfo(object);
  }

  /** Injects some information into the native [object]. */
  void operator[]=(var object, var info) {
    tagged.add(object);
    _setAttachedInfo(object, info);
  }

  /** Get ready to rumble. */
  void reset() {
    assert(tagged == null);
    tagged = new List();
  }

  /** Remove all information injected in the native objects. */
  void cleanup() {
    for (int i = 0, length = tagged.length; i < length; i++) {
      _clearAttachedInfo(tagged[i]);
    }
    tagged = null;
  }

  void _clearAttachedInfo(var o) {
    JS("void", "#['__MessageTraverser__attached_info__'] = #", o, null);
  }

  void _setAttachedInfo(var o, var info) {
    JS("void", "#['__MessageTraverser__attached_info__'] = #", o, info);
  }

  _getAttachedInfo(var o) {
    return JS("", "#['__MessageTraverser__attached_info__']", o);
  }
}

// only visible for testing purposes
// TODO(sigmund): remove once we can disable privacy for testing (bug #1882)
class TestingOnly {
  static copy(x) {
    return new _JsCopier().traverse(x);
  }

  // only visible for testing purposes
  static serialize(x) {
    _Serializer serializer = new _JsSerializer();
    _Deserializer deserializer = new _JsDeserializer();
    return deserializer.deserialize(serializer.traverse(x));
  }
}

/********************************************************
  Inserted from lib/isolate/serialization.dart
 ********************************************************/

class _MessageTraverserVisitedMap {

  operator[](var object) => null;
  void operator[]=(var object, var info) { }

  void reset() { }
  void cleanup() { }

}

/** Abstract visitor for dart objects that can be sent as isolate messages. */
class _MessageTraverser {

  _MessageTraverserVisitedMap _visited;
  _MessageTraverser() : _visited = new _MessageTraverserVisitedMap();

  /** Visitor's entry point. */
  traverse(var x) {
    if (isPrimitive(x)) return visitPrimitive(x);
    _visited.reset();
    var result;
    try {
      result = _dispatch(x);
    } finally {
      _visited.cleanup();
    }
    return result;
  }

  _dispatch(var x) {
    if (isPrimitive(x)) return visitPrimitive(x);
    if (x is List) return visitList(x);
    if (x is Map) return visitMap(x);
    if (x is SendPort) return visitSendPort(x);
    if (x is SendPortSync) return visitSendPortSync(x);
    if (x is JsIsolateSink) return visitIsolateSink(x);
    if (x is CloseToken) return visitCloseToken(x);

    // Overridable fallback.
    return visitObject(x);
  }

  visitPrimitive(x);
  visitList(List x);
  visitMap(Map x);
  visitSendPort(SendPort x);
  visitSendPortSync(SendPortSync x);
  visitIsolateSink(IsolateSink x);
  visitCloseToken(CloseToken x);

  visitObject(Object x) {
    // TODO(floitsch): make this a real exception. (which one)?
    throw "Message serialization: Illegal value $x passed";
  }

  static bool isPrimitive(x) {
    return (x == null) || (x is String) || (x is num) || (x is bool);
  }
}


/** A visitor that recursively copies a message. */
class _Copier extends _MessageTraverser {

  visitPrimitive(x) => x;

  List visitList(List list) {
    List copy = _visited[list];
    if (copy != null) return copy;

    int len = list.length;

    // TODO(floitsch): we loose the generic type of the List.
    copy = new List(len);
    _visited[list] = copy;
    for (int i = 0; i < len; i++) {
      copy[i] = _dispatch(list[i]);
    }
    return copy;
  }

  Map visitMap(Map map) {
    Map copy = _visited[map];
    if (copy != null) return copy;

    // TODO(floitsch): we loose the generic type of the map.
    copy = new Map();
    _visited[map] = copy;
    map.forEach((key, val) {
      copy[_dispatch(key)] = _dispatch(val);
    });
    return copy;
  }

}

/** Visitor that serializes a message as a JSON array. */
class _Serializer extends _MessageTraverser {
  int _nextFreeRefId = 0;

  visitPrimitive(x) => x;

  visitList(List list) {
    int copyId = _visited[list];
    if (copyId != null) return ['ref', copyId];

    int id = _nextFreeRefId++;
    _visited[list] = id;
    var jsArray = _serializeList(list);
    // TODO(floitsch): we are losing the generic type.
    return ['list', id, jsArray];
  }

  visitMap(Map map) {
    int copyId = _visited[map];
    if (copyId != null) return ['ref', copyId];

    int id = _nextFreeRefId++;
    _visited[map] = id;
    var keys = _serializeList(map.keys.toList());
    var values = _serializeList(map.values.toList());
    // TODO(floitsch): we are losing the generic type.
    return ['map', id, keys, values];
  }

  _serializeList(List list) {
    int len = list.length;
    var result = new List(len);
    for (int i = 0; i < len; i++) {
      result[i] = _dispatch(list[i]);
    }
    return result;
  }
}

/** Deserializes arrays created with [_Serializer]. */
class _Deserializer {
  Map<int, dynamic> _deserialized;

  _Deserializer();

  static bool isPrimitive(x) {
    return (x == null) || (x is String) || (x is num) || (x is bool);
  }

  deserialize(x) {
    if (isPrimitive(x)) return x;
    // TODO(floitsch): this should be new HashMap<int, dynamic>()
    _deserialized = new HashMap();
    return _deserializeHelper(x);
  }

  _deserializeHelper(x) {
    if (isPrimitive(x)) return x;
    assert(x is List);
    switch (x[0]) {
      case 'ref': return _deserializeRef(x);
      case 'list': return _deserializeList(x);
      case 'map': return _deserializeMap(x);
      case 'sendport': return deserializeSendPort(x);
      case 'isolateSink': return deserializeIsolateSink(x);
      case 'closeToken': return deserializeCloseToken(x);
      default: return deserializeObject(x);
    }
  }

  _deserializeRef(List x) {
    int id = x[1];
    var result = _deserialized[id];
    assert(result != null);
    return result;
  }

  List _deserializeList(List x) {
    int id = x[1];
    // We rely on the fact that Dart-lists are directly mapped to Js-arrays.
    List dartList = x[2];
    _deserialized[id] = dartList;
    int len = dartList.length;
    for (int i = 0; i < len; i++) {
      dartList[i] = _deserializeHelper(dartList[i]);
    }
    return dartList;
  }

  Map _deserializeMap(List x) {
    Map result = new Map();
    int id = x[1];
    _deserialized[id] = result;
    List keys = x[2];
    List values = x[3];
    int len = keys.length;
    assert(len == values.length);
    for (int i = 0; i < len; i++) {
      var key = _deserializeHelper(keys[i]);
      var value = _deserializeHelper(values[i]);
      result[key] = value;
    }
    return result;
  }

  deserializeSendPort(List x);

  deserializeIsolateSink(List x);

  deserializeCloseToken(List x);

  deserializeObject(List x) {
    // TODO(floitsch): Use real exception (which one?).
    throw "Unexpected serialized object";
  }
}

class TimerImpl implements Timer {
  final bool _once;
  bool _inEventLoop = false;
  int _handle;

  TimerImpl(int milliseconds, void callback())
      : _once = true {
    if (milliseconds == 0 && (!hasTimer() || _globalState.isWorker)) {
      // This makes a dependency between the async library and the
      // event loop of the isolate library. The compiler makes sure
      // that the event loop is compiled if [Timer] is used.
      // TODO(7907): In case of web workers, we need to use the event
      // loop instead of setTimeout, to make sure the futures get executed in
      // order.
      _globalState.topEventLoop.enqueue(
          _globalState.currentContext, callback, 'timer');
      _inEventLoop = true;
    } else if (hasTimer()) {
      _globalState.topEventLoop.activeTimerCount++;
      void internalCallback() {
        callback();
        _handle = null;
        _globalState.topEventLoop.activeTimerCount--;
      }
      _handle = JS('int', '#.setTimeout(#, #)',
                   globalThis,
                   convertDartClosureToJS(internalCallback, 0),
                   milliseconds);
    } else {
      assert(milliseconds > 0);
      throw new UnsupportedError("Timer greater than 0.");
    }
  }

  TimerImpl.periodic(int milliseconds, void callback(Timer timer))
      : _once = false {
    if (hasTimer()) {
      _globalState.topEventLoop.activeTimerCount++;
      _handle = JS('int', '#.setInterval(#, #)',
                   globalThis,
                   convertDartClosureToJS(() { callback(this); }, 0),
                   milliseconds);
    } else {
      throw new UnsupportedError("Periodic timer.");
    }
  }

  void cancel() {
    if (hasTimer()) {
      if (_inEventLoop) {
        throw new UnsupportedError("Timer in event loop cannot be canceled.");
      }
      if (_handle == null) return;
      _globalState.topEventLoop.activeTimerCount--;
      if (_once) {
        JS('void', '#.clearTimeout(#)', globalThis, _handle);
      } else {
        JS('void', '#.clearInterval(#)', globalThis, _handle);
      }
      _handle = null;
    } else {
      throw new UnsupportedError("Canceling a timer.");
    }
  }
}

bool hasTimer() => JS('', '#.setTimeout', globalThis) != null;

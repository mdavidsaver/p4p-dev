
from __future__ import print_function

import logging
import warnings
import sys
_log = logging.getLogger(__name__)

try:
    from itertools import izip
except ImportError:
    izip = zip
from functools import partial
import json
import threading

try:
    from Queue import Queue, Full, Empty
except ImportError:
    from queue import Queue, Full, Empty

from . import raw
from .raw import Disconnected, RemoteError, Cancelled, Finished, LazyRepr
from ..util import _defaultWorkQueue
from ..wrapper import Value, Type
from ..rpc import WorkQueue
from .._p4p import (logLevelAll, logLevelTrace, logLevelDebug,
                    logLevelInfo, logLevelWarn, logLevelError,
                    logLevelFatal, logLevelOff)

__all__ = [
    'Context',
    'Value',
    'Type',
    'RemoteError',
    'TimeoutError',
]

if sys.version_info >= (3, 0):
    unicode = str
    TimeoutError = TimeoutError

else:
    class TimeoutError(RuntimeError):
        "Local timeout has expired"
        def __init__(self):
            RuntimeError.__init__(self, 'Timeout')


class Subscription(object):
    """An active subscription.

    Returned by `Context.monitor`.
    """

    def __init__(self, ctxt, name, cb, notify_disconnect=False, queue=None, subscription_arg=False):
        # const after ctor
        if subscription_arg:
            _cb = cb
            cb = lambda val:_cb(val, self)

        self.name, self._cb = name, cb
        self._notify_disconnect = notify_disconnect
        self._Q = queue or ctxt._Q or _defaultWorkQueue()
        self._lock = threading.Lock()
        self._evt = threading.Event()
        # guarded by self_lock
        self._S = None
        self._pending = []
        self._paused = False

    def __repr__(self):
        return 'Subscription("%s", paused=%s, #pending=%s)'%(self.name, self._paused, len(self._pending))
    __str__ = __repr__

    def close(self):
        """Close subscription.
        """
        if self._S is not None:
            _log.debug("Subscription close for %s", self.name)
            # we lose any other queued events
            self._pending = []
            self._paused = False
            # after .close() self._event should never be called
            self._S.close()
            # wait for Cancelled to be delivered
            self._evt.wait()
            self._S = None

    def __enter__(self):
        return self

    def __exit__(self, A, B, C):
        self.close()

    @property
    def done(self):
        'Has all data for this subscription been received?'
        with self._lock:
            return self._S is None or self._S.done()

    @property
    def paused(self):
        with self._lock:
            return self._paused

    def pause(self):
        """Pause the subscription.

        Since 3.1.0

        Once called, stop delivering callbacks until `resume()` is called.

        Best used in combination with pvRequest options 'pipeline=true' and 'queueSize=<#>',
        and "subscription_arg=True". ::

            TQ = p4p.util.TimerQueue().start()
            def cb(value, S):
                if ... detect over-rate:
                    S.pause()
                    TQ.push(S.resume, delay=1.0)
                .print(value)
            S = ctxt.monitor("pv:name", cb, request='record[pipeline=true,queueSize=2]', subscription_arg=True)
        """
        with self._lock:
            _log.debug("pause %s", self.name)
            self._paused = True

    def resume(self):
        """Reverse the effects of `pause()`.

        Since 3.1.0
        """
        with self._lock:
            _log.debug("resume %s %s %s", self.name, self._paused, len(self._pending))
            if self._paused:
                self._paused = False
                if len(self._pending):
                    self._Q.push(self._handle)

    def _event(self, E):
        "Adding a new event to the queue"
        with self._lock:
            assert self._S is not None, self._S

            first = len(self._pending)==0 and not self._paused
            self._pending.append(E)

            if first:
                _log.debug('Subscription wakeup for %s with %s', self.name, LazyRepr(E))
                self._Q.push(self._handle)

            else:
                _log.debug('Subscription queue for %s with %s', self.name, LazyRepr(E))

    def _handle(self):
        try:
            cb = None
            with self._lock:

                if self._paused:
                    return
                try:
                    E = self._pending.pop(0)
                except IndexError:
                    return # nothing to do there

                if isinstance(E, Cancelled):
                    self._evt.set()
                    return

                elif isinstance(E, (Disconnected, RemoteError)):
                    _log.debug('Subscription notify for %s with %s', self.name, E)
                    if self._notify_disconnect:
                        cb = self._cb
                    elif isinstance(E, RemoteError):
                        _log.error("Subscription Error %s", E)

                elif self._S is not None and E is None: # FIFO not empty
                    for n in range(4):
                        E = self._S.pop()
                        if E is None:
                            break

                        self._lock.release()
                        try:
                            self._cb(E)
                        finally:
                            self._lock.acquire()

                        if self._paused:
                            break

                    if E is not None:
                        # removed 4 elements without emptying queue
                        # re-schedule to mux with others
                        self._pending.insert(0, None)

                if self._S.done:
                    _log.debug('Subscription complete %s', self.name)
                    self._S.close()
                    self._S = None
                    if self._notify_disconnect:
                        cb, E = self._cb, Finished()

                elif self._paused:
                    _log.debug("Subscription is paused %s", self.name)

                elif self._pending:
                    _log.debug("Subscription resched for %s", self.name)
                    self._Q.push(self._handle)

                else:
                    _log.debug("Subscription idles %s", self.name)
            # re-lock
            if cb is not None:
                cb(E)

        except:
            _log.exception("Error processing Subscription event: %s", LazyRepr(E))
            if self._S is not None:
                self._S.close()
            self._S = None
            # no point in re-raising into shared thread pool worker


class Context(raw.Context):

    """Context(provider, conf=None, useenv=True)

    :param str provider: A Provider name.  Try "pva" or run :py:meth:`Context.providers` for a complete list.
    :param dict conf: Configuration to pass to provider.  Depends on provider selected.
    :param bool useenv: Allow the provider to use configuration from the process environment.
    :param int workers: Size of thread pool in which monitor callbacks are run.  Default is 4
    :param int maxsize: Size of internal work queue used for monitor callbacks.  Default is unlimited
    :param dict nt: Controls :ref:`unwrap`.  None uses defaults.  Set False to disable
    :param dict unwrap: Legacy :ref:`unwrap`.
    :param WorkQueue queue: A work queue through which monitor callbacks are dispatched.

    The methods of this Context will block the calling thread until completion or timeout

    The meaning, and allowed keys, of the configuration dictionary depend on the provider.
    conf= will override values taken from the process environment.  Pass useenv=False to
    ensure that environment variables are completely ignored.

    The "pva" provider understands the following keys:

    * EPICS_PVA_ADDR_LIST
    * EPICS_PVA_AUTO_ADDR_LIST
    * EPICS_PVA_SERVER_PORT
    * EPICS_PVA_BROADCAST_PORT
    """
    Value = Value

    name = ''
    "Provider name string"

    def __init__(self, provider, conf=None, useenv=True, nt=None, unwrap=None,
                 maxsize=0, queue=None):
        self._channel_lock = threading.Lock()

        super(Context, self).__init__(provider, conf=conf, useenv=useenv, nt=nt, unwrap=unwrap)

        # lazy start threaded WorkQueue
        self._Q = self._T = None

        self._Q = queue

    def _channel(self, name):
        with self._channel_lock:
            return super(Context, self)._channel(name)

    def disconnect(self, *args, **kws):
        with self._channel_lock:
            super(Context, self).disconnect(*args, **kws)

    def _queue(self):
        if self._Q is None:
            Q = WorkQueue(maxsize=self._Qmax)
            Ts = []
            for n in range(self._Wcnt):
                T = threading.Thread(name='p4p Context worker', target=Q.handle)
                T.daemon = True
                Ts.append(T)
            for T in Ts:
                T.start()
            _log.debug('Started %d Context worker', self._Wcnt)
            self._Q, self._T = Q, Ts
        return self._Q

    def close(self):
        """Force close all Channels and cancel all Operations
        """
        if self._Q is not None:
            for T in self._T:
                self._Q.interrupt()
            for n, T in enumerate(self._T):
                _log.debug('Join Context worker %d', n)
                T.join()
            _log.debug('Joined Context workers')
            self._Q, self._T = None, None
        super(Context, self).close()

    def get(self, name, request=None, timeout=5.0, throw=True):
        """Fetch current value of some number of PVs.

        :param name: A single name string or list of name strings
        :param request: A :py:class:`p4p.Value` or string to qualify this request, or None to use a default.
        :param float timeout: Operation timeout in seconds
        :param bool throw: When true, operation error throws an exception.  If False then the Exception is returned instead of the Value

        :returns: A p4p.Value or Exception, or list of same.  Subject to :py:ref:`unwrap`.

        When invoked with a single name then returns is a single value.
        When invoked with a list of name, then returns a list of values

        >>> ctxt = Context('pva')
        >>> V = ctxt.get('pv:name')
        >>> A, B = ctxt.get(['pv:1', 'pv:2'])
        >>>
        """
        singlepv = isinstance(name, (bytes, unicode))
        if singlepv:
            name = [name]
            request = [request]

        elif request is None:
            request = [None] * len(name)

        assert len(name) == len(request), (name, request)

        # use Queue instead of Event to allow KeyboardInterrupt
        done = Queue()
        result = [TimeoutError()] * len(name)
        ops = [None] * len(name)

        raw_get = super(Context, self).get

        try:
            for i, (N, req) in enumerate(izip(name, request)):
                def cb(value, i=i):
                    try:
                        if not isinstance(value, Cancelled):
                            done.put_nowait((value, i))
                        _log.debug('get %s Q %s', N, LazyRepr(value))
                    except:
                        _log.exception("Error queuing get result %s", value)

                _log.debug('get %s w/ %s', N, req)
                ops[i] = raw_get(N, cb, request=req)

            for _n in range(len(name)):
                try:
                    value, i = done.get(timeout=timeout)
                except Empty:
                    if throw:
                        _log.debug('timeout %s after %s', name[i], timeout)
                        raise TimeoutError()
                    break
                _log.debug('got %s %s', name[i], LazyRepr(value))
                if throw and isinstance(value, Exception):
                    raise value
                result[i] = value

        finally:
            [op and op.close() for op in ops]

        if singlepv:
            return result[0]
        else:
            return result

    def put(self, name, values, request=None, timeout=5.0, throw=True,
            process=None, wait=None, get=True):
        """Write a new value of some number of PVs.

        :param name: A single name string or list of name strings
        :param values: A single value, a list of values, a dict, a `Value`.  May be modified by the constructor nt= argument.
        :param request: A :py:class:`p4p.Value` or string to qualify this request, or None to use a default.
        :param float timeout: Operation timeout in seconds
        :param bool throw: When true, operation error throws an exception.
                     If False then the Exception is returned instead of the Value
        :param str process: Control remote processing.  May be 'true', 'false', 'passive', or None.
        :param bool wait: Wait for all server processing to complete.
        :param bool get: Whether to do a Get before the Put.  If True then the value passed to the builder callable
                         will be initialized with recent PV values.  eg. use this with NTEnum to find the enumeration list.

        :returns: A None or Exception, or list of same

        When invoked with a single name then returns is a single value.
        When invoked with a list of name, then returns a list of values

        If 'wait' or 'process' is specified, then 'request' must be omitted or None.

        >>> ctxt = Context('pva')
        >>> ctxt.put('pv:name', 5.0)
        >>> ctxt.put(['pv:1', 'pv:2'], [1.0, 2.0])
        >>> ctxt.put('pv:name', {'value':5})
        >>>

        The provided value(s) will be automatically coerced to the target type.
        If this is not possible then an Exception is raised/returned.

        Unless the provided value is a dict, it is assumed to be a plain value
        and an attempt is made to store it in '.value' field.
        """
        if request and (process or wait is not None):
            raise ValueError("request= is mutually exclusive to process= or wait=")
        elif process or wait is not None:
            request = 'field()record[block=%s,process=%s]' % ('true' if wait else 'false', process or 'passive')

        singlepv = isinstance(name, (bytes, unicode))
        if singlepv:
            name = [name]
            values = [values]
            request = [request]

        elif request is None:
            request = [None] * len(name)

        assert len(name) == len(request), (name, request)
        assert len(name) == len(values), (name, values)

        # use Queue instead of Event to allow KeyboardInterrupt
        done = Queue()
        result = [TimeoutError()] * len(name)
        ops = [None] * len(name)

        raw_put = super(Context, self).put

        try:
            for i, (n, value, req) in enumerate(izip(name, values, request)):
                if isinstance(value, (bytes, unicode)) and value[:1] == '{':
                    try:
                        value = json.loads(value)
                    except ValueError:
                        raise ValueError("Unable to interpret '%s' as json" % value)

                # completion callback
                def cb(value, i=i):
                    try:
                        done.put_nowait((value, i))
                    except:
                        _log.exception("Error queuing put result %s", LazyRepr(value))

                ops[i] = raw_put(n, cb, builder=value, request=req, get=get)

            for _n in range(len(name)):
                try:
                    value, i = done.get(timeout=timeout)
                except Empty:
                    if throw:
                        raise TimeoutError()
                    break
                if throw and isinstance(value, Exception):
                    raise value
                result[i] = value

            if singlepv:
                return result[0]
            else:
                return result
        finally:
            [op and op.close() for op in ops]

    def rpc(self, name, value, request=None, timeout=5.0, throw=True):
        """Perform a Remote Procedure Call (RPC) operation

        :param str name: PV name string
        :param Value value: Arguments.  Must be Value instance
        :param request: A :py:class:`p4p.Value` or string to qualify this request, or None to use a default.
        :param float timeout: Operation timeout in seconds
        :param bool throw: When true, operation error throws an exception.
                     If False then the Exception is returned instead of the Value

        :returns: A Value or Exception.  Subject to :py:ref:`unwrap`.

        >>> ctxt = Context('pva')
        >>> ctxt.rpc('pv:name:add', {'A':5, 'B'; 6})
        >>>

        The provided value(s) will be automatically coerced to the target type.
        If this is not possible then an Exception is raised/returned.

        Unless the provided value is a dict, it is assumed to be a plain value
        and an attempt is made to store it in '.value' field.
        """
        done = Queue()

        op = super(Context, self).rpc(name, done.put_nowait, value, request=request)

        try:
            try:
                result = done.get(timeout=timeout)
            except Empty:
                result = TimeoutError()
            if throw and isinstance(result, Exception):
                raise result

            return result
        except:
            op.close()
            raise

    def monitor(self, name, cb, request=None, notify_disconnect=False, queue=None, subscription_arg=False):
        """Create a subscription.

        :param str name: PV name string
        :param callable cb: Processing callback
        :param request: A :py:class:`p4p.Value` or string to qualify this request, or None to use a default.
        :param bool notify_disconnect: In additional to Values, the callback may also be call with instances of Exception.
                                       Specifically: Disconnected , RemoteError, or Cancelled
        :param bool subscription_arg: If set, pass `Subscription` as second argument of callback
        :param WorkQueue queue: A work queue through which monitor callbacks are dispatched.
        :returns: a :py:class:`Subscription` instance

        The callable will be invoked with either one or two arguments.  The first is one of:

        * A p4p.Value (Subject to :py:ref:`unwrap`)
        * A sub-class of Exception (Disconnected , RemoteError, or Cancelled)

        If subscription_arg=True, the callback is passed its `Subscription` as a second argument.  Since 3.1.0.

        While it is acceptable to block in a monitor callback, this does not scale passed the size of the thread
        pool in which monitor callbaks run (~4).  To utilize flow control on a larger scale, see `Subscription.pause`.
        """
        R = Subscription(self, name, cb, notify_disconnect=notify_disconnect, queue=queue, subscription_arg=subscription_arg)

        R._S = super(Context, self).monitor(name, R._event, request)
        
        if notify_disconnect:
            # all subscriptions are initially disconnected
            R._event(Disconnected())
        return R

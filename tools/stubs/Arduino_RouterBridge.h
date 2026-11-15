/*
 * Stub Arduino_RouterBridge.h for host-side syntax checking ONLY.
 *
 * API surface transcribed from arduino-libraries/Arduino_RouterBridge v0.4.4
 * (src/bridge.h, src/monitor.h, src/singletons.cpp) and the official UNO Q user
 * manual's Bridge chapter.
 *
 * The real handlers are TEMPLATED on MsgPack-compatible types, not fixed to
 * String fn(String). That is modelled faithfully here, because getting it wrong
 * would let a sketch compile on the laptop that then fails on the board.
 */

#ifndef STUB_ARDUINO_ROUTERBRIDGE_H
#define STUB_ARDUINO_ROUTERBRIDGE_H

#include <Arduino.h>

/*
 * Monitor.
 *
 * The real object is a BridgeMonitor<> singleton that carries text to the Linux
 * side over the router's mon/write RPC method. It derives from Stream, so the
 * full print/println family is available.
 */
class BridgeMonitor : public Stream {
public:
  /* The real signature is begin(unsigned long _legacy_baud = 0,
   * uint16_t _legacy_config = 0); both parameters are unused and exist only for
   * Stream compatibility. Modelled so a call with or without arguments parses. */
  bool begin(unsigned long = 0, uint16_t = 0) { return true; }
  void end(void) {}
  operator bool() const { return true; }
};

/*
 * Asynchronous RPC handle returned by Bridge.call().
 * .result() blocks for the response and may only be retrieved once.
 */
template <typename... Args>
class RpcCall {
public:
  template <typename T>
  bool result(T &) { return true; }
  bool available(void) const { return true; }
  int  error(void) const { return 0; }
};

class BridgeClass {
public:
  /* Blocks until the Linux arduino-router service responds; no timeout on the
   * MCU side. DEFAULT_SERIAL_BAUD is 115200. */
  bool begin(unsigned long = 115200) { return true; }
  void end(void) {}

  /* Registers func as an RPC service executing on the high-priority background
   * RPC thread. Thread-UNSAFE: must not touch Arduino APIs, and must not call
   * Bridge.call / Monitor.print / Serial.print — doing so deadlocks. */
  template <typename F>
  bool provide(const char *name, F &&func) { (void)name; (void)func; return true; }

  /* Same registration, but func executes inside the main loop() context via the
   * __loopHook / safeUpdate() mechanism. Use this for anything touching SPI,
   * GPIO or the LED matrix. */
  template <typename F>
  bool provide_safe(const char *name, F &&func) { (void)name; (void)func; return true; }

  template <typename... Args>
  RpcCall<Args...> call(const char *method, Args &&...args) {
    (void)method;
    return RpcCall<Args...>();
  }

  template <typename... Args>
  void notify(const char *method, Args &&...args) { (void)method; }

  operator bool() const { return true; }
};

/* The real library declares these as `extern BridgeClass Bridge;` and
 * `extern BridgeMonitor<> Monitor;` (BridgeMonitor is a variadic template there,
 * collapsed to a plain class here because nothing in this project depends on the
 * template parameters). Both are singletons defined in singletons.cpp. */
extern BridgeClass Bridge;
extern BridgeMonitor Monitor;

#endif /* STUB_ARDUINO_ROUTERBRIDGE_H */

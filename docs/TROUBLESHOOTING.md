# PhishTap Lite - Troubleshooting Guide

Common issues and their solutions.

---

## 🔴 Critical Issues (App Won't Start)

### App Lab Shows "Port 7000 Already in Use"

**Symptom:**
```
Error: Port 7000 is declared by multiple sources: arduino:web_ui, app.yaml
```

**Cause:**
The `app.yaml` file declares port 7000, but the `web_ui` brick already claims it.

**Solution:**
```bash
# Edit app.yaml and remove the ports section
nano /home/arduino/ArduinoApps/phishtap/app.yaml

# Should look like this (no ports section):
name: phishtap
description: "PhishTap Lite - Phishing awareness game"
bricks:
  - web_ui
```

Then restart the app.

---

### Python Import Errors

**Symptom:**
```
ModuleNotFoundError: No module named 'fastapi'
```

**Cause:**
Dependencies not installed in the container.

**Solution:**
```bash
# SSH into the board
ssh arduino@<board-ip>

# Enter the app container
docker exec -it phishtap-main-1 bash

# Install dependencies
pip install -r /app/python/requirements.txt

# Exit and restart
exit
arduino-app-cli app restart user:phishtap
```

---

### Firmware Compilation Fails

**Symptom:**
```
error: 'Arduino_LED_Matrix' does not name a type
```

**Cause:**
Missing Arduino library.

**Solution:**
```bash
# Install the LED Matrix library
arduino-cli lib install "Arduino_LED_Matrix"

# Verify installation
arduino-cli lib list | grep LED_Matrix

# Recompile
cd /home/arduino/ArduinoApps/phishtap/sketch
arduino-cli compile --fqbn arduino:zephyr:unoq .
```

---

## 🟡 Runtime Issues (App Starts But Doesn't Work)

### RFID Card Not Detected

**Symptom:**
- Tapping card does nothing
- Serial monitor shows no `[rfid] CARD TAP` messages

**Diagnostic Steps:**

1. **Check serial monitor:**
```bash
arduino-app-cli app logs user:phishtap --follow
```
Look for:
```
[rfid] init: version 0x82 (clone detected)
[rfid] state: seek
```

2. **Test with hw_selftest:**
```bash
# Deploy the selftest sketch
tools/deploy_to_board.sh --sketch hw_selftest

# Run Phase B (RFID test)
# Follow the prompts on serial monitor
```

3. **Verify wiring:**
- VCC → 3.3V (NOT 5V!)
- GND → GND
- SDA → D10
- SCK → D13
- MOSI → D11
- MISO → D12
- RST → D5

**Common Causes:**
- Card is not MIFARE Classic (try a different card)
- Reader is counterfeit but not initialized properly
- SPI bus conflict (check for other SPI devices)
- Power supply insufficient (use 5V/3A adapter)

---

### QR Code Won't Scan

**Symptom:**
- QR code appears on kiosk
- Phone camera can't read it

**Solutions:**

1. **Increase screen brightness**
   - Set laptop to maximum brightness
   - Disable auto-dimming

2. **Clean the screen**
   - Wipe with microfiber cloth
   - Remove screen protector if present

3. **Adjust QR size**
   ```javascript
   // In assets/js/kiosk.js, line ~150
   const qr = qrcode(0, 'M');
   qr.addData(url);
   qr.make();
   document.getElementById('qr').innerHTML = qr.createImgTag(6); // Increase from 4 to 6
   ```

4. **Use PIN fallback**
   - Show the 4-digit PIN on kiosk
   - Manually enter at `http://<board-ip>:7000/join.html?pin=XXXX`

---

### Phone Can't Connect to Board

**Symptom:**
- QR scans successfully
- Phone shows "Connection refused" or timeout

**Diagnostic Steps:**

1. **Verify network connectivity:**
```bash
# From laptop
ping <board-ip>

# From phone (use a ping app)
ping <board-ip>
```

2. **Check firewall:**
```bash
# On the board
sudo ufw status
# Should show: Status: inactive

# If active, allow port 7000
sudo ufw allow 7000/tcp
```

3. **Test port directly:**
```bash
# From laptop
curl http://<board-ip>:7000/api/health
# Should return: {"status":"ok","version":"1.0.0"}
```

4. **Check for client isolation:**
   - Some college networks block device-to-device traffic
   - Solution: Use laptop hotspot instead

**Network Setup Options:**

**Option A: Laptop Hotspot (Most Reliable)**
```bash
# On laptop (Linux)
nmcli device wifi hotspot con-name PhishTapDemo ssid PhishTapDemo password "demo1234"

# Connect board to this hotspot
# Board will get IP like 10.42.0.x
```

**Option B: Direct Ethernet**
```bash
# Connect laptop to board via USB-C
# Board gets IP 192.168.7.2
# Access at http://192.168.7.2:7000
```

**Option C: College WiFi**
```bash
# Connect both to same network
# Check for client isolation
# If blocked, use Option A
```

---

### Game Freezes or Crashes

**Symptom:**
- Phone shows loading spinner forever
- Kiosk stops updating
- No response to card taps

**Diagnostic Steps:**

1. **Check Python logs:**
```bash
arduino-app-cli app logs user:phishtap --tail 50
```
Look for:
- `Traceback` (Python exception)
- `ConnectionError` (bridge failure)
- `TimeoutError` (MCU not responding)

2. **Check MCU serial output:**
```bash
# Open serial monitor
screen /dev/ttyACM0 115200

# Or via App Lab
# Click "Serial Monitor" tab
```
Look for:
- `[error]` messages
- `Guru Meditation Error` (crash)
- `Watchdog timeout`

3. **Restart services:**
```bash
# Restart the app
arduino-app-cli app restart user:phishtap

# If that fails, restart the bridge
sudo systemctl restart arduino-router

# Last resort: reboot the board
sudo reboot
```

**Common Causes:**
- Memory leak in Python (check with `docker stats`)
- Bridge connection dropped (check `arduino-router` service)
- Firmware crash (check serial monitor for stack trace)
- Database locked (check for concurrent writes)

---

### LED Matrix Shows Garbled Output

**Symptom:**
- Random pixels lit
- Text is unreadable
- Animations stutter

**Solutions:**

1. **Check power supply:**
   - Use 5V/3A adapter (not laptop USB)
   - Measure voltage at board: should be 4.8-5.2V

2. **Reduce brightness:**
   ```cpp
   // In sketch/phishtap/pt_matrix.h
   matrix.setBrightness(128); // Reduce from 255 to 128
   ```

3. **Check for SPI conflicts:**
   - Disconnect other SPI devices
   - Verify no other library uses SPI

4. **Reflash firmware:**
   ```bash
   tools/deploy_to_board.sh --sketch phishtap --force
   ```

---

## 🟢 Minor Issues (Cosmetic or Edge Cases)

### Admin Panel Shows "Unauthorized"

**Symptom:**
- Can't access `/admin.html`
- API returns 401 Unauthorized

**Solution:**
```bash
# Get the admin token
curl -X POST http://<board-ip>:7000/api/admin/login \
  -H "Content-Type: application/json" \
  -d '{"pin":"1234"}'

# Use the token in requests
curl http://<board-ip>:7000/api/admin/analytics \
  -H "Authorization: Bearer <token>"
```

Default PIN is `1234`. Change it in `python/phishtap/config.py`.

---

### Leaderboard Shows Wrong Scores

**Symptom:**
- Scores don't match game results
- Players appear multiple times

**Cause:**
Database inconsistency or duplicate player records.

**Solution:**
```bash
# Reset the database
curl -X POST http://<board-ip>:7000/api/admin/reset \
  -H "Authorization: Bearer <token>"

# Or manually clean up
sqlite3 /home/arduino/ArduinoApps/phishtap/data/phishtap.db
> DELETE FROM players WHERE nickname IS NULL;
> VACUUM;
> .quit
```

---

### Buzzer Doesn't Sound

**Symptom:**
- No audio feedback during game
- Serial monitor shows `[buzzer] play` but no sound

**Solutions:**

1. **Check wiring:**
   - Buzzer + → D6 (via NPN transistor)
   - Buzzer - → GND
   - Transistor base → 1kΩ → D6
   - Transistor emitter → GND
   - Transistor collector → Buzzer +

2. **Test buzzer directly:**
   ```cpp
   // Upload a simple test sketch
   void setup() {
     pinMode(6, OUTPUT);
   }
   void loop() {
     digitalWrite(6, HIGH);
     delay(500);
     digitalWrite(6, LOW);
     delay(500);
   }
   ```

3. **Check firmware:**
   ```bash
   # Verify buzzer is enabled
   grep -r "PT_BUZZER_ENABLED" sketch/phishtap/
   # Should be defined in pt_config.h
   ```

---

### Lives LEDs Don't Light Up

**Symptom:**
- D7/D8/D9 LEDs stay off
- Lives only shown on matrix

**Solutions:**

1. **Check wiring:**
   - D7 → 220Ω → LED anode → LED cathode → GND
   - D8 → 220Ω → LED anode → LED cathode → GND
   - D9 → 220Ω → LED anode → LED cathode → GND

2. **Test LEDs directly:**
   ```cpp
   void setup() {
     pinMode(7, OUTPUT);
     pinMode(8, OUTPUT);
     pinMode(9, OUTPUT);
   }
   void loop() {
     digitalWrite(7, HIGH);
     delay(500);
     digitalWrite(7, LOW);
     digitalWrite(8, HIGH);
     delay(500);
     digitalWrite(8, LOW);
     digitalWrite(9, HIGH);
     delay(500);
     digitalWrite(9, LOW);
   }
   ```

3. **Check resistor values:**
   - Should be 220Ω (red-red-brown)
   - Not 10kΩ (brown-black-orange)

---

## 🔍 Advanced Debugging

### Enable Verbose Logging

**Python backend:**
```bash
# Edit python/phishtap/config.py
LOG_LEVEL = "DEBUG"

# Restart app
arduino-app-cli app restart user:phishtap
```

**Firmware:**
```cpp
// Edit sketch/phishtap/pt_config.h
#define PT_DEBUG 1

// Recompile and upload
tools/deploy_to_board.sh --sketch phishtap
```

---

### Capture Network Traffic

```bash
# On laptop
tcpdump -i any -w phishtap.pcap host <board-ip> and port 7000

# Reproduce the issue
# Stop capture with Ctrl+C

# Analyze
wireshark phishtap.pcap
```

Look for:
- HTTP 500 errors (backend crash)
- WebSocket disconnects (bridge failure)
- Retransmissions (network issues)

---

### Profile Performance

**Python:**
```bash
# Install profiler
pip install py-spy

# Profile the running app
py-spy top --pid $(pgrep -f "python.*phishtap")
```

**Firmware:**
```cpp
// Add timing to loop()
unsigned long start = micros();
// ... existing code ...
unsigned long elapsed = micros() - start;
if (elapsed > 10000) {
  Serial.printf("[perf] loop took %lu us\n", elapsed);
}
```

---

### Check Resource Usage

```bash
# CPU and memory
docker stats phishtap-main-1

# Disk usage
du -sh /home/arduino/ArduinoApps/phishtap/

# Database size
ls -lh /home/arduino/ArduinoApps/phishtap/data/phishtap.db
```

---

## 📞 Getting Help

### Before Asking for Help

1. **Check this guide** - 90% of issues are covered here
2. **Read the logs** - `arduino-app-cli app logs user:phishtap`
3. **Check serial monitor** - Look for error messages
4. **Verify hardware** - Run `hw_selftest` sketch
5. **Test network** - Ping the board from your phone

### Providing a Bug Report

Include:
1. **Board info:**
   ```bash
   cat /etc/os-release
   arduino-cli version
   ```

2. **App status:**
   ```bash
   arduino-app-cli app list
   arduino-app-cli app logs user:phishtap --tail 100
   ```

3. **Firmware version:**
   ```bash
   # From serial monitor
   [boot] PhishTap Lite v1.0.0
   ```

4. **Steps to reproduce:**
   - What you did
   - What you expected
   - What actually happened

5. **Logs and screenshots:**
   - Python logs
   - Serial monitor output
   - Photos of hardware setup

### Where to Ask

- **GitHub Issues:** https://github.com/priyanshu2119/phishtap-lite/issues
- **Arduino Forum:** https://forum.arduino.cc/
- **Email:** priyanshupratik2119@gmail.com

---

## 🛠️ Recovery Procedures

### Complete Reset

If nothing works:

```bash
# 1. Stop the app
arduino-app-cli app stop user:phishtap

# 2. Delete the app
arduino-app-cli app delete user:phishtap

# 3. Clear the database
rm -rf /home/arduino/ArduinoApps/phishtap/data/

# 4. Reflash firmware
tools/deploy_to_board.sh --sketch phishtap --force

# 5. Recreate the app
arduino-app-cli app create phishtap --brick web_ui

# 6. Deploy Python code
scp -r python/* arduino@<board-ip>:/home/arduino/ArduinoApps/phishtap/python/

# 7. Start the app
arduino-app-cli app start user:phishtap
```

---

### Factory Reset (Last Resort)

```bash
# Reboot into recovery mode
sudo reboot recovery

# Or hold the recovery button for 10 seconds

# Reflash the OS
# Follow instructions at https://docs.arduino.cc/tutorials/uno-q/
```

---

## 📚 Additional Resources

- [Arduino UNO Q Documentation](https://docs.arduino.cc/hardware/uno-q/)
- [App Lab Guide](https://docs.arduino.cc/software/app-lab/)
- [FastAPI Documentation](https://fastapi.tiangolo.com/)
- [Socket.IO Documentation](https://socket.io/docs/v4/)
- [MFRC522 Library](https://github.com/miguelbalboa/rfid)

---

**Remember:** Most issues are network-related or caused by insufficient power. Start there before diving into code!

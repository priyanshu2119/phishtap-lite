# Quick Start Guide

Get PhishTap running in 5 minutes.

---

## 📋 What You Need

### Hardware
- ✅ Arduino UNO Q board
- ✅ RC522 RFID module
- ✅ 2-3 MIFARE Classic cards (1KB)
- ✅ USB-C cable (data + power)
- ✅ Laptop for kiosk display
- ✅ Smartphone for playing

### Optional Hardware
- 🔊 Buzzer (TMB12A05 or similar)
- 💡 RGB LEDs (common cathode)
- 💡 3x LEDs for lives indicator
- 🔌 Jumper wires, breadboard

---

## 🚀 5-Minute Setup

### Step 1: Clone the Repository

```bash
git clone https://github.com/priyanshu2119/phishtap-lite.git
cd phishtap-lite
```

### Step 2: Wire the RFID Reader

Connect the RC522 to the Arduino UNO Q:

| RC522 Pin | Arduino Pin |
|-----------|-------------|
| VCC       | 3.3V        |
| GND       | GND         |
| RST       | D5          |
| SDA       | D10         |
| SCK       | D13         |
| MOSI      | D11         |
| MISO      | D12         |

**⚠️ Important:** Use 3.3V, NOT 5V! The RC522 is a 3.3V device.

See [docs/HARDWARE.md](HARDWARE.md) for detailed wiring diagrams.

### Step 3: Deploy Firmware

```bash
# This runs all verification gates and flashes the board
tools/deploy_to_board.sh --sketch phishtap
```

**What this does:**
1. ✅ Runs syntax checks on firmware
2. ✅ Runs 374 Python tests
3. ✅ Validates frontend assets
4. ✅ Compiles with real Arduino toolchain
5. ✅ Flashes firmware to the board

**Expected output:**
```
syntax_check: PASS
host_gate: 374 passed
web_gate: PASS
board_compile: PASS
Flashing... Done!
```

### Step 4: Start the App

```bash
# Start the Python backend
arduino-app-cli app start user:phishtap
```

**Expected output:**
```
Starting phishtap...
Backend running on http://0.0.0.0:7000
Bridge connected to MCU
```

### Step 5: Open the Kiosk

1. Find your board's IP address:
   ```bash
   arduino-app-cli app list
   # Look for "phishtap" and note the IP
   ```

2. Open a browser on your laptop:
   ```
   http://<board-ip>:7000
   ```

3. You should see the kiosk screen with "Tap a card to play"

### Step 6: Play!

1. **Tap** an RFID card on the reader
2. **Scan** the QR code with your phone
3. **Enter** a nickname
4. **Answer** phishing questions (PHISH or SAFE)
5. **Learn** from the review screen

---

## 🎮 First Game Walkthrough

### 1. Card Tap
- Tap your MIFARE card on the RC522 reader
- The LED matrix shows "SCAN QR"
- A QR code appears on the kiosk screen

### 2. Phone Connection
- Open your phone's camera
- Point it at the QR code
- Tap the notification to open the game

### 3. Consent Screen
- Enter a nickname (3-12 characters)
- Read the privacy notice
- Tap "I Understand"

### 4. Gameplay
- Read the message carefully
- Decide: Is this PHISH or SAFE?
- Tap your answer before time runs out
- Correct = points, Wrong = lose a life

### 5. Game Over
- After 3 wrong answers or all messages answered
- Review screen shows what you got wrong
- Learn from the explanations
- Tap "Play Again" to restart

---

## 🔧 Troubleshooting

### "Port 7000 already in use"

```bash
# Stop any existing app
arduino-app-cli app stop user:phishtap

# Check what's using port 7000
sudo lsof -i :7000

# Kill the process
sudo kill -9 <PID>

# Restart
arduino-app-cli app start user:phishtap
```

### "Card not detected"

1. **Check wiring** - Verify all 7 connections
2. **Check card type** - Must be MIFARE Classic (not DESFire)
3. **Run hardware test:**
   ```bash
   tools/deploy_to_board.sh --sketch hw_selftest
   # Follow serial monitor prompts
   ```

### "Phone can't connect"

1. **Check network** - Phone and board must be on same WiFi
2. **Check firewall** - Allow port 7000
3. **Try direct connection:**
   ```bash
   # Connect phone to laptop hotspot
   # Access http://<laptop-ip>:7000
   ```

### "Game freezes"

```bash
# Check logs
arduino-app-cli app logs user:phishtap

# Restart the app
arduino-app-cli app restart user:phishtap

# If persistent, reboot the board
sudo reboot
```

See [docs/TROUBLESHOOTING.md](TROUBLESHOOTING.md) for more issues.

---

## 📱 Network Setup

### Option A: Laptop Hotspot (Recommended)

**Linux:**
```bash
nmcli device wifi hotspot con-name PhishTapDemo ssid PhishTapDemo password "demo1234"
```

**Windows:**
- Settings → Network & Internet → Mobile hotspot
- Network name: PhishTapDemo
- Password: demo1234

**macOS:**
- System Preferences → Sharing → Internet Sharing
- WiFi Options → Network Name: PhishTapDemo

### Option B: College WiFi

1. Connect board to college WiFi
2. Connect phone to same network
3. Check for client isolation (if devices can't see each other, use Option A)

### Option C: Portable Router

1. Set up router with SSID: PhishTapDemo
2. Connect board and phones to router
3. Most reliable for demos

See [docs/NETWORK_SETUP.md](NETWORK_SETUP.md) for detailed instructions.

---

## 🎯 Demo Tips

### Before the Demo

- [ ] Test everything (card tap, QR scan, gameplay)
- [ ] Charge all devices
- [ ] Set laptop brightness to maximum
- [ ] Disable screen saver
- [ ] Have backup cards ready

### During the Demo

- [ ] Start with a live demonstration
- [ ] Explain the phishing cues as you play
- [ ] Invite volunteers to try
- [ ] Show the review screen (where learning happens)
- [ ] Mention the privacy features (no data collection)

### After the Demo

- [ ] Collect feedback
- [ ] Export analytics (admin panel)
- [ ] Share the GitHub repo
- [ ] Encourage students to build their own

See [docs/DEMO_SCRIPT.md](DEMO_SCRIPT.md) for a complete 15-minute script.

---

## 📚 Next Steps

### For Users
- Read the [Demo Script](DEMO_SCRIPT.md) for presentation tips
- Check [Troubleshooting](TROUBLESHOOTING.md) if issues arise
- Explore the [Admin Panel](http://<board-ip>:7000/admin.html)

### For Developers
- Read the [Architecture](ARCHITECTURE.md) to understand the system
- Check the [API Reference](API.md) for integration details
- See [Contributing](../CONTRIBUTING.md) to add features

### For Educators
- Review the [Corpus](../python/corpus.json) to see all messages
- Export analytics to assess class performance
- Customize messages for your context (edit corpus.json)

---

## 🆘 Getting Help

### Quick Fixes
1. **Restart the app:** `arduino-app-cli app restart user:phishtap`
2. **Check logs:** `arduino-app-cli app logs user:phishtap`
3. **Verify hardware:** Run `hw_selftest` sketch
4. **Test network:** Ping the board from your phone

### Resources
- 📖 [Full Documentation](../docs/)
- 🐛 [Report Issues](https://github.com/priyanshu2119/phishtap-lite/issues)
- 💬 [Discussions](https://github.com/priyanshu2119/phishtap-lite/discussions)
- 📧 Email: priyanshupratik2119@gmail.com

---

## ✅ Success Checklist

Before your first demo, verify:

- [ ] Firmware deployed successfully
- [ ] App starts without errors
- [ ] Kiosk loads in browser
- [ ] Card tap generates QR code
- [ ] Phone can scan and connect
- [ ] Game plays through to completion
- [ ] Review screen shows explanations
- [ ] Leaderboard updates correctly
- [ ] Admin panel accessible
- [ ] Network stable for 10+ minutes

**All checked? You're ready to demo! 🎉**

---

## 🎓 Example Session

Here's what a typical 15-minute demo looks like:

**Minute 0-2: Introduction**
- "How many of you have received a suspicious SMS this week?"
- Show the arcade console
- Explain the concept

**Minute 2-7: Live Demo**
- Tap a card, scan QR, play 2-3 rounds
- Narrate your thinking process
- Show the review screen

**Minute 7-12: Audience Participation**
- Invite 2-3 volunteers
- Let them play while you explain
- Show the leaderboard

**Minute 12-15: Technical Deep Dive**
- Explain the architecture (for CS students)
- Show the code on GitHub
- Discuss privacy features

**After: Free Play**
- Let everyone try
- Answer questions
- Share the repo URL

---

**Ready? Let's fight phishing, one game at a time! 🎮**

[Back to README](../README.md)

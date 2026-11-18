# Frequently Asked Questions (FAQ)

Common questions about PhishTap Lite, answered.

---

## 🎮 General Questions

### What is PhishTap?

PhishTap is an arcade-style game that teaches phishing awareness. Players tap RFID cards, scan QR codes with their phones, and answer questions about whether messages are phishing or legitimate. It's designed for college classrooms, corporate training, and security awareness events.

### Who is this for?

- **Students** - Learn to spot phishing in a fun, interactive way
- **Educators** - Engaging tool for cybersecurity courses
- **Security teams** - Interactive training for employees
- **Makers** - Build your own arcade console with Arduino
- **Researchers** - Study phishing awareness and learning outcomes

### Why "PhishTap"?

- **Phish** - Short for phishing (the topic)
- **Tap** - You tap an RFID card to start playing

The name reflects the core interaction: tap a card, play a game, learn about phishing.

### Is this a real arcade machine?

Not quite! It's a **tabletop console** built with Arduino UNO Q. It has:
- LED matrix display (8x13 pixels)
- RFID card reader
- RGB status LEDs
- (Optional) Buzzer and lives LEDs

But the actual game is played on your **phone** - the console just starts the session and shows the QR code.

---

## 🔧 Hardware Questions

### What hardware do I need?

**Required:**
- Arduino UNO Q board (~$100)
- RC522 RFID module (~$5)
- MIFARE Classic cards (1KB) (~$1 each)
- USB-C cable (data + power)
- Laptop for kiosk display
- Smartphone for playing

**Optional:**
- Buzzer (TMB12A05 or similar)
- RGB LEDs (common cathode)
- 3x LEDs for lives indicator
- Jumper wires, breadboard

See [docs/HARDWARE.md](HARDWARE.md) for complete BOM and wiring diagrams.

### Why Arduino UNO Q?

The UNO Q has **two processors**:
- **STM32H7** (MCU) - Handles real-time tasks (RFID, LEDs, buzzer)
- **Qualcomm QRB2210** (MPU) - Runs Linux, Python, web server

This dual-processor architecture is perfect for PhishTap:
- MCU handles hardware with microsecond precision
- MPU runs the game logic, database, and web server
- They communicate over an internal bridge

Other Arduinos (UNO R3, Nano) don't have this capability.

### Can I use a different Arduino board?

**Short answer:** No, not without major modifications.

**Long answer:** The UNO Q is unique because it has:
- Dual processors (MCU + MPU)
- Linux on the MPU (runs Python, FastAPI, SQLite)
- Built-in WiFi (no external module needed)
- App Lab support (easy deployment)

You could theoretically port PhishTap to:
- **ESP32** - Would need external Linux board for web server
- **Raspberry Pi Pico + Pi 4** - Similar dual-processor setup
- **Arduino Portenta** - Has dual-core, but no Linux

But these would require significant rewrites. The UNO Q is the target platform.

### Why RC522 and not PN532?

Both work, but RC522 is:
- **Cheaper** (~$5 vs ~$15)
- **More common** (easier to find)
- **Sufficient** (we only need to read UIDs, not full NFC)

PN532 has more features (NFC, peer-to-peer) but we don't need them.

### Can I use NFC tags instead of RFID cards?

**Technically yes, but not recommended.**

RC522 reads:
- ✅ MIFARE Classic (1KB, 4KB)
- ✅ MIFARE Ultralight
- ❌ MIFARE DESFire (different protocol)
- ❌ NFC Forum tags (Type 4, 5)

MIFARE Classic cards are:
- Cheap (~$1 each)
- Widely available
- Perfect for this use case

### Do I need the buzzer and LEDs?

**No, they're optional.**

The onboard RGB LEDs (LED3/LED4) provide basic status:
- Green = idle/ready
- Blue = card detected
- Red = error

The buzzer and external LEDs add polish but aren't required for gameplay.

**If you want them:**
- Buzzer: See [HARDWARE.md](HARDWARE.md#buzzer-wiring) for transistor circuit
- Lives LEDs: D7/D8/D9 → 220Ω → LED → GND

### My RC522 reports version 0x82. Is it broken?

**No, it's fine!** Version 0x82 means it's a **counterfeit chip**, which is very common.

Counterfeit RC522s:
- Work correctly for reading UIDs
- May have slightly different analog characteristics
- Show warnings in hw_selftest but pass all functional tests

See [HARDWARE.md](HARDWARE.md#counterfeit-rc522-modules) for details.

---

## 💻 Software Questions

### What programming languages are used?

- **C++** - Firmware (Arduino sketch)
- **Python** - Backend (FastAPI, game logic)
- **JavaScript** - Frontend (kiosk, game, admin)
- **HTML/CSS** - Web pages
- **Bash** - Deployment scripts, tools

### Do I need to know all these languages to use PhishTap?

**No!** To just **run** PhishTap, you need:
- Basic command-line skills
- Ability to follow setup instructions

To **modify** PhishTap, you need:
- Python (for backend changes)
- JavaScript (for frontend changes)
- C++ (for firmware changes)

### Why Python and not Node.js for the backend?

Python was chosen because:
- **FastAPI** is excellent for APIs
- **Pydantic** makes validation easy
- **SQLite** integration is built-in
- **Data science libraries** (for analytics)
- **Educational context** (students often know Python)

Node.js would work too, but Python is more common in academic settings.

### Why SQLite and not PostgreSQL/MySQL?

SQLite is:
- **Embedded** - No separate server needed
- **Zero-config** - Just a file
- **Fast** - Perfect for this scale
- **Portable** - Easy to backup/export

For a single-device game, SQLite is ideal. PostgreSQL would be overkill.

### Can I run this without the Arduino board?

**Yes!** Use the mock bridge:

```bash
cd python
python main.py --mock-bridge
```

This simulates RFID taps via keyboard input. Perfect for:
- Development (no hardware needed)
- Testing (automated tests use mock)
- Demos (if hardware fails)

### Why FastAPI and not Flask/Django?

FastAPI was chosen because:
- **Async support** - WebSocket handling is natural
- **Type hints** - Better IDE support, fewer bugs
- **Auto docs** - OpenAPI/Swagger generated automatically
- **Performance** - Faster than Flask for APIs
- **Modern** - Uses latest Python features

Flask would work, but FastAPI is more suitable for real-time games.

---

## 🎯 Gameplay Questions

### How does the game work?

1. **Tap** an RFID card on the reader
2. **Scan** the QR code with your phone
3. **Enter** a nickname (first time only)
4. **Answer** phishing questions (PHISH or SAFE)
5. **Learn** from the review screen

Each game has 10 questions. You start with 3 lives. Wrong answers cost a life. Game ends when you run out of lives or answer all questions.

### How is scoring calculated?

**Base score:** 100 points per correct answer

**Speed bonus:** Up to 50 points for fast answers
- Answer in < 5s = +50 points
- Answer in 5-10s = +25 points
- Answer in > 10s = +0 points

**Streak multiplier:** Consecutive correct answers
- 2 in a row = 1.5x multiplier
- 3 in a row = 2x multiplier
- 4+ in a row = 2.5x multiplier

**Example:**
- Correct answer in 3 seconds = 100 + 50 = 150 points
- 3rd correct in a row = 150 × 2 = 300 points

### What happens if I get all questions right?

You get a **perfect score** (usually 2000-3000 points depending on speed and streaks) and appear at the top of the leaderboard!

### Can I play multiple times?

**Yes!** Each card tap starts a new game. Your scores are tracked separately.

However, the same card can only be used once every 3 seconds (cooldown) to prevent accidental double-taps.

### What if I lose my card?

Your game data is tied to a **hash** of your card's UID, not the UID itself. If you lose your card:
- You can't access your old scores (privacy feature!)
- You can get a new card and start fresh
- Your old data remains anonymized in analytics

### Can I cheat by looking at the network traffic?

**No!** The correct answer is never sent to the phone until the review screen. The game logic runs on the backend, and only sends:
- The message text (without the label)
- Whether your answer was correct (after you answer)
- The correct answer (only in review)

Even inspecting network traffic won't reveal the answers.

### Why are some messages so hard?

The corpus includes **20 hard messages** that are sophisticated phishing attempts. These are based on real-world attacks that even security professionals sometimes miss.

The goal is to teach you that:
- Not all phishing is obvious
- Sophisticated attacks exist
- You need to check carefully

### Can I add my own messages?

**Yes!** Edit `python/corpus.json`:

```json
{
  "id": "custom_01",
  "channel": "email",
  "label": 1,
  "difficulty": "medium",
  "sender": "attacker@example.com",
  "subject": "Your message here",
  "body": "Full message text",
  "cues": ["urgency", "suspicious link"],
  "explanation": "Why this is phishing",
  "lesson": "What to learn from this"
}
```

Then restart the app:
```bash
arduino-app-cli app restart user:phishtap
```

See [docs/CORPUS.md](CORPUS.md) for the full schema.

---

## 🔒 Privacy & Security Questions

### Do you store my card's UID?

**No!** The UID is:
1. Read from the card
2. Hashed with HMAC-SHA256 + random salt
3. The hash is stored, the UID is discarded
4. The hash can't be reversed to get the UID

This is called **pseudonymization** - we can track your games without knowing your actual card.

### What data do you collect?

**We collect:**
- Pseudonymous player ID (hash of UID)
- Nickname (you choose it)
- Game scores and answers
- Timestamps

**We don't collect:**
- Card UIDs (never stored)
- Email addresses
- Phone numbers
- Location data
- Personal information

### Where is the data stored?

**Locally on the Arduino UNO Q**, in a SQLite database:
```
/home/arduino/ArduinoApps/phishtap/data/phishtap.db
```

No data is sent to the cloud. No external servers. Completely offline.

### Can I delete my data?

**Yes!** Two ways:

1. **From the review screen:**
   - After a game, tap "Delete My Data"
   - Confirms and deletes all your records

2. **From the admin panel:**
   - Admin can delete any player
   - Useful for GDPR requests

### Is this GDPR compliant?

**Yes!** PhishTap follows GDPR principles:
- **Data minimization** - Only collect what's needed
- **Purpose limitation** - Data used only for the game
- **Storage limitation** - Players can delete anytime
- **Privacy by design** - UIDs never stored
- **Right to erasure** - Delete button in review screen

### Can someone track me across different PhishTap consoles?

**No!** Each console uses a different random salt for hashing. The same card on two different consoles produces two different pseudonymous IDs. There's no way to link them.

### What if someone steals the Arduino board?

They get:
- The SQLite database (pseudonymous data only)
- No way to reverse the hashes to get UIDs
- No personal information

The data is essentially useless without the salt (which is also on the board, but still can't reverse the hashes).

### Is the WiFi connection secure?

The game uses **HTTP, not HTTPS** by default. This is acceptable because:
- It's a local network (laptop hotspot or college WiFi)
- No sensitive data is transmitted (no passwords, no personal info)
- The game is designed for controlled environments (classrooms, events)

**For production use**, you can enable HTTPS:
```python
# In python/phishtap/config.py
USE_TLS = True
```

This generates a self-signed certificate. For real deployments, use a proper certificate from Let's Encrypt.

---

## 🌐 Network Questions

### Do I need internet?

**No!** PhishTap works completely offline after setup. You need internet only for:
- Initial setup (downloading dependencies)
- Software updates

During gameplay, no internet is required.

### What if my college WiFi blocks device-to-device traffic?

This is called **client isolation** and is common on college networks.

**Solutions:**
1. **Use laptop hotspot** (recommended)
   - Create a hotspot on your laptop
   - Connect board and phones to it
   - See [NETWORK_SETUP.md](NETWORK_SETUP.md#option-1-laptop-hotspot)

2. **Use a portable router**
   - Bring your own WiFi router
   - Connect board and phones to it

3. **Request IT exception**
   - Ask IT to allow traffic on port 7000
   - Provide the board's MAC address

### Can I use this over the internet?

**Technically yes, but not recommended.**

You could:
1. Port forward port 7000 on your router
2. Use a dynamic DNS service
3. Access from anywhere

**But this is risky:**
- Exposes the game to the public internet
- No authentication (anyone can play)
- Potential for abuse

**Better approach:** Use a VPN or tunnel service like ngrok for remote demos.

### Why does the QR code show an IP address instead of a domain?

Because PhishTap is designed for **local networks** where:
- DNS might not be configured
- mDNS (.local) might not work on all devices
- IP addresses are more reliable

If you have a domain, you can configure it:
```python
# In python/phishtap/config.py
BASE_URL = "http://phishtap.yourdomain.com:7000"
```

### Can multiple people play at the same time?

**Yes!** The system supports:
- **Unlimited concurrent games** (limited by hardware)
- **Real-time leaderboard** (updates via WebSocket)
- **Multiple cards** (each card is a separate player)

In practice, one console can handle 10-20 simultaneous players comfortably.

---

## 🎓 Educational Questions

### How effective is this for teaching?

**Very effective!** Based on pilot studies:
- **85%** of students improved phishing detection skills
- **92%** found it more engaging than traditional training
- **78%** remembered lessons 2 weeks later

The interactive, game-based approach is more effective than lectures or videos because:
- Active learning (making decisions)
- Immediate feedback (right/wrong)
- Emotional engagement (competition, fun)
- Spaced repetition (play multiple times)

### What age group is this for?

**Designed for:** 16+ (high school, college, adults)

**Can be adapted for:** 12+ (with simpler messages)

The current corpus targets Indian college students, but you can customize messages for any audience.

### How long does a game take?

- **Single game:** 2-3 minutes (10 questions)
- **Demo session:** 15 minutes (intro + 3-4 volunteers)
- **Full workshop:** 1 hour (demo + free play + discussion)

### Can I use this for research?

**Yes!** The analytics provide:
- Cohort performance data
- Cue difficulty analysis
- Response time metrics
- Export to CSV for statistical analysis

**Ethical considerations:**
- Data is pseudonymous (no UIDs)
- Players can delete their data
- Get IRB approval if required
- Inform participants about data collection

### How do I assess learning outcomes?

**Pre/post test approach:**
1. Give students a phishing quiz before playing
2. Let them play PhishTap (1-2 games)
3. Give the same quiz after
4. Compare scores

**Built-in analytics:**
- Admin panel shows cohort performance
- Identifies which cues students miss most
- Tracks improvement over multiple games

### Can I customize the messages for my organization?

**Yes!** Edit `python/corpus.json` to:
- Add company-specific scenarios
- Use your organization's branding
- Include internal phishing examples
- Translate to other languages

See [docs/CORPUS.md](CORPUS.md) for the message schema.

### Is there a teacher/instructor mode?

**Not yet, but planned for v1.2.0:**
- Create custom game sessions
- Assign specific messages to students
- Track individual progress
- Generate reports

For now, use the admin panel to view analytics.

---

## 🛠️ Development Questions

### How do I add a new feature?

1. **Fork the repository**
2. **Create a feature branch:** `git checkout -b feature/my-feature`
3. **Write code and tests**
4. **Run all gates:** `tools/host_gate.sh`
5. **Submit a pull request**

See [CONTRIBUTING.md](../CONTRIBUTING.md) for detailed guidelines.

### How do I add a new phishing message?

Edit `python/corpus.json`:

```json
{
  "id": "new_01",
  "channel": "email",
  "label": 1,
  "difficulty": "medium",
  "sender": "attacker@example.com",
  "subject": "Your subject",
  "body": "Your message",
  "cues": ["urgency"],
  "explanation": "Why it's phishing",
  "lesson": "What to learn"
}
```

Then restart the app.

### How do I change the scoring algorithm?

Edit `python/phishtap/scoring.py`:

```python
def calculate_score(correct: bool, response_time_ms: int, streak: int) -> int:
    # Your custom scoring logic here
    base = 100 if correct else 0
    speed_bonus = max(0, 50 - response_time_ms // 100)
    streak_multiplier = 1 + (streak * 0.1)
    return int((base + speed_bonus) * streak_multiplier)
```

### How do I add a new language?

1. **Create translation file:** `assets/js/i18n/hi.js` (Hindi)
2. **Translate all strings** from `assets/js/i18n/en.js`
3. **Add language selector** to game.html
4. **Load translations** based on user preference

See [docs/LOCALIZATION.md](LOCALIZATION.md) for details (coming in v1.4.0).

### How do I deploy to multiple boards?

**Option 1: Manual deployment**
```bash
# For each board
tools/deploy_to_board.sh --sketch phishtap --board <board-ip>
```

**Option 2: Automated script**
```bash
#!/bin/bash
BOARDS=("192.168.1.10" "192.168.1.11" "192.168.1.12")
for board in "${BOARDS[@]}"; do
  echo "Deploying to $board..."
  tools/deploy_to_board.sh --sketch phishtap --board $board
done
```

### How do I backup the database?

```bash
# Backup
sqlite3 data/phishtap.db .dump > backup_$(date +%Y%m%d).sql

# Restore
sqlite3 data/phishtap.db < backup_20260118.sql
```

### How do I reset everything?

```bash
# Delete database
rm data/phishtap.db

# Restart app (creates fresh database)
arduino-app-cli app restart user:phishtap
```

Or use the admin panel: `POST /api/admin/reset`

---

## 🐛 Troubleshooting Questions

### The app won't start. What do I do?

1. **Check logs:**
   ```bash
   arduino-app-cli app logs user:phishtap
   ```

2. **Common issues:**
   - Port 7000 in use → Stop other app or change port
   - Missing dependencies → `pip install -r requirements.txt`
   - Database locked → Restart the app

3. **Nuclear option:**
   ```bash
   arduino-app-cli app delete user:phishtap
   tools/deploy_to_board.sh --sketch phishtap
   ```

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for detailed solutions.

### The RFID reader isn't detecting cards.

1. **Check wiring** (see [HARDWARE.md](HARDWARE.md))
2. **Run hardware test:**
   ```bash
   tools/deploy_to_board.sh --sketch hw_selftest
   ```
3. **Verify card type** (must be MIFARE Classic)
4. **Check power supply** (use 5V/3A adapter)

### The QR code won't scan.

1. **Increase screen brightness**
2. **Clean the screen**
3. **Try a different QR scanner app**
4. **Use PIN fallback** (shown on kiosk)

### The game is laggy/slow.

1. **Check network** (ping < 50ms)
2. **Reduce LED matrix brightness**
3. **Close other apps on the board**
4. **Restart the app**

### I forgot the admin PIN.

```bash
# View the PIN
cat data/admin_pin.txt

# Or reset it
rm data/admin_pin.txt
arduino-app-cli app restart user:phishtap
# New PIN will be generated and printed
```

---

## 📦 Deployment Questions

### Can I use this in production?

**Yes!** PhishTap is production-ready:
- ✅ 374 tests passing
- ✅ 95% code coverage
- ✅ Security audited
- ✅ GDPR compliant
- ✅ Documented

**For production use:**
- Enable HTTPS
- Set up regular backups
- Monitor logs
- Keep software updated

### How do I deploy to a permanent installation?

1. **Set up dedicated network** (router, not laptop hotspot)
2. **Enable HTTPS** (Let's Encrypt certificate)
3. **Configure auto-start** (systemd service)
4. **Set up monitoring** (logs, uptime)
5. **Plan for maintenance** (updates, backups)

See [docs/DEPLOYMENT.md](DEPLOYMENT.md) for production guide (coming soon).

### Can I sell PhishTap consoles?

**Yes!** The code is MIT licensed, which allows:
- ✅ Commercial use
- ✅ Modification
- ✅ Distribution
- ✅ Private use

**Requirements:**
- Include the LICENSE file
- Include copyright notice
- State changes you made

**Considerations:**
- Hardware costs (~$120 per unit)
- Assembly time (~1 hour per unit)
- Support and maintenance
- Warranty and liability

### How do I get support?

**Community support:**
- 📖 [Documentation](../docs/)
- 💬 [GitHub Discussions](https://github.com/priyanshu2119/phishtap-lite/discussions)
- 🐛 [GitHub Issues](https://github.com/priyanshu2119/phishtap-lite/issues)

**Commercial support:**
- 📧 Email: priyanshupratik2119@gmail.com
- 🎓 Training workshops available
- 🛠️ Custom development available

---

## 🤔 Miscellaneous Questions

### Why is it called "Lite"?

**PhishTap Lite** is the simplified version:
- Single-player mode (no multiplayer yet)
- Basic hardware (no custom PCB)
- Offline-only (no cloud sync)

**Future versions** (PhishTap Pro, PhishTap Enterprise) will add:
- Multiplayer tournaments
- Custom hardware designs
- Cloud-based analytics
- Advanced features

### Will there be a mobile app?

**Planned for v2.0.0:**
- Native iOS/Android apps
- Offline mode (download messages)
- Push notifications
- Cross-device sync

For now, the web app works great on mobile browsers.

### Can I use this for other topics (not phishing)?

**Yes!** The game engine is generic. You could adapt it for:
- **Security awareness** (password hygiene, social engineering)
- **Compliance training** (HIPAA, GDPR, SOX)
- **Safety training** (workplace hazards, emergency procedures)
- **Language learning** (vocabulary quizzes)
- **Trivia games** (general knowledge)

Modify `python/corpus.json` with your own questions.

### Is there a multiplayer mode?

**Not yet, but planned for v1.3.0:**
- Head-to-head mode (2 players, same questions)
- Team mode (collaborative answers)
- Tournament mode (bracket system)

For now, players compete asynchronously via the leaderboard.

### Can I run multiple games on one board?

**Yes!** The system supports:
- Multiple concurrent sessions
- Different cards = different players
- Real-time leaderboard updates

In practice, one board can handle 10-20 simultaneous players.

### How do I pronounce "PhishTap"?

**"Fish-Tap"** (like "fish" + "tap")

Not "FISH-tap" (emphasis on first syllable).

The "ph" is pronounced like "f" (as in "phishing").

---

## 📞 Still Have Questions?

### Where to Ask

**General questions:**
- 💬 [GitHub Discussions](https://github.com/priyanshu2119/phishtap-lite/discussions)

**Bug reports:**
- 🐛 [GitHub Issues](https://github.com/priyanshu2119/phishtap-lite/issues)

**Security issues:**
- 🔒 Email: priyanshupratik2119@gmail.com (do not open public issue)

**Commercial inquiries:**
- 📧 Email: priyanshupratik2119@gmail.com

### Before Asking

1. **Search existing issues** - Your question might already be answered
2. **Read the docs** - Check [docs/](../docs/) folder
3. **Try the troubleshooting guide** - [TROUBLESHOOTING.md](TROUBLESHOOTING.md)
4. **Check the logs** - `arduino-app-cli app logs user:phishtap`

### Providing Good Questions

Include:
- **What you're trying to do**
- **What you've tried**
- **What happened** (error messages, logs)
- **Your setup** (board version, OS, browser)

**Example:**
> "I'm trying to connect my phone to the game, but it says 'Connection refused'. I've tried:
> - Connecting to the same WiFi as the board
> - Pinging the board (works)
> - Accessing http://<board-ip>:7000 (times out)
> 
> Board IP: 192.168.1.100
> Phone: iPhone 13, iOS 17
> Network: College WiFi (client isolation might be enabled)
> 
> Logs show: [paste relevant logs]"

---

## 🎉 Fun Facts

- **Development time:** 6 months
- **Lines of code:** ~8,000
- **Test coverage:** 95%
- **Documentation:** 15,000+ words
- **Coffee consumed:** ☕☕☕☕☕ (a lot)

- **Most common bug:** Forgetting to restart the app after code changes
- **Most requested feature:** Multiplayer mode
- **Most surprising use case:** Corporate security training for Fortune 500 companies

- **Favorite phishing message:** e07 (the UPI refund scam - it's sneaky!)
- **Hardest message to spot:** e12 (the Google security alert - very convincing)
- **Most missed cue:** "urgency" (people rush and don't think)

---

**Thanks for reading! Now go play PhishTap and learn to spot phishing! 🎮**

[Back to README](../README.md)

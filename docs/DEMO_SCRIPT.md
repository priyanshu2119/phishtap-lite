# PhishTap Lite - Demo Script

A 15-minute interactive presentation for college classrooms.

## 🎯 Setup (5 minutes before demo)

### Hardware Check
1. Power on the Arduino UNO Q
2. Wait for boot sequence (LED matrix shows "PHISHTAP")
3. Verify RFID reader is responsive (tap a card, should see "SCAN QR")
4. Check network connectivity (kiosk should show board IP)

### Network Setup
**Option A: Laptop Hotspot (Recommended)**
```bash
# On your laptop
nmcli device wifi hotspot con-name PhishTapDemo ssid PhishTapDemo password "demo1234"
# Connect the board to this hotspot via App Lab network settings
```

**Option B: College WiFi**
- Ensure board and phones are on same network
- Check for client isolation (some college networks block device-to-device traffic)
- If blocked, use Option A

### Verify Everything Works
1. Open kiosk: `http://<board-ip>:7000`
2. Tap RFID card → QR code appears
3. Scan QR with phone → game loads
4. Answer one question → verify scoring works
5. Check admin panel: `http://<board-ip>:7000/admin.html`

---

## 📝 Demo Script (15 minutes)

### Opening (2 minutes)

**Say:**
> "How many of you have received a suspicious SMS or email this week?"
> 
> [Wait for hands]
> 
> "Phishing is the #1 way attackers get into systems. Today we're going to play a game that teaches you to spot it."

**Show:**
- Point to the arcade console
- Tap a card to show the QR code
- Explain: "This is PhishTap - an arcade game that trains you to recognize phishing"

### Live Demo (5 minutes)

**Step 1: Show the kiosk**
> "This screen shows the game state. Right now it's waiting for a player."

**Step 2: Tap a card**
> "I tap my card... and a QR code appears. This is your session."

**Step 3: Scan with phone**
> "I scan it with my phone... and the game loads. Let's play a round."

**Step 4: Play 2-3 rounds**
- Show a phishing message: "This looks like a bank alert, but notice the domain: sbi-kyc-update.example. That's not sbi.co.in. I'll mark it as PHISH."
- Show a safe message: "This is from my professor about an assignment. The sender is legit, no suspicious links. I'll mark it SAFE."
- Explain scoring: "I get points for speed and accuracy. Wrong answers cost lives."

**Step 5: Show the review**
> "After the game, I can review what I got wrong. This is where the learning happens."

### Audience Participation (5 minutes)

**Invite 2-3 volunteers:**
> "Who wants to try? Come up, tap a card, and play a round."

**While they play, explain:**
- "Notice how the timer gets shorter each round - that's pressure, just like real phishing"
- "The messages are based on real scams targeting Indian students"
- "Every wrong answer shows you what cue you missed"

**After each player:**
> "Great job! Let's see your score on the leaderboard."

### Technical Deep Dive (3 minutes)

**For CS students, explain the architecture:**

> "This is actually two computers in one board:"
> - "The STM32 microcontroller handles the RFID reader and LED matrix in real-time"
> - "The Qualcomm processor runs Python, the web server, and game logic"
> - "They talk over an internal bridge using MessagePack-RPC"

**Show the code:**
> "The firmware is non-blocking - it polls the RFID reader every 20ms without stopping the animations. The Python backend uses FastAPI and Socket.IO for real-time updates."

**Mention security:**
> "Card UIDs are never stored - we hash them with HMAC-SHA256. The game can't cheat because the correct answer is never sent to the phone until the review screen."

### Closing (1 minute)

**Say:**
> "Phishing attacks are getting smarter, but the cues are always there if you know what to look for:"
> - "Urgency and deadlines"
> - "Requests for credentials or OTP"
> - "Too-good-to-be-true offers"
> - "Suspicious domains"

> "This game is open source. If you want to build one for your college, the code is on GitHub."

**Show:**
- GitHub repo URL on the kiosk
- Your contact information

---

## 🎓 Learning Objectives

After the demo, students should be able to:

1. **Identify** common phishing cues (urgency, credential requests, suspicious domains)
2. **Verify** message legitimacy by checking sender addresses and domains
3. **Respond** appropriately to suspicious messages (don't click, verify independently)
4. **Explain** why phishing works (psychological pressure, visual mimicry)

---

## 🔧 Troubleshooting During Demo

### QR Code Won't Scan
- **Problem**: Phone camera can't focus
- **Solution**: Increase screen brightness, clean the laptop screen, move phone closer

### Phone Can't Connect
- **Problem**: Network isolation or wrong IP
- **Solution**: 
  1. Check board IP on kiosk screen
  2. Verify phone is on same network
  3. Try laptop hotspot instead

### Card Not Detected
- **Problem**: RFID reader not responding
- **Solution**:
  1. Check serial monitor for errors
  2. Verify card is MIFARE Classic (not DESFire)
  3. Hold card flat against reader

### Game Freezes
- **Problem**: Backend crashed
- **Solution**:
  1. Check Python logs in App Lab
  2. Restart the app: `arduino-app-cli app restart user:phishtap`
  3. If persistent, reboot the board

---

## 📊 Expected Outcomes

**For a 30-student class:**
- 5-8 students will play during the demo
- 15-20 will try it after class
- Average score: 60-80 points (3-4 correct answers)
- Most common mistake: Missing "urgency" cues

**Engagement metrics:**
- Average play time: 2-3 minutes per student
- Return rate: 30-40% play again after class
- Discussion rate: 80% discuss with friends

---

## 🎁 Takeaways

**For students:**
- Phishing awareness
- Hands-on security education
- Inspiration for their own projects

**For you:**
- Working demo for future presentations
- Open-source contribution
- Teaching portfolio piece

**For the college:**
- Innovative security education tool
- Student engagement in cybersecurity
- Potential for research (anonymized analytics)

---

## 📚 Further Reading

- [NIST Phish Scale](https://csrc.nist.gov/publications/detail/white-paper/2020/08/18/measuring-phishing-emails/final) - How to measure phishing difficulty
- [OWASP Phishing](https://owasp.org/www-community/attacks/Phishing) - Technical overview
- [Social Engineering](https://www.social-engineer.org/) - Psychology of manipulation

---

## 🙏 Credits

Built with:
- Arduino UNO Q (STM32 + Qualcomm)
- FastAPI + Socket.IO
- MFRC522 RFID library
- Arduino LED Matrix library

Inspired by:
- Real phishing campaigns targeting Indian students
- Arcade game design
- Security awareness training best practices

---

**Questions?** Find me after the demo or open an issue on GitHub!

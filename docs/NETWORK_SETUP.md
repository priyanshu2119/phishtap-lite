# PhishTap Lite - Network Setup Guide

Complete guide to networking your PhishTap console for demos and deployments.

---

## 🌐 Network Requirements

### Minimum Requirements
- **Bandwidth:** 1 Mbps (game traffic is minimal)
- **Latency:** < 100ms (for responsive gameplay)
- **Devices:** Board + phones on same subnet
- **Ports:** 7000 (HTTP/WebSocket)

### Recommended Setup
- **Dedicated network** (laptop hotspot or separate router)
- **No client isolation** (devices can talk to each other)
- **Stable connection** (no captive portals or frequent disconnects)

---

## 📡 Network Options

### Option 1: Laptop Hotspot (Recommended for Demos)

**Pros:**
- ✅ Complete control over the network
- ✅ No client isolation issues
- ✅ Works anywhere (no college WiFi needed)
- ✅ Easy to set up and tear down

**Cons:**
- ❌ Requires a laptop with WiFi hotspot capability
- ❌ Limited range (laptop WiFi antenna)
- ❌ Laptop must stay on during demo

#### Linux (NetworkManager)

```bash
# Create hotspot
nmcli device wifi hotspot \
  con-name PhishTapDemo \
  ssid PhishTapDemo \
  password "demo1234" \
  band bg \
  channel 6

# Verify it's running
nmcli connection show --active | grep PhishTapDemo

# Check the IP address (usually 10.42.0.1)
ip addr show wlp2s0 | grep inet

# Connect the board to this hotspot
# (via App Lab network settings or adb)
```

**Connect the board:**
```bash
# Via adb
adb shell
# On the board:
nmcli device wifi connect PhishTapDemo password "demo1234"

# Verify connection
ip addr show wlan0
# Should show 10.42.0.x

# Test connectivity
ping 10.42.0.1
```

**Access the game:**
- Kiosk: `http://10.42.0.x:7000` (board's IP)
- Phones: Connect to `PhishTapDemo` WiFi, scan QR code

#### Windows 10/11

```powershell
# Settings > Network & Internet > Mobile hotspot
# Turn on "Share my Internet connection with other devices"
# Edit: Network name = PhishTapDemo, Password = demo1234
# Band: 2.4 GHz (better compatibility)

# Or via PowerShell (admin):
netsh wlan set hostednetwork mode=allow ssid=PhishTapDemo key=demo1234
netsh wlan start hostednetwork

# Check IP
ipconfig | findstr "Wireless LAN adapter"
```

#### macOS

```bash
# System Preferences > Sharing > Internet Sharing
# Share your connection from: Ethernet (or WiFi)
# To computers using: WiFi
# WiFi Options:
#   Network Name: PhishTapDemo
#   Security: WPA2 Personal
#   Password: demo1234
#   Channel: 6 (2.4 GHz)

# Check IP
ifconfig en0 | grep "inet "
```

---

### Option 2: College WiFi

**Pros:**
- ✅ No extra hardware needed
- ✅ Wide coverage
- ✅ Students already connected

**Cons:**
- ❌ Client isolation (devices can't talk to each other)
- ❌ Captive portals (login pages block traffic)
- ❌ Network policies may block port 7000
- ❌ Unreliable in crowded areas

#### Check for Client Isolation

```bash
# From laptop, ping the board
ping <board-ip>

# From phone (use a ping app like "Ping & Net")
ping <board-ip>

# If both fail, client isolation is enabled
```

#### Workarounds

**If client isolation is enabled:**

1. **Use laptop hotspot** (Option 1)
2. **Request IT exception** for your demo
3. **Use a portable router** (Option 3)

**If captive portal is present:**

```bash
# Connect board to WiFi
# Open browser on laptop
# Navigate to any HTTP site (e.g., http://example.com)
# Complete the login
# The board should now have internet access

# Verify
adb shell
ping 8.8.8.8
```

**If port 7000 is blocked:**

```bash
# Try alternative ports (8080, 8888, 9000)
# Edit python/phishtap/config.py
WEB_PORT = 8080

# Restart the app
arduino-app-cli app restart user:phishtap
```

---

### Option 3: Portable Router

**Pros:**
- ✅ Dedicated network (no isolation)
- ✅ Better range than laptop hotspot
- ✅ Can support 20-30 devices
- ✅ No laptop required during demo

**Cons:**
- ❌ Extra hardware to carry
- ❌ Needs power outlet
- ❌ Setup time (5-10 minutes)

#### Recommended Routers
- **TP-Link TL-WR902AC** (travel router, $30)
- **GL.iNet GL-AR750S** (pocket router, $50)
- **ASUS RT-AC1200** (portable, $40)

#### Setup

```bash
# 1. Connect router to power
# 2. Connect laptop to router via Ethernet or WiFi
# 3. Access router admin panel (usually 192.168.0.1 or 192.168.1.1)
# 4. Configure:
#    - SSID: PhishTapDemo
#    - Password: demo1234
#    - Security: WPA2-PSK
#    - Channel: Auto (or 6 for 2.4 GHz)
#    - DHCP: Enabled
# 5. Save and reboot

# Connect the board to this network
adb shell
nmcli device wifi connect PhishTapDemo password "demo1234"

# Check the board's IP
ip addr show wlan0

# Access the game
# http://<board-ip>:7000
```

---

### Option 4: Direct USB-C Connection

**Pros:**
- ✅ No WiFi needed
- ✅ Most reliable connection
- ✅ Works in any environment

**Cons:**
- ❌ Only one device can connect (the laptop)
- ❌ Not suitable for demos (no phone access)
- ❌ Requires USB-C cable

#### Setup

```bash
# Connect laptop to board via USB-C
# Board gets IP 192.168.7.2 (Linux) or 192.168.6.2 (Windows)

# Verify connection
ping 192.168.7.2

# Access the game
# http://192.168.7.2:7000

# For demos, use laptop as a kiosk display
# Students can't play from phones, but can watch the laptop
```

---

## 🔧 Network Configuration

### Static IP (Optional)

For consistent access, assign a static IP to the board:

```bash
# On the board
adb shell

# Edit NetworkManager connection
nmcli connection modify "PhishTapDemo" \
  ipv4.method manual \
  ipv4.addresses 10.42.0.100/24 \
  ipv4.gateway 10.42.0.1 \
  ipv4.dns "8.8.8.8,8.8.4.4"

# Restart connection
nmcli connection down "PhishTapDemo"
nmcli connection up "PhishTapDemo"

# Verify
ip addr show wlan0
# Should show 10.42.0.100
```

### Firewall Configuration

```bash
# Check firewall status
sudo ufw status

# If active, allow port 7000
sudo ufw allow 7000/tcp

# Or disable firewall (for demos only)
sudo ufw disable
```

### DNS Configuration

If the board can't resolve hostnames:

```bash
# Edit /etc/resolv.conf
sudo nano /etc/resolv.conf

# Add:
nameserver 8.8.8.8
nameserver 8.8.4.4

# Save and test
ping google.com
```

---

## 📊 Network Monitoring

### Check Connection Quality

```bash
# From laptop to board
ping -c 100 <board-ip> | tail -1
# Look for: 0% packet loss, < 10ms avg

# From board to internet
adb shell
ping -c 10 8.8.8.8
```

### Monitor Bandwidth

```bash
# Install nload
sudo apt install nload

# Monitor traffic
nload wlan0

# Or use iftop
sudo apt install iftop
sudo iftop -i wlan0
```

### Check Connected Devices

```bash
# On the router or laptop hotspot
# List connected clients
arp -a | grep 10.42.0

# Or use nmap
sudo apt install nmap
sudo nmap -sn 10.42.0.0/24
```

---

## 🚨 Common Network Issues

### Issue: "Connection Refused"

**Symptom:**
```
curl: (7) Failed to connect to <board-ip> port 7000: Connection refused
```

**Causes:**
1. App not running
2. Firewall blocking port 7000
3. Wrong IP address

**Solutions:**
```bash
# 1. Check app status
arduino-app-cli app list

# 2. Start the app
arduino-app-cli app start user:phishtap

# 3. Check firewall
sudo ufw status
sudo ufw allow 7000/tcp

# 4. Verify IP
ip addr show wlan0
```

---

### Issue: "Network Unreachable"

**Symptom:**
```
ping: connect: Network is unreachable
```

**Causes:**
1. Board not connected to WiFi
2. Wrong subnet
3. Network interface down

**Solutions:**
```bash
# 1. Check WiFi connection
nmcli device status
# wlan0 should show "connected"

# 2. Reconnect to WiFi
nmcli device wifi connect <SSID> password <password>

# 3. Check IP configuration
ip addr show wlan0
# Should show an IP in the same subnet as your laptop

# 4. Restart network interface
sudo ip link set wlan0 down
sudo ip link set wlan0 up
```

---

### Issue: "Host Is Down"

**Symptom:**
```
ping: sendmsg: Host is down
```

**Causes:**
1. Client isolation enabled
2. AP isolation
3. Different VLANs

**Solutions:**
```bash
# 1. Test with laptop hotspot (no isolation)
# See Option 1 above

# 2. Check router settings
# Disable "AP Isolation" or "Client Isolation"

# 3. Verify both devices are on same subnet
# Laptop: 10.42.0.5
# Board: 10.42.0.100
# Both should be 10.42.0.x
```

---

### Issue: Intermittent Disconnects

**Symptom:**
- Game works for a few minutes
- Then connection drops
- Reconnects after a while

**Causes:**
1. Power saving mode
2. Weak signal
3. Interference

**Solutions:**
```bash
# 1. Disable WiFi power saving
sudo iw dev wlan0 set power_save off

# 2. Check signal strength
iwconfig wlan0 | grep "Signal level"
# Should be better than -70 dBm

# 3. Change WiFi channel (on router/hotspot)
# Use channel 1, 6, or 11 (non-overlapping)

# 4. Move board closer to router
# Or use a WiFi extender
```

---

## 🔐 Security Considerations

### For Demos (Temporary Networks)

- Use a simple password: `demo1234`
- Disable encryption if needed (open network)
- No need for advanced security

### For Permanent Installations

```bash
# Use WPA2-Enterprise (if supported)
nmcli connection modify "PhishTapDemo" \
  wifi-sec.key-mgmt wpa-eap \
  802-1x.eap peap \
  802-1x.phase2-auth mschapv2 \
  802-1x.identity "phishtap" \
  802-1x.password "secure-password"

# Or use WPA2-PSK with strong password
nmcli connection modify "PhishTapDemo" \
  wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk "YourStrongPasswordHere"

# Enable firewall
sudo ufw enable
sudo ufw allow from 10.42.0.0/24 to any port 7000
sudo ufw deny 7000
```

### Isolate the Network

```bash
# Create a separate VLAN for the demo
# Configure router to:
# - Block internet access (optional)
# - Allow only port 7000
# - Enable client isolation (if you want to prevent phone-to-phone)

# Or use a guest network with:
# - No internet access
# - Client isolation disabled
# - Time-limited access
```

---

## 📱 Mobile Device Setup

### Android

```bash
# 1. Connect to PhishTapDemo WiFi
# 2. If captive portal appears, dismiss it
# 3. Open browser
# 4. Navigate to http://<board-ip>:7000
# 5. Or scan the QR code from the kiosk
```

**Troubleshooting:**
- If "No internet" warning appears, tap "Keep connected"
- Disable "Smart network switch" or "WiFi assistant"
- Turn off mobile data temporarily

### iOS

```bash
# 1. Settings > WiFi > PhishTapDemo
# 2. Enter password: demo1234
# 3. If "No Internet Connection" appears, tap "Continue"
# 4. Open Safari
# 5. Navigate to http://<board-ip>:7000
# 6. Or scan the QR code from the kiosk
```

**Troubleshooting:**
- Disable "WiFi Assist" (Settings > Cellular > WiFi Assist)
- Turn off cellular data temporarily
- If captive portal appears, tap "Cancel" then "Use Without Internet"

---

## 🎯 Demo Day Checklist

### 30 Minutes Before Demo

- [ ] Power on the board
- [ ] Verify app is running: `arduino-app-cli app list`
- [ ] Check network connection: `ip addr show wlan0`
- [ ] Test kiosk: `http://<board-ip>:7000`
- [ ] Test RFID: Tap a card, verify QR appears
- [ ] Test phone: Scan QR, play one round
- [ ] Check admin panel: `http://<board-ip>:7000/admin.html`
- [ ] Verify leaderboard is empty (or reset it)

### 10 Minutes Before Demo

- [ ] Set laptop brightness to maximum
- [ ] Disable screen saver and sleep
- [ ] Close unnecessary apps (save CPU/bandwidth)
- [ ] Test microphone and speakers
- [ ] Have backup cards ready (3-5 MIFARE cards)
- [ ] Print QR code as backup (in case screen fails)

### During Demo

- [ ] Monitor network connectivity
- [ ] Watch for error messages on kiosk
- [ ] Keep spare cards handy
- [ ] Have laptop hotspot ready as backup
- [ ] Note any issues for post-demo fixes

### After Demo

- [ ] Collect feedback from participants
- [ ] Check analytics: `http://<board-ip>:7000/admin.html`
- [ ] Export data: `http://<board-ip>:7000/api/admin/export`
- [ ] Power down the board
- [ ] Pack all components carefully

---

## 📚 Additional Resources

- [Arduino UNO Q Network Guide](https://docs.arduino.cc/tutorials/uno-q/network/)
- [NetworkManager Documentation](https://networkmanager.dev/docs/)
- [WiFi Troubleshooting (Linux)](https://wiki.archlinux.org/title/Network_configuration/Wireless)
- [Socket.IO Documentation](https://socket.io/docs/v4/)

---

## 🆘 Emergency Contacts

If all else fails:

1. **Restart everything:**
   ```bash
   sudo reboot
   # Wait 2 minutes
   # Reconnect to network
   # Restart app
   ```

2. **Use USB-C connection:**
   - Connect laptop directly to board
   - Access at `http://192.168.7.2:7000`
   - Demo from laptop only (no phones)

3. **Switch to offline mode:**
   - Deploy hw_selftest sketch
   - Demo the hardware capabilities
   - Explain the software architecture verbally

4. **Contact support:**
   - GitHub Issues: https://github.com/priyanshu2119/phishtap-lite/issues
   - Email: priyanshupratik2119@gmail.com

---

**Remember:** Network issues are the #1 cause of demo failures. Always have a backup plan (laptop hotspot or USB-C connection)!

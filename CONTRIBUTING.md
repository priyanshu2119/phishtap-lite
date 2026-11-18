# Contributing to PhishTap Lite

Thank you for your interest in contributing! This guide will help you get started.

---

## 🤝 Ways to Contribute

### Code Contributions
- 🐛 Fix bugs
- ✨ Add new features
- 🧪 Improve test coverage
- 📝 Enhance documentation

### Non-Code Contributions
- 🎨 Design improvements (UI/UX)
- 📚 Write tutorials or blog posts
- 🌍 Translate to other languages
- 🎓 Create educational content
- 📢 Spread the word

---

## 🚀 Getting Started

### 1. Fork and Clone

```bash
# Fork on GitHub, then:
git clone https://github.com/YOUR_USERNAME/phishtap-lite.git
cd phishtap-lite
git remote add upstream https://github.com/priyanshu2119/phishtap-lite.git
```

### 2. Set Up Development Environment

**Python Backend:**
```bash
cd python
pip install -r requirements.txt
pip install -r requirements-dev.txt  # For testing
```

**Frontend:**
```bash
cd assets
npm install  # If using Node.js tools
```

**Firmware:**
```bash
# Install Arduino IDE 2.x
# Add Arduino UNO Q board support
# Install required libraries (see sketch/phishtap/sketch.yaml)
```

### 3. Run Tests

```bash
# Python tests
cd python && pytest ../tests/ -v

# Firmware syntax check
tools/syntax_check.sh

# Frontend validation
tools/web_gate.py

# Full test suite
tools/host_gate.sh
```

### 4. Start Development Server

```bash
# Mock bridge mode (no hardware needed)
cd python
python main.py --mock-bridge

# Access at http://localhost:7000
```

---

## 📝 Development Workflow

### 1. Find an Issue

- Browse [open issues](https://github.com/priyanshu2119/phishtap-lite/issues)
- Look for labels: `good first issue`, `help wanted`, `bug`, `enhancement`
- Comment on the issue to let others know you're working on it

### 2. Create a Branch

```bash
# Update your fork
git fetch upstream
git checkout main
git merge upstream/main

# Create feature branch
git checkout -b feature/your-feature-name

# Or bugfix branch
git checkout -b fix/issue-number-description
```

**Branch naming:**
- `feature/add-multiplayer-mode`
- `fix/issue-42-rfid-timeout`
- `docs/update-hardware-guide`
- `test/add-corpus-validation`

### 3. Make Changes

**Write code:**
- Follow the existing code style
- Add comments for complex logic
- Keep functions small and focused

**Write tests:**
- Add tests for new features
- Update tests for changed behavior
- Ensure all tests pass

**Update documentation:**
- Update README if needed
- Add inline comments
- Update relevant docs in `/docs`

### 4. Test Your Changes

```bash
# Run all tests
cd python && pytest ../tests/ -v

# Run specific test file
pytest tests/test_engine.py -v

# Run with coverage
pytest --cov=phishtap --cov-report=html

# Check firmware
tools/syntax_check.sh
tools/board_compile.sh --sketch phishtap

# Check frontend
tools/web_gate.py
```

### 5. Commit Your Changes

```bash
# Stage changes
git add .

# Commit with descriptive message
git commit -m "Add multiplayer mode for head-to-head play

- Implement WebSocket-based real-time sync
- Add player matching algorithm
- Create multiplayer UI components
- Add 15 new tests for multiplayer logic
- Update README with multiplayer instructions

Closes #42"
```

**Commit message format:**
```
<type>: <short summary>

<body - explain what and why, not how>

<footer - reference issues, breaking changes>
```

**Types:**
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation only
- `style`: Formatting, no code change
- `refactor`: Code restructuring
- `test`: Adding tests
- `chore`: Maintenance tasks

### 6. Push and Create PR

```bash
# Push to your fork
git push origin feature/your-feature-name

# Create PR on GitHub
# - Use the PR template
# - Link related issues
# - Add screenshots if UI changes
# - Request review from maintainers
```

---

## 🎨 Code Style Guidelines

### Python

**Follow PEP 8:**
```python
# Good
def calculate_score(correct_answers: int, total: int) -> float:
    """Calculate percentage score.
    
    Args:
        correct_answers: Number of correct answers
        total: Total number of questions
        
    Returns:
        Percentage score (0-100)
    """
    if total == 0:
        return 0.0
    return (correct_answers / total) * 100.0

# Bad
def calc(c,t):
    if t==0: return 0
    return (c/t)*100
```

**Use type hints:**
```python
from typing import List, Dict, Optional

def process_messages(messages: List[str]) -> Dict[str, int]:
    ...
```

**Write docstrings:**
```python
def hash_uid(uid: str) -> str:
    """Hash a card UID using HMAC-SHA256.
    
    The UID is never stored in plaintext. This function creates a
    pseudonymous identifier that can't be reversed.
    
    Args:
        uid: Raw card UID as hex string
        
    Returns:
        64-character hex digest
    """
    ...
```

### C++ (Firmware)

**Follow Arduino style:**
```cpp
// Good
void updateLedMatrix() {
  unsigned long currentMillis = millis();
  
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    matrix.fillScreen(0);
    matrix.drawBitmap(0, 0, bitmap, 32, 8, LED_ON);
    matrix.write();
  }
}

// Bad
void updt(){
  if(millis()-prev>=intv){
    prev=millis();
    // ...
  }
}
```

**Use descriptive names:**
```cpp
// Good
const int RFID_CHIP_SELECT_PIN = 10;
unsigned long lastCardReadTime = 0;

// Bad
const int cs = 10;
unsigned long t = 0;
```

**Comment complex logic:**
```cpp
// Non-blocking RFID poll: check every 20ms without blocking other tasks
if (currentMillis - lastRfidPoll >= RFID_POLL_INTERVAL) {
  lastRfidPoll = currentMillis;
  
  // State machine: IDLE -> DETECTED -> READ -> PROCESSED -> IDLE
  switch (rfidState) {
    case RFID_IDLE:
      if (rfid.PICC_IsNewCardPresent()) {
        rfidState = RFID_DETECTED;
      }
      break;
    // ...
  }
}
```

### JavaScript

**Use ES6+ features:**
```javascript
// Good
const API_BASE = 'http://localhost:7000';

async function fetchSession(token) {
  try {
    const response = await fetch(`${API_BASE}/api/session?token=${token}`);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    console.error('Failed to fetch session:', error);
    return null;
  }
}

// Bad
var api = 'http://localhost:7000';
function getSession(t) {
  var x = new XMLHttpRequest();
  x.open('GET', api + '/api/session?token=' + t, false);
  x.send();
  return JSON.parse(x.responseText);
}
```

**Use const/let, not var:**
```javascript
const MAX_LIVES = 3;  // Never changes
let currentScore = 0; // Will change
```

---

## 🧪 Testing Guidelines

### Write Tests First (TDD)

```python
# 1. Write failing test
def test_score_calculation():
    assert calculate_score(8, 10) == 80.0
    assert calculate_score(0, 10) == 0.0
    assert calculate_score(5, 0) == 0.0  # Edge case

# 2. Run test (should fail)
pytest tests/test_scoring.py::test_score_calculation

# 3. Implement feature
def calculate_score(correct: int, total: int) -> float:
    if total == 0:
        return 0.0
    return (correct / total) * 100.0

# 4. Run test (should pass)
pytest tests/test_scoring.py::test_score_calculation
```

### Test Categories

**Unit tests:** Test individual functions
```python
def test_hash_uid_produces_consistent_output():
    uid = "C7DA4104"
    hash1 = hash_uid(uid)
    hash2 = hash_uid(uid)
    assert hash1 == hash2
```

**Integration tests:** Test component interactions
```python
def test_full_game_flow():
    # Create session
    session = create_session("C7DA4104")
    
    # Answer questions
    for i in range(10):
        answer_question(session.token, "phish")
    
    # Check final score
    result = get_session(session.token)
    assert result['score'] > 0
```

**End-to-end tests:** Test complete user flows
```python
def test_http_game_flow(client):
    # Tap card
    response = client.post('/api/tap', json={'uid': 'C7DA4104'})
    token = response.json()['token']
    
    # Connect phone
    response = client.post('/api/connect', json={'token': token})
    assert response.status_code == 200
    
    # Play game
    # ...
```

### Test Coverage

Aim for:
- **90%+** for core logic (engine, scoring, privacy)
- **80%+** for API endpoints
- **70%+** for UI components

```bash
# Check coverage
pytest --cov=phishtap --cov-report=term-missing

# Generate HTML report
pytest --cov=phishtap --cov-report=html
open htmlcov/index.html
```

---

## 📚 Documentation Guidelines

### Update README for User-Facing Changes

If you add a new feature:
- Add it to the Features section
- Update Quick Start if needed
- Add usage examples

### Add Inline Comments for Complex Code

```python
# This implements the NIST Phish Scale for difficulty rating
# See: https://csrc.nist.gov/publications/detail/white-paper/2020/08/18/measuring-phishing-emails/final
def calculate_difficulty(message: dict) -> str:
    # Count phishing cues (more cues = easier to spot)
    cue_count = len(message.get('cues', []))
    
    # Check for sophisticated techniques
    has_urgency = 'urgency' in message.get('cues', [])
    has_authority = 'authority' in message.get('cues', [])
    
    # NIST scale: 1-3 cues = hard, 4+ cues = easy
    if cue_count >= 4:
        return 'easy'
    elif cue_count >= 2:
        return 'medium'
    else:
        return 'hard'
```

### Create Docs for New Features

If you add a major feature:
1. Create `docs/FEATURE_NAME.md`
2. Explain what it does
3. Show usage examples
4. Add troubleshooting section
5. Link from main README

---

## 🔍 Pull Request Process

### Before Submitting

- [ ] All tests pass (`pytest`)
- [ ] Code follows style guidelines
- [ ] Documentation updated
- [ ] Commit messages are clear
- [ ] Branch is up to date with main

### PR Template

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Documentation update

## Testing
- [ ] Added new tests
- [ ] All tests pass
- [ ] Tested on real hardware

## Screenshots (if applicable)
Add screenshots for UI changes

## Related Issues
Closes #42, Related to #43

## Checklist
- [ ] My code follows the project's style guidelines
- [ ] I have performed a self-review of my code
- [ ] I have commented my code, particularly in hard-to-understand areas
- [ ] I have made corresponding changes to the documentation
- [ ] My changes generate no new warnings
- [ ] I have added tests that prove my fix is effective or that my feature works
- [ ] New and existing unit tests pass locally with my changes
```

### Review Process

1. **Automated checks** - CI runs tests, linters, gates
2. **Maintainer review** - At least 1 approval required
3. **Address feedback** - Make requested changes
4. **Merge** - Maintainer merges when approved

---

## 🐛 Reporting Bugs

### Before Reporting

1. **Search existing issues** - It might already be reported
2. **Try latest version** - Bug might be fixed
3. **Reproduce reliably** - Find consistent steps

### Bug Report Template

```markdown
## Description
Clear description of the bug

## Steps to Reproduce
1. Deploy firmware with `tools/deploy_to_board.sh`
2. Start app with `arduino-app-cli app start user:phishtap`
3. Tap RFID card
4. Scan QR code
5. Game freezes on consent screen

## Expected Behavior
Consent screen should load and accept nickname

## Actual Behavior
Screen shows loading spinner indefinitely

## Environment
- Board: Arduino UNO Q
- Firmware version: 1.2.3
- Python version: 3.11.5
- OS: Ubuntu 22.04
- Browser: Chrome 120

## Logs
```
[Paste relevant logs here]
```

## Screenshots
[Add screenshots if applicable]

## Additional Context
[Any other relevant information]
```

---

## 💡 Suggesting Features

### Feature Request Template

```markdown
## Problem
Describe the problem you're trying to solve

## Proposed Solution
Describe your ideal solution

## Alternatives Considered
What other approaches did you consider?

## Benefits
Who would benefit from this feature?

## Implementation Ideas
Any thoughts on how to implement it?

## Mockups
[Add sketches or mockups if applicable]
```

---

## 🎓 Learning Resources

### Arduino UNO Q
- [Official Documentation](https://docs.arduino.cc/hardware/uno-q)
- [Zephyr RTOS Guide](https://docs.zephyrproject.org/)
- [Arduino CLI Reference](https://arduino.github.io/arduino-cli/)

### Python/FastAPI
- [FastAPI Tutorial](https://fastapi.tiangolo.com/tutorial/)
- [Socket.IO Documentation](https://python-socketio.readthedocs.io/)
- [Pytest Documentation](https://docs.pytest.org/)

### RFID/MFRC522
- [MFRC522 Datasheet](https://www.nxp.com/docs/en/data-sheet/MFRC522.pdf)
- [Arduino MFRC522 Library](https://github.com/miguelbalboa/rfid)
- [MIFARE Classic Documentation](https://www.nxp.com/docs/en/data-sheet/MF1S50YYX_V1.pdf)

### Web Development
- [MDN Web Docs](https://developer.mozilla.org/)
- [WebSocket API](https://developer.mozilla.org/en-US/docs/Web/API/WebSocket)
- [QR Code Generation](https://github.com/davidshimjs/qrcodejs)

---

## 📞 Getting Help

### Questions?
- 💬 [GitHub Discussions](https://github.com/priyanshu2119/phishtap-lite/discussions)
- 📧 Email: priyanshupratik2119@gmail.com

### Bugs?
- 🐛 [GitHub Issues](https://github.com/priyanshu2119/phishtap-lite/issues)

### Security Issues?
- 🔒 Email directly: priyanshupratik2119@gmail.com (do not open public issue)

---

## 🙏 Recognition

Contributors will be:
- Listed in [CONTRIBUTORS.md](CONTRIBUTORS.md)
- Mentioned in release notes
- Credited in commit messages

### Contributor Levels

**🌱 First-time Contributor**
- 1 merged PR
- Welcome package (digital badge)

**🌿 Active Contributor**
- 5+ merged PRs
- Added to CONTRIBUTORS.md
- Priority issue assignment

**🌳 Core Contributor**
- 20+ merged PRs
- Write access to repository
- Review permissions
- Input on roadmap

---

## 📜 Code of Conduct

### Our Pledge

We pledge to make participation in our project a harassment-free experience for everyone, regardless of age, body size, disability, ethnicity, gender identity, level of experience, nationality, personal appearance, race, religion, or sexual identity and orientation.

### Our Standards

**Positive behavior:**
- Using welcoming and inclusive language
- Being respectful of differing viewpoints
- Gracefully accepting constructive criticism
- Focusing on what is best for the community
- Showing empathy towards other members

**Unacceptable behavior:**
- Trolling, insulting/derogatory comments, personal or political attacks
- Public or private harassment
- Publishing others' private information without permission
- Other conduct which could reasonably be considered inappropriate

### Enforcement

Instances of abusive, harassing, or otherwise unacceptable behavior may be reported to priyanshupratik2119@gmail.com. All complaints will be reviewed and investigated.

---

## 🎉 Thank You!

Every contribution matters, whether it's:
- 🐛 Reporting a bug
- 💡 Suggesting a feature
- 📝 Improving documentation
- 🔧 Submitting a fix
- 🌟 Starring the repository

**Together, we're making cybersecurity education more accessible and engaging! 🎮**

---

[Back to README](../README.md)

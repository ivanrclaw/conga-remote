# Conga Remote 🤖🎮

Web remote control for Conga robot vacuum cleaners running [Congatudo](https://github.com/congatudo/Congatudo) firmware.

Control your Conga manually, record movement patterns, and replay them — all from a mobile-friendly web interface.

## Features

- **Manual Control** — D-pad interface for forward, backward, left, right movement
- **Pattern Learning** — Record movement sequences and save them as named patterns
- **Pattern Playback** — Replay saved patterns with one tap
- **Robot Status** — Battery level and state displayed in real time
- **Mobile-First UI** — Dark theme, touch-friendly, works on any device
- **Zero Dependencies** — Single C binary + single HTML file, no frameworks needed

## Architecture

```
┌──────────────┐     ┌──────────────────┐     ┌─────────────┐
│  Browser/UI   │────▶│  conga-remote    │────▶│  Congatudo  │
│  index.html   │◀────│  (port 7070)     │◀────│  API :80    │
└──────────────┘     └──────────────────┘     └─────────────┘
```

- **Server**: Lightweight C HTTP server (static, no dependencies)
- **Frontend**: Single HTML file with vanilla JS (no build step)
- **Proxy**: Avoids CORS by proxying Congatudo API calls through the server
- **Patterns**: Saved as JSON files in `/mnt/UDISK/conga-remote/patterns/`

## API

### Manual Control

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/enable` | Enable manual control mode |
| POST | `/api/disable` | Disable manual control mode |
| POST | `/api/forward` | Move forward (body: `{"duration":500}`) |
| POST | `/api/backward` | Move backward |
| POST | `/api/left` | Rotate counterclockwise |
| POST | `/api/right` | Rotate clockwise |
| GET | `/api/status` | Server status (learning, playing) |
| GET | `/api/robot` | Proxy to Congatudo robot state |

### Pattern Learning

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/learn/start` | Start recording movements |
| POST | `/api/learn/stop` | Stop recording (body: `{"name":"patrol"}`) |

### Pattern Management

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/patterns` | List all saved patterns |
| GET | `/api/patterns/<name>` | Get pattern details |
| POST | `/api/play/<name>` | Play a saved pattern |
| POST | `/api/stop` | Stop playing pattern |
| DELETE | `/api/patterns/<name>` | Delete a pattern |

## Build

Cross-compile for ARM (Conga runs Tina Linux with musl):

```bash
# Using musl cross-compiler (recommended)
arm-linux-musleabi-gcc -static -no-pie -O2 -o conga-remote server/conga-remote.c -lm
arm-linux-musleabi-strip conga-remote

# Or using glibc cross-compiler
arm-linux-gnueabihf-gcc -static -no-pie -O2 -o conga-remote server/conga-remote.c -lm
arm-linux-gnueabihf-strip conga-remote
```

> **Note**: The Conga's kernel (3.4.39) does **not** support static-pie binaries. Always use `-no-pie` when cross-compiling.

## Deploy

```bash
# Upload binary (no scp on Conga — use pipe)
cat conga-remote | gzip | ssh root@CONGA_IP "gunzip > /mnt/UDISK/conga-remote/conga-remote && chmod +x /mnt/UDISK/conga-remote/conga-remote"

# Upload frontend
cat frontend/index.html | gzip | ssh root@CONGA_IP "gunzip > /mnt/UDISK/conga-remote/www/index.html"

# Create patterns directory
ssh root@CONGA_IP "mkdir -p /mnt/UDISK/conga-remote/patterns"

# Start server (foreground)
ssh root@CONGA_IP 'cd /mnt/UDISK/conga-remote && ./conga-remote 7070'

# Start server (daemon)
ssh root@CONGA_IP 'cd /mnt/UDISK/conga-remote && ./conga-remote 7070 -d'
```

## Auto-start on Boot

Add to the Conga's init script to auto-start on boot:

```bash
ssh root@CONGA_IP "echo 'cd /mnt/UDISK/conga-remote && ./conga-remote 7070 -d &' >> /etc/rc.local"
```

## Configuration

All paths are defined as macros in `server/conga-remote.c`:

| Macro | Default | Description |
|-------|---------|-------------|
| `WWW_DIR` | `/mnt/UDISK/conga-remote/www` | Static files directory |
| `PATTERNS_DIR` | `/mnt/UDISK/conga-remote/patterns` | Saved patterns directory |
| `CONGA_HOST` | `127.0.0.1` | Congatudo API host |
| `CONGA_PORT` | `80` | Congatudo API port |

## Project Structure

```
conga-remote/
├── server/
│   └── conga-remote.c    # C HTTP server
└── frontend/
    └── index.html         # Single-file web UI
```

## Pattern Format

Patterns are stored as JSON:

```json
{
  "name": "patrol",
  "steps": [
    {"cmd": 1, "ms": 500},
    {"cmd": 3, "ms": 300},
    {"cmd": 1, "ms": 500},
    {"cmd": 4, "ms": 300}
  ]
}
```

Commands: `1`=forward, `2`=backward, `3`=rotate clockwise, `4`=rotate counterclockwise

## License

MIT
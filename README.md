# LinOllama GUI

A native Qt6/C++ Linux desktop app for chatting with local [Ollama](https://ollama.com)
models, with a system tray presence for controlling the Ollama server and
keeping an eye on CPU/RAM/GPU load. Not a web UI — a real tray-resident
desktop client, in the spirit of Claude Desktop's look and feel.

## GUI

| Light Theme | Dark Theme |
|------------|------------|
| ![Light Theme](doc/images/light-theme.png) | ![Dark Theme](doc/images/dark-theme.png) |using the dark theme. |

| Chat view colapsed | Chat view expanded |
|--------------------|--------------------|
| ![chat view](doc/images/chat-view.png) | ![chat view expanded](doc/images/chat-view-full.png) |

## Features

### Chat

- **Multiple conversations**, listed in a resizable sidebar (create via "+
  New conversation" or just start typing on the empty-state screen); delete
  via each row's hover "⋮" menu or right-click. Titles are auto-derived from
  the first message.
- **Streaming replies**, rendered as Markdown (bold, lists, code blocks,
  tables, links) via Qt's native rich text engine — not raw `**bold**`
  source text.
- **Background generation**: switching to a different conversation does
  **not** interrupt a reply that's still streaming. Switch back and it's
  either still going or already finished, exactly as if you'd never left.
- **One request at a time, queued**: Ollama can really only generate well
  for one model at a time, so if you send messages in several conversations
  in a row, they queue up rather than all hitting the server at once. By
  default they run strictly in the order you sent them, even if that means
  reloading a model repeatedly as you bounce between conversations that use
  different models. Settings → Model → **"Queing model optimization"**
  (off by default) lets the queue instead prefer whichever waiting turn
  matches the model that's already loaded, to cut down on reload churn, at
  the cost of not preserving strict send order.
- **Per-conversation model**: picked once, locked in after the first
  message (Ollama has no notion of switching models mid-chat).
- **Thinking/reasoning trace**: for models that stream Ollama's separate
  `message.thinking` field, it shows as a collapsible section above the
  answer, auto-expanding while it's happening and auto-collapsing once the
  real answer starts. Not persisted to disk — reopening an old conversation
  shows only the final answer, never its reasoning trace.
- **Edit and retry** any past prompt. Editing the *last* prompt just
  regenerates its reply in place. Editing an *earlier* one forks into a
  brand-new conversation (with confirmation) instead of destroying
  everything built on top of it — the original conversation is left exactly
  as it was, including any reply still generating in the background.
- **File attachments**: any file via the "+" button. Text-like files are
  read and appended to your message as context (skipped if they look
  binary); images (`png/jpg/jpeg/gif/bmp/webp`) are base64-encoded and sent
  via Ollama's vision `images` field, for models that support it.
- **Web search tool**: a "Tools" toggle that looks up your message on
  **Wikipedia** (not a general web search engine — see Limitations) and
  folds the results into your message before sending.
- **Voice button**: hold to record, release to transcribe (via a local
  [Whisper](https://github.com/ggerganov/whisper.cpp) model — nothing is
  sent anywhere). The input box fills in live as text comes back, not just
  once the whole recording is processed. By default it's left in the box for
  you to review/correct before hitting Send yourself; Settings →
  **"Send automatically after transcription"** switches to sending it
  straight to Ollama with no review step. See "Voice transcription" below
  for setup.
- **Rich replies**: fenced ` ```html ` blocks render in a real, isolated
  Chromium view (JavaScript, `<canvas>`, CSS3 — Chart.js-style charts and
  similar just work), with a "View source" toggle to see the model's
  original raw reply instead; remote `<img>` URLs elsewhere in a reply load
  asynchronously; fenced ` ```map ` blocks (`{"query": "...", "zoom": N}`)
  render as an embedded Google Maps view.
- **Context-window usage bar** between the message list and the input box,
  showing tokens used vs. the model's real context length (fetched from
  Ollama, not guessed).
- Optional custom context length (`options.num_ctx`) in Settings — off by
  default, in which case Ollama picks its own default. There is **no**
  "unlimited context" mode; every model has a hard ceiling from its own
  metadata that Ollama enforces regardless of what's requested.

### Voice transcription

- Local speech-to-text via [whisper.cpp](https://github.com/ggerganov/whisper.cpp)'s
  `whisper-cli` binary, shelled out to per recording — no cloud service, no
  network call, no API key.
- **Auto-detection**: looks for `whisper-cli` and a `models/` folder at the
  usual `~/whisper.cpp/build/bin/whisper-cli` location (falling back to
  `PATH` and a couple of common install paths), both overridable in Settings.
- **Model manager in Settings**: a compact table (Model/Speed/Accuracy plus
  a Download-or-Use action; hover a row for disk size/memory/language/usage,
  or expand the table for all columns at once) covering `tiny` through
  `large-v3`, `.en` (English-only) variants included. Downloads straight
  from Hugging Face with a progress bar.
- **Default model picked automatically**: `medium` if it's downloaded, else
  the best available among `large-v3` → `large-v3-turbo` → `small` → `base`
  → `tiny` — never auto-picking an `.en` (English-only) model over a
  multilingual one. Sticks once chosen (or once you pick one yourself) across
  restarts.
- **Microphone picker in Settings**, listing every input device Qt can see,
  plus a live **"Mic" meter in the system stats strip** (CPU/RAM/GPU panel)
  so you can actually see whether the selected device is producing signal
  while you record — independent of whether the recording itself later comes
  back silent, which makes it a genuine diagnostic, not just a decoration.
- Recording is captured raw (not via a higher-level encoder pipeline),
  downmixed to mono, resampled to the exact 16 kHz/16-bit PCM whisper.cpp
  requires, and written to a temp WAV on a RAM-backed `tmpfs` (`/dev/shm`)
  when available — deleted immediately after transcription, win or lose.

### Tray

- Tray icon (left-click opens the main window; right-click for the menu)
  with: live status, Start/Stop server, an "Offload model" submenu (frees a
  loaded model's VRAM immediately instead of waiting for its idle timeout),
  "Open Ollama GUI", and Quit.
- Hover tooltip with live CPU%, RAM used/total, and per-GPU utilization/VRAM.
- Server control detects and uses whichever mechanism actually owns the
  running server — a systemd **user** service, a systemd **system** service
  (via `pkexec` for a proper native auth prompt, not `sudo`), or a plain
  `ollama serve` process it manages directly — rather than assuming one.
- **Server environment overrides** (Settings → Ollama): `OLLAMA_MODELS`,
  `OLLAMA_KEEP_ALIVE`, `OLLAMA_FLASH_ATTENTION`, `OLLAMA_NUM_PARALLEL` —
  applied the next time Start is used. For a systemd **user** service, a
  drop-in override (`~/.config/systemd/user/ollama.service.d/override.conf`)
  is written and reloaded automatically; for a plain process this app
  starts, they're just set directly in its environment. A systemd
  **system** service isn't modified (see Limitations).

### System monitoring

- CPU (`/proc/stat`) and RAM (`/proc/meminfo`) need no extra dependencies.
- GPU: enumerates every `/sys/class/drm/cardN` device and identifies each
  one's vendor from its PCI ID. **Multiple GPUs of mixed vendors are fully
  supported** and shown as a list, not just a single card.
  - **NVIDIA**: via NVML, loaded at runtime with `dlopen` — no NVIDIA driver
    or CUDA toolkit needed to *build* the app, and it runs fine on machines
    with no NVIDIA hardware at all.
  - **AMD**: read directly from sysfs (`amdgpu`).
  - **Intel**: utilization only, best-effort (varies by driver); VRAM is
    reported as unavailable since it's shared with system RAM.

### Appearance

- Light / Dark / Auto theme (Auto follows the OS live on Qt 6.5+).
- Customizable accent color, applied app-wide, plus independent colors for
  each of the four stats meters (CPU/RAM/GPU/VRAM) — all with a one-click
  reset back to default.
- Choice of Send button style (paper-plane icon, arrow, or text label).
- Themed app icon (tray, window/taskbar, and every dialog) that follows the
  active theme — black on light, light-on-dark — rather than a fixed color.
- Some icons are originaly from [System UI Line icon pack](https://www.svgrepo.com/collection/system-ui-line-icons)

## Requirements

- **Ollama itself, installed separately and reachable at
  `http://127.0.0.1:11434`** — this app is a client, it doesn't bundle or
  install Ollama. If Ollama isn't on `PATH`, the tray's Start/Stop won't be
  able to launch it directly (systemd-managed installs are unaffected,
  since that path doesn't need `PATH` at all).
- A working system tray. On **GNOME**, this means installing and enabling
  the AppIndicator/KStatusNotifierItem extension first — GNOME doesn't
  support tray icons out of the box, and the app will refuse to start
  without one.
- **Optional, for voice transcription**: a built
  [whisper.cpp](https://github.com/ggerganov/whisper.cpp) — clone it,
  `cmake -B build && cmake --build build --target whisper-cli`, and download
  at least one `ggml-*.bin` model (Settings' model manager can do this part
  for you). Everything else in the app works fine without it; the voice
  button just reports it isn't configured yet.
- Linux — GPU monitoring in particular is Linux-specific (sysfs, NVML via
  `dlopen`), and server control assumes systemd or a plain Unix process.

## Dependencies

Qt6, plus a handful of its modules:

```bash
sudo apt install build-essential cmake \
    qt6-base-dev qt6-multimedia-dev qt6-svg-dev qt6-webengine-dev
```

| Module | What it's for |
|---|---|
| `qt6-base-dev` | Core, Widgets, Network |
| `qt6-multimedia-dev` | Voice recording (push-to-talk capture) |
| `qt6-svg-dev` | Themed SVG icon rendering |
| `qt6-webengine-dev` | Embedded map view in chat replies |

No NVIDIA/AMD vendor SDK is needed at build time — GPU support is resolved
entirely at runtime.

## Build & run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ollama-tray
```

There's no install step required to run it locally; `cmake --install build`
will place the binary under `bin/` (via `RUNTIME DESTINATION bin`) if you
want it on `PATH` elsewhere.

**Launching from an app menu**: `ollama-tray.desktop` (checked in at the
repo root) points at this checkout's own `build/ollama-tray` and
`src/icons/ollama-app-icon.svg` — a static-color copy of the in-app themed
icon, since a `.desktop` file can't do the `{{iconColor}}` light/dark
substitution the app itself does at runtime (see `Theme::loadThemedIcon()`).
Copy or symlink it into `~/.local/share/applications/` to add it to your
launcher:

```bash
ln -s "$(pwd)/ollama-tray.desktop" ~/.local/share/applications/
update-desktop-database ~/.local/share/applications  # optional, most DEs pick it up automatically
```

See Limitations if you move this checkout elsewhere afterward.

## Data & configuration

- **Conversations**: one JSON file per conversation, at
  `~/.local/share/ollama-tray/conversations/<uuid>.json`. Deleting one
  through the app removes its file; there's no separate export/import.
- **Settings**: `~/.config/ollama-tray/ollama-tray.conf` (theme, accent/meter
  colors, send button style, context-length override, model-optimization
  toggle, Whisper binary/models-folder/selected-model paths, microphone
  device, Ollama server environment overrides). Delete this file to reset
  everything to defaults.
- **Systemd user drop-in** (only written if a systemd *user* `ollama.service`
  unit exists and at least one server environment override is set):
  `~/.config/systemd/user/ollama.service.d/override.conf`.
- Nothing is sent anywhere except your configured Ollama server, your
  selected microphone → the local whisper.cpp process (never leaves the
  machine), and — only when you explicitly enable the web search tool for a
  message — Wikipedia's public API.

## Known limitations

- **No in-app whisper.cpp build/install automation.** The app auto-detects
  an existing `whisper-cli` binary and can download *models*, but you still
  need to clone and build whisper.cpp yourself first — see Requirements.
  Also CPU-only: there's no GPU-acceleration toggle for transcription.
- **Web search is Wikipedia-only.** It's a genuinely useful "look this up"
  tool, but it is not a general web search engine — general search engines
  either block non-browser API access or require a paid key, and adding one
  (e.g. Brave Search) would need a user-supplied API key in Settings, which
  isn't built yet.
- **No remote/non-default Ollama host setting.** The client can technically
  point at a different base URL, but there's no Settings UI to configure it
  yet — it's hardcoded to `http://127.0.0.1:11434`.
- **Server environment settings (Settings → Ollama) only take effect via a
  systemd *user* service or a plain process this app starts directly.** A
  systemd *system* service isn't touched automatically (would need `pkexec`
  to write a root-owned unit file) — set `OLLAMA_MODELS`, `OLLAMA_KEEP_ALIVE`,
  `OLLAMA_FLASH_ATTENTION`, and `OLLAMA_NUM_PARALLEL` in its own unit file
  instead. Also: these only apply the next time Ollama is *(re)started* via
  the tray's Start/Stop, not to a server that's already running.
- **No model pull/delete UI.** You manage which models exist via the
  `ollama` CLI directly; this app only lets you pick among models Ollama
  already reports.
- **Map embeds use Google's unofficial, keyless "Embed a map" URL pattern**
  (not the documented, key-required Maps Embed API). No API key needed, but
  it's undocumented behavior that could change without notice. One
  query/marker per map block, no multi-stop routes.
- **Conversations can't be renamed** through the UI (only auto-titled from
  the first message), and there's no rename/archive/export flow.
- **The `.desktop` launcher entry points at a `build/` path, not an
  installed one** — `ollama-tray.desktop`'s `Exec`/`Icon` reference this
  checkout's own `build/ollama-tray` binary and `src/icons/` directly
  (there's no `make install` step that copies them anywhere system-wide),
  so it'll break if this directory is moved and needs re-generating/editing
  by hand for a packaged install.

## Troubleshooting

**"No system tray detected" on startup.**
GNOME doesn't support tray icons without an extension — install and enable
AppIndicator/KStatusNotifierItem (e.g. via GNOME Extensions), then relaunch.
Other desktop environments (KDE, XFCE, Cinnamon, etc.) generally support
tray icons natively.

**Tray icon looks like a plain drawn circle with "O" instead of the Ollama logo.**
This is a guaranteed-valid fallback used if the bundled icon somehow fails
to load — check the terminal output for a warning. It shouldn't normally
happen since the icon is compiled into the binary as a Qt resource.

**Tray icon or window icon doesn't recolor when I switch themes.**
It should update live — if it doesn't, that's a bug; a full app restart is
a reasonable workaround in the meantime.

**"Couldn't start `ollama serve`" from the tray.**
The loose-process fallback only fires when no systemd unit is found in
either the user or system scope, and it shells out to `ollama` by name — it
needs to be on `PATH` for whichever user account is running this app.
Confirm with `which ollama`, or start Ollama yourself and the app will pick
up on it as an externally-managed process instead.

**Stopping/starting the server via systemd asks for a password unexpectedly, or nothing happens.**
System-scope (not user-scope) systemd units go through `pkexec`, which pops
a native polkit authentication dialog — if that dialog doesn't appear (some
minimal window managers don't have a polkit agent running), the action will
silently fail. Installing your desktop environment's polkit agent (usually
already present on GNOME/KDE/XFCE) fixes this.

**Context-usage bar shows "context size unknown."**
The model's context length is fetched from Ollama via `/api/show` the first
time you use it and cached; this can briefly show "unknown" right after
picking a model, or permanently on a very old Ollama version whose
`/api/show` response doesn't expose `context_length` in a format this app
recognizes.

**A reply just stops with no error.**
Check that Ollama itself hasn't crashed or run out of VRAM — the tray
tooltip's live stats and the "Offload model" list are good first places to
look. If a different conversation's turn was queued behind this one and a
model swap was needed, also check whether the swap (an explicit unload)
actually completed on the Ollama side.

**Voice transcription fails or says it isn't configured.**
Check Settings' "Voice transcription (Whisper)" section: it needs a built
`whisper-cli` binary and at least one downloaded model (both auto-detected
from a standard `~/whisper.cpp` checkout, or set manually). If a model is
selected but transcription still comes back empty, check the "Mic" meter in
the system stats strip while holding the record button — if it doesn't move
at all, the wrong input device is likely selected in Settings' microphone
picker; if it does move but transcription still fails, the error message
now surfaces whisper-cli's actual diagnostic line rather than a generic one.

## Contributing / project layout

Single Qt6 Widgets application, no QML. Key files:

- `MainWindow` — the sidebar + chat + stats-strip window.
- `ChatWidget` — message rendering, streaming, editing/retry, attachments.
- `ChatQueue` — serializes/schedules turns across conversations against the
  one real Ollama server.
- `OllamaClient` — thin wrapper over Ollama's REST API, multi-stream-capable.
- `WhisperManager` — detects/configures `whisper-cli` and its models,
  downloads new ones, and runs transcription as a subprocess.
- `VoiceRecorder` — raw microphone capture (push-to-talk), live level
  metering, and 16 kHz mono WAV encoding for `WhisperManager`.
- `ConversationStore` — in-memory conversation list mirrored to per-file JSON.
- `SystemMonitor` — CPU/RAM/GPU polling.
- `ServerController` — detects and drives whichever mechanism (systemd
  user/system, or a raw process) actually owns the Ollama server.
- `TrayApplication` — tray icon, menu, tooltip.
- `Theme` / `ThemeManager` — the app's QSS stylesheet and light/dark/auto
  resolution.

No automated test suite exists yet; changes have generally been verified by
building, running, and exercising the relevant feature manually.

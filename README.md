# ollama_gui

A lightweight, single-file Qt6 desktop chat interface for [Ollama](https://ollama.com). No Electron, no Python runtime — just a small C++ binary that talks directly to the local Ollama HTTP API.

---

## Features

### Model selection
- On launch a **model picker dialog** fetches all locally available models from Ollama (`/api/tags`) and displays them in a list.
- Models can also be typed in manually if the name is known but not yet pulled.
- The dialog falls back gracefully when Ollama is unreachable, showing an error and keeping the text field available.
- A **Switch model** button in the chat header lets you change models mid-session at any time. The new model is recorded in the session and a notice is appended to the chat.
- Pass a model name as a command-line argument to skip the picker entirely:
  ```
  ./ollama_gui mistral
  ```

### Streaming responses
- Responses are streamed token-by-token from Ollama's `/api/generate` endpoint, so text appears progressively rather than after a long wait.
- Sending a new message while a response is still streaming aborts the current request cleanly before starting the new one.

### Session management
- A **sidebar** on the left lists all saved sessions, sorted by most recently updated.
- Click any session to switch to it instantly. The current session is saved before switching.
- The **New session** button creates a fresh chat with the same model.
- **Right-click** a session for a context menu with:
  - **Rename** — set a custom title via an input dialog.
  - **Delete** — remove the session with a confirmation prompt (cannot be undone).
- The session title is set automatically from the first message you send (up to 40 characters), so sessions are self-labelling without any manual step.
- Long titles are elided with `…` to fit the sidebar width; no horizontal scrollbar appears.
- Hovering a session shows a tooltip with the model name and last-updated timestamp.

### Auto-save
- Every session is saved to disk automatically after each user message and after each completed bot reply — no explicit save action required.
- Sessions survive force-closing the application because they are written after every exchange, not just on clean exit.
- Storage location: `~/.ollama_gui_sessions/` — one JSON file per session, named by UUID.

### Native look and feel
- Uses the platform's native Qt style (Breeze on KDE, Fusion, Windows, macOS Aqua, etc.) with no overriding stylesheets.
- Separators between the sidebar and chat area use `QFrame::VLine` and `QFrame::HLine` with sunken shadow, matching the desktop theme.
- Alternating row colours in the session list come from the platform palette.

---

## Requirements

- **Ollama** running locally on `http://localhost:11434`
- **Qt 6** (Widgets + Network modules)
- A C++20-capable compiler (GCC 11+, Clang 13+)

---

## Building

```bash
$(pkg-config --variable=libexecdir Qt6Core)/moc main.cpp -o main.moc && \
g++ -std=c++20 -fPIC main.cpp -o ollama_gui \
  $(pkg-config --cflags Qt6Widgets Qt6Network | sed 's/-I/-isystem /g') \
  $(pkg-config --libs Qt6Widgets Qt6Network) \
  -Wall -Wextra -Wpedantic -Werror \
  -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor \
  -Wold-style-cast -Woverloaded-virtual -Wnull-dereference \
  -Wdouble-promotion -Wformat=2
```

Or with the included `Makefile` if present:

```bash
make
```

---

## Usage

```
./ollama_gui              # opens model picker on launch
./ollama_gui <model>      # skips picker, starts with the given model
```

Press **Enter** or click **Send** to submit a message.

---

## Session storage format

Each session is stored as a plain JSON file in `~/.ollama_gui_sessions/<uuid>.json`:

```json
{
  "id": "3f2a1b4c-...",
  "title": "What is the capital of France?",
  "model": "mistral",
  "updatedAt": "2024-11-15T14:32:00",
  "messages": [
    { "role": "user", "text": "What is the capital of France?" },
    { "role": "bot",  "text": "The capital of France is Paris." }
  ]
}
```

Files can be read, backed up, or deleted directly from the filesystem.

---

## License

MIT

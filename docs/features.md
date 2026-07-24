# orc — Feature List

A complete inventory of every feature in `orc`, the native-C OpenRouter chat
client. Ordered roughly from the most basic to the most advanced. As the
project grows, append new features in the section where they best fit.

---

## 1. Invocation & modes

- One-shot prompt: `orc "prompt"` — ephemeral, not persisted
- Interactive chat session (REPL) when no prompt is given
- Named conversations: `orc -c NAME` resumes or starts `conversations/NAME.jsonl`
- Single appended turn to a named conversation: `orc -c NAME "prompt"`
- `-h` / `--help` with a full command and option reference
- Argument validation: unknown options and multiple prompts are rejected
- Interactive sessions are always persisted (auto-named); bare one-shots stay ephemeral
- Auto-named conversations use a `chat-YYYYMMDD-HHMMSS` timestamp

## 2. Model selection

- `-m MODEL` flag to choose the model id
- Resolution order: `-m` flag > saved config model > built-in `DEFAULT_MODEL`
- `/model` in-chat picker: switch the active model mid-session from a live-filtered list
  - Server-side `output_modalities=text` filter — only models orc can render are fetched (no image/audio/embedding-only models)
  - Alternate-screen UI: type to filter (id/name), ↑/↓ to move, Enter to select, Esc to cancel — leaves scrollback and history untouched
  - Columns: context length, input/output price ($/M tokens), and a vision-input flag
  - Star favourites with Tab; stars persist in the user config (`stars=`) and sort to the top on future opens
  - The chosen model is saved as the default (user config `model=`), so it persists across restarts (a `-m` flag still overrides for that run)
  - Parsed with a depth-aware JSON array walker (`json_array_next_object`), so nested/brace-containing fields never mis-delimit a model object

## 3. Configuration & API key

- API key discovery order: `OPENROUTER_API_KEY` env var > project-local `.env` > user config file
- `KEY=value` parsing: whitespace trimmed, one surrounding quote pair stripped, comments/blank lines ignored, empty value counts as "not set"
- Persistent user settings at `$XDG_CONFIG_HOME/orc/config` (or `~/.config/orc/config`), file `0600` / dir `0700`
- `orc config set key <API_KEY>` — save the API key
- `orc config set model <id>` — save the default model
- `orc config get <key|model>` — show a value (API key masked, e.g. `sk-or-…a1b2`)
- `orc config list` — show all settings (key masked)
- `orc config unset <key|model>` — remove a value
- `orc config path` — print the config file location
- Config subcommands run before startup and need no API key
- Atomic writes: temp file + `fchmod 0600` + `rename(2)`, preserving unrelated keys and comments

## 4. Conversation persistence

- JSONL storage: `conversations/<name>.jsonl`, one message object per line
- Request payload rebuilt by comma-joining stored lines (each line is a valid messages-array element)
- `.jsonl` extension auto-appended; name validation rejects empty names and `/` (path-traversal guard)
- Malformed/blank lines skipped with a warning on load
- Message-shape validation (string role, string-or-array content) before replay
- Durable append (fflush + fclose), file created on first write
- Prior history replayed on resume (`--- Previous messages ---`), styled like the live prompt
- `/rename NAME` renames the live conversation; refuses to overwrite an existing target, tolerates a not-yet-created source
- `/save [NAME]` exports the last assistant reply as clean Markdown to `saves/<name>.md` (timestamped default name, `.md` auto-appended, `/` rejected); works on a resumed conversation by recalling the last reply from disk
- Both plain-string and pre-encoded-JSON (multimodal) content variants for append and in-memory add

## 5. Interactive REPL

- `/quit`, `/exit`, and Ctrl-D (EOF) end the session
- `/help` (or `/?`) shows a styled command and key-binding reference on the alternate screen, dismissed with any key — leaves scrollback and history untouched
- Colored `you>` / `assistant>` prompts and banners (256-color, TTY-gated)
- Emacs-style editing keys: Ctrl-A/E (line start/end), Ctrl-K/U (kill to end/start), Ctrl-W (kill word), Ctrl-D (delete/EOF), Backspace joins lines
- Ctrl-L clears the screen and repaints
- Session command history: submitted messages recalled via Up/Down, consecutive duplicates skipped, in-progress line stashed and restored
- Failed-turn rollback: an unanswered user message is removed from memory and truncated from the file

## 6. Line editor (raw-mode)

- Raw-mode termios editor (no echo/ICANON/IEXTEN), ISIG kept on so Ctrl-C still works
- Graceful non-TTY fallback to `getline()` so piped input keeps working
- ANSI-aware prompt width (CSI color escapes excluded from width math)
- UTF-8 aware cursor movement (whole characters, continuation bytes skipped)
- Wide-character display-width accounting via `mbrtowc`/`wcwidth` for CJK/double-width glyph alignment
- Horizontal scrolling for long lines (linenoise-style clipped windows), cursor kept on-screen
- Multi-line input: `\`+Enter or Shift+Enter opens a continuation line; embedded newlines returned in the result
- Vertical cursor movement across message lines (Up/Down preserve goal column, every line editable)
- History recall at edges (Up/Down fall through to session history at first/last line)
- Kitty keyboard protocol negotiation so Shift+Enter / Ctrl / Alt arrive as CSI-u sequences (popped on exit and in the signal handler)
- xterm modifyOtherKeys support (decodes the `27;2;13` form of Shift+Enter)
- Ctrl-key-via-CSI-u handling re-dispatched to C0 handlers; self-delivers SIGINT for Ctrl-C when no C0 byte arrives
- Bracketed paste: pasted blocks arrive as one unit and never submit; CR/CRLF normalized to `\n`
- Multi-line paste placeholders `[Pasted #N +K lines]` spliced back on submit, discarded if the placeholder is deleted (up to 16 held)
- In-place block repaint: per-line prompts redrawn, shrunk rows erased, cursor parked precisely, synchronized redraw
- Resize-aware repaint on `EINTR` (SIGWINCH) using live `TIOCGWINSZ` width
- OOM-hardened editing: insert/paste growth guarded against `SIZE_MAX` overflow; keystroke dropped rather than corrupting the line
- Async-signal-safe terminal restore (`le_signal_restore` pops kitty protocol + bracketed paste and restores termios from a fatal-signal handler)

## 7. Streaming & display

- SSE token streaming to stdout (default)
- `--no-stream` single-shot buffered mode
- `--no-markdown` disables the Markdown re-render (plain text only)
- Background "thinking" spinner (Braille animation on stderr, in its own thread) while a blocking call runs
- Condvar-driven spinner timing so `spinner_stop` wakes it immediately
- Spinner TTY-gating (no-op when stderr isn't a terminal; erases its line on stop)
- Empty/role-announcement content deltas ignored so the spinner doesn't stop early
- Live Markdown re-render on the alternate screen as tokens stream, then a final clean render on the main screen
- Two-phase alt-screen render (`?1049h` enter, wiped on leave, final reply drawn on the main screen)
- Synchronized-update framing (`?2026h`/`?2026l`) for flicker-free redraws
- ~60ms repaint throttle so fast token streams don't thrash the terminal
- Full-screen clear + redraw per frame to handle replies taller than the terminal
- Esc interrupts a streaming reply while keeping the partial text
- Alternate-screen entry/leave with signal-safe restoration on SIGINT/SIGTERM/SIGHUP

## 8. Markdown → ANSI renderer

- libcmark-backed CommonMark renderer (full document parsed and rendered to ANSI)
- NO_COLOR + dumb-terminal support: color gated on `isatty` and absence of `NO_COLOR` / `TERM=dumb`, plain-text fallback otherwise
- `setlocale(LC_CTYPE, "")` for correct cell-width (emoji/CJK/combining) in the table renderer
- Colored headings by level (6-level palette with dim `#` prefix)
- Inline emphasis: bold, italic, inline code, links, dim/underline
- Thematic breaks / horizontal rules
- Blockquotes with nested `│ ` prefixes and depth tracking
- Lists: ordered/unordered with `•`/`N.` markers, nested indentation, hanging indent aligned to marker width
- Code blocks framed with box-drawing (`┌─`/`│`/`└─`) plus the fence-info language label
- Inline & block HTML shown dimmed rather than dropped
- GFM extensions: strikethrough `~~`, highlight `==`, task-list checkboxes `☐`/`☑`
- Sub/superscript rendering (`^...^` / `~...~` mapped to Unicode glyphs, `^(...)` fallback)
- Autolink detection for bare `http(s)://` / `www.` URLs with trailing-punctuation trimming
- Link/image annotation: muted parenthetical URL/title; images prefixed `🖼`
- GFM pipe tables: hand-rolled parser (escaped pipes, code-span aware, fenced-code exclusion), per-column widths, left/center/right alignment, box-drawn borders with styled header row
- Inline LaTeX-to-Unicode translation: large symbol table (arrows, operators, sets/logic, full Greek), `\frac{a}{b}` → `a/b` with smart parenthesization, `^`/`_` scripts, `$...$`/`$$...$$` spans, price-like false positives rejected
- Robust fallbacks: parser/OOM failure prints raw text; malformed UTF-8 consumed one visible byte instead of losing content

## 9. Images & multimodal

- Ctrl+V pastes a clipboard image into the prompt as an `[Image N]` placeholder, printing a status line above the prompt
- Cross-platform clipboard image capture: macOS via `osascript`; Linux/BSD via `wl-paste` (Wayland) or `xclip` (X11), chosen at runtime from the session environment
- Distinct diagnostics for an imageless clipboard versus a missing clipboard helper (advises installing wl-clipboard/xclip)
- Shell-injection guard: rejects paths containing quotes/backslashes before interpolation; removes empty/partial output files
- Base64 data-URL encoding (`data:image/png;base64,...`) for the OpenRouter multimodal format
- Multimodal content array (text + `image_url` data URLs) sent to vision models
- Image-modality preflight against OpenRouter model metadata (`input_modalities`), cached once per session
- Optimistic fallback when support can't be verified (sends anyway with a warning)
- Image de-attach by placeholder deletion (only images whose `[Image N]` survives editing are sent)
- Up to 16 pending images per message; saved as PNGs under `conversations/attachments/`
- Resumed history shows the text part (with `[Image N]` markers) of multimodal messages

### Image output (generation)

- Generated images are parsed from the reply's `images` array (both streaming `delta.images` and non-streaming `message.images`), in the same `image_url` data-URL shape used for input
- No request changes: image-output models return images by default; sending a `modalities` param would break text-only models, so it is never sent
- Base64-decoded and saved under `conversations/generated/img-<ts>-N.<ext>`, extension from the data-URL mime (png, jpeg, …); multiple images per reply supported
- Inline rendering by shelling out to `chafa`, which handles every format and the terminal's graphics protocol itself (Kitty on kitty/Ghostty/WezTerm, Sixel, iTerm2, or symbol art)
- `chafa` is an optional runtime helper (like the clipboard tools): when it is absent, or stdout is not a terminal, the saved path is printed instead
- Shell-injection guard on the path before invoking chafa; rendered as a modest inline preview (`--size=72x24`)
- Image-only replies (null text content) are handled as success, not an error
- Persisted as an `[image: <path>]` marker in the conversation text (image data is not stored in history); resuming a conversation re-renders each referenced image via chafa

## 10. API transport

- OpenRouter `/chat/completions` POST via libcurl (streaming and single-shot paths)
- App attribution headers (HTTP-Referer, X-OpenRouter-Title)
- Bearer-token auth header with an implausible-length guard
- 300-second overall transfer timeout; 30s for the model preflight
- `CURLOPT_NOSIGNAL` for thread-safe timeout alongside the spinner thread
- SSE line reassembly across arbitrary network chunk boundaries (CR/LF handling, `[DONE]` sentinel, comment lines)
- Growable byte buffer (geometric growth, always NUL-terminated, `SIZE_MAX`-checked) for HTTP/SSE/JSON assembly and off-screen render measurement
- Retry-After header capture for 429/503
- Actionable per-status error hints (401 key, 402 credits, 408 timeout, 429 rate limit, 502/503 provider)
- Mid-stream SSE error-event detection (HTTP 200 with `finish_reason: "error"`)
- Warm-up detection: a 200 with no content suggests retrying
- Rich error diagnostics (HTTP code + error_type + message + hint + retry-after)
- Error-sink redirection (`errs` buffer) so alt-screen streaming errors print after screen restore
- Esc-cancel via a curl progress callback polling raw stdin (works even on a stalled stream; distinguishes a lone Esc from an escape sequence)

---

*Note: clipboard image capture works on macOS (`osascript`) and on Linux/BSD
(`wl-paste` for Wayland, `xclip` for X11); the Linux helper must be installed for
Ctrl+V to capture. Everything else is portable POSIX.*

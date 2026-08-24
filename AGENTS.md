# ZIme Agent Knowledge Base

This file is the fast-start maintenance guide for agents working on this repo.
It is intentionally practical: where to look, what each layer owns, and which
behaviors are already known-good and should not be accidentally reverted.

## 1. Project Overview

- This repository contains a Windows TSF IME project.
- The main DLL lives in `ime/`.
- The built IME DLL is `casual_ime.dll`.
- The root `CMakeLists.txt` builds:
  - the IME DLL in `ime/`
  - dictionary tools in `dic/`
- IME name, GUIDs, profile info, and basic constants are in
  `ime/ime_config.h`.

## 2. Directory Map

### Core IME

- `ime/text_service.h`
  - central `text_service` TIP class
  - implements:
    - `ITfTextInputProcessorEx`
    - `ITfThreadMgrEventSink`
    - `ITfKeyEventSink`
    - `ITfCompositionSink`
    - `ITfTextLayoutSink`
- `ime/text_service.cpp`
  - activation/deactivation
  - sink registration
  - focus handling
  - runtime state init
  - layout sink refresh
- `ime/text_service_key_processor.cpp`
  - almost all keyboard behavior
  - start here for most feature changes
- `ime/text_service_composition_controller.cpp`
  - edit sessions
  - composition creation/update/commit/clear
- `ime/text_service_ui_controller.cpp`
  - candidate window
  - TSF UIElement integration
  - candidate positioning
  - status window
  - create-word dialog
  - runtime config load/save

### UI

- `ime/candidate_form.*`
  - custom floating candidate window
  - paging, selection, display text, click handling
- `ime/candidate_ui_element.*`
  - TSF candidate UIElement bridge
  - host can choose to integrate or suppress our own window
- `ime/status_window.*`
  - floating mode/status window
  - menu-driven feature toggles

### Dictionary

- `ime/ime_dictionary.*`
  - main dictionary loading
  - user overlay DB
  - candidate sorting
  - pinyin vs non-pinyin detection
- `dic/`
  - dictionary compiler / helper tools

### Registration and DLL entry

- `ime/DllMain.cpp`
  - `DllRegisterServer`
  - `DllUnregisterServer`
- `ime/register.cpp`
  - COM registration
  - TSF categories
  - TSF profiles
  - AppContainer file ACL support
  - per-user Broker installation-path registration

### Per-user Broker

- `broker/main.cpp`
  - secured per-session named-pipe server
  - same-user/session validation and lifecycle
- `broker/broker_ui.*`
  - out-of-process candidate and status windows
- `broker/broker_storage.*`
  - owns the only initialized `ime_dict` instance
  - candidate lookup, sorting, auto-commit directives, config, and dictionary writes
  - legacy data migration and AppContainer data ACLs
- `common/broker_protocol.h`
  - versioned, bitness-neutral IPC contract
- `ime/broker_client.*`
  - nonblocking worker client
  - callbacks marshalled back to the TSF thread

### Misc

- `ime/tool.*`
  - `tool::get_current_dll_path()`
  - many runtime files are resolved relative to DLL directory
- `test/tsf_helper.*`
  - helper code only, not a full automated test suite

## 3. Runtime Files and Paths

Many issues are really "wrong deployment directory" issues.

Current path rules:

- Main dictionary:
  - `dict.idx`
  - loaded from `tool::get_current_dll_path() / "dict.idx"`
- User overlay DB:
  - `user_dict.db`
  - loaded from `tool::get_user_data_path() / "user_dict.db"`
- Runtime config:
  - `config.ini`
  - loaded from `tool::get_user_data_path() / CONFIG_FILE`
  - a fresh user is seeded from installed `default-config.ini`
- Status icons:
  - canonical sources are the eight files in `assets/status-icons/`
  - installed under `ico/` and searched from the DLL dir and parent dirs

The Broker migrates legacy mutable files beside the DLL on first start.
`ZIME_DATA_DIR` is a test-only data-directory override.

## 4. Central Runtime State

Important `text_service` members:

- `m_compositionText`
  - logical composition string owned by the IME
- `m_bInComposition`
  - logical "we are composing" flag
- `m_pComposition`
  - actual TSF composition object
  - this may be null even while `m_compositionText` is not empty
- `m_candidateWindow`
  - custom candidate form
- `m_candidateUIElement`
  - TSF UIElement bridge
- `m_brokerCandidateCode`
  - code associated with the latest Broker candidate snapshot
- `m_lastCandidateAnchorRect`
  - cached last good candidate anchor rect

Important persisted / runtime toggles:

- `m_autoCommitFourCodeUnique`
- `m_commitFirstCandidateOnFifthCode`
- `m_showUncommonCandidates`
- `m_replaceDotAfterDigit`
- `m_useEnglishPunctuationInChineseMode`
- `m_disableChineseDash`
- `m_candidateSortMode`
- `m_uiFontPercent`

## 5. Input Flow

The normal path for Chinese-mode input is:

1. `OnTestKeyDown`
   - decides whether a key is intercepted
2. `OnKeyDown`
   - routes letters, numbers, space, backspace, enter, paging, arrows, symbols
3. `HandleCharacter`
   - appends to `m_compositionText`
   - asynchronously requests droppable 1-3 code previews from the Broker
   - uses a priority synchronous Broker query for four-code decisions
   - executes Broker auto-commit/fifth-code directives through TSF
   - calls `ShowCandidates`
4. `ShowCandidates`
   - updates UIElement state
   - shows / repositions custom candidate window if needed
5. commit path
   - `HandleNumber`
   - `CommitFirstCandidateOrComposition`
   - `InsertText`
   - `InsertRawText`

Important rule for maintainers:

- If a key behavior changes, inspect both `OnTestKeyDown` and `OnKeyDown`.
- Editing only `OnKeyDown` often causes the host to receive keys first.

## 6. Composition Model

This project intentionally distinguishes between:

- logical composition string: `m_compositionText`
- actual TSF composition object: `m_pComposition`

They are not guaranteed to exist together.

Why:

- Some hosts do not provide a valid layout/caret range early enough.
- Forcing TSF composition in that state may cause a system floating
  composition UI at the top-left corner.

Current design:

- `UpdateCompositionInContext()` uses `composition_edit_session`
- `composition_edit_session::EnsureComposition()` tries to create a true TSF
  composition only if `GetTextExt` returns a valid rect
- If that fails, the IME can still keep its internal composition string and
  later commit text directly

Never assume:

- "input is active, so `m_pComposition` must exist"
- "candidate positioning can always depend on composition range"

## 7. Candidate Retrieval and Sorting

All runtime candidate lookup goes through the Broker. The Broker owns the only
initialized `ime_dict` and calls `ime_dict::get_candidates()` under its storage
lock. Hosts retain only the current IPC candidate snapshot needed by TSF/UI.

High-level flow:

1. query main dictionary by current code
2. if code length is less than 4, query extension codes
3. merge overlay data
4. dedupe
5. sort
6. generate view texts

The Broker loads the complete immutable dictionary, including candidate data,
into memory during startup rather than reading candidate records on each key.
Asynchronous candidate requests are coalesced so only the newest queued code is
looked up. The Broker posts successful results directly to its candidate window
before returning the same snapshot to the TIP for TSF/UIElement state.

Candidate text crosses the pipe only from Broker to TIP. Candidate-state
messages from the TIP contain composition, visibility, selection, paging, and
anchor metadata, but never resend candidate strings. The Broker caches its own
latest query result per client connection so result-before-state ordering is
safe and layout storms remain lightweight.

Candidate previews are asynchronous and droppable. Four-code auto-commit,
fifth-code decisions, space selection, and number selection use a priority
synchronous query with a short timeout. Never make later keys wait for a normal
window-message candidate callback again.

Overlay sources in `user_dict.db`:

- `user_words`
- `blocked_words`
- `uncommon_words`
- `candidate_stats`

### Pinyin vs non-pinyin

Current meaning of `Candidate::is_pinyin()`:

- `wubi_code_index != 0` means pinyin candidate
- `wubi_code_index == 0` means non-pinyin candidate

This matters because auto-commit logic already depends on it.

### Hard sorting rule

Even with recent/frequency sorting enabled:

- full-code, non-pinyin, wubi candidates must stay as the top hard group

Do not accidentally reorder pinyin candidates above that group.

## 8. Candidate UI Model

There are three candidate UI layers:

### 1. Custom candidate window

- `candidate_form`
- our own floating UI

### 2. TSF UIElement

- `candidate_ui_element`
- exposes candidate state to the host
- host may integrate the UI itself or suppress our custom window

### 3. Broker candidate window

- runs outside AppContainer restrictions
- uses Broker-owned query snapshots without making TSF calls
- receives only UI metadata and cursor anchors from host DLLs
- returns UI actions to the original TSF thread

Important interactions:

- `ShowCandidates()` updates UIElement first
- then it arbitrates local or Broker UI
- `OnHostCandidateUiShowChanged()` updates `m_hostWantsCandidateWindow`

If candidates exist but our custom window does not show, check whether the host
suppressed it through UIElement flow.

## 9. Candidate Window Positioning

This area has historically been easy to break.

Current positioning idea:

1. `ITfContextView::GetTextExt`
2. real caret fallback
3. cached last good anchor rect
4. visible fallback from focus/view/owner area

### Why `ITfTextLayoutSink` exists here

Many hosts do not provide a usable text layout during the same key event that
starts or updates composition.

So this project now:

- advises `ITfTextLayoutSink` for the focused context
- refreshes that sink on activate/focus/context changes
- repositions the candidate window in `OnLayoutChange(TF_LC_CHANGE)`
- performs the first positioning EditSession while the Broker is active
- lets the Broker try the client GUI-thread caret before a visible fallback

This is much safer than relying only on synchronous key-time positioning.

### Things not to reintroduce

Do not bring back these behaviors:

- fallback to mouse cursor position
- caching obviously fake fallback coordinates as if they were real anchors
- assuming "first lookup failed, but repeated key handling on the same path
  will naturally fix it"

## 10. Known-Good Behavior Rules

These behaviors were recently fixed and should be preserved.

### 1. Four-code unique auto-commit

Auto-commit only when all are true:

- the feature is enabled
- composition length is exactly 4
- there is exactly one candidate
- that unique candidate is not pinyin

### 2. Fifth-code commit-first-candidate

Commit first candidate on the fifth code only when all are true:

- the feature is enabled
- the new input char is a letter code
- the previous composition length is exactly 4
- a first candidate exists
- the first candidate is not pinyin

### 3. Symbol behavior during composition

If there is an active composition/candidate state:

These four classes are treated as part of the composition string:

- `Shift+digit`
- `Shift+=`
- `Shift+-`
- `Shift+\``

They do not commit the current candidate first.

Other symbols behave differently:

- commit first candidate or current composition first
- then insert the symbol itself

Do not collapse these two symbol classes into one behavior.

### 4. Chinese / English mode expectations

- When focus returns to this IME, Chinese mode is restored by default.
- Entering Chinese mode initializes punctuation from
  `m_useEnglishPunctuationInChineseMode`; a status-button override must then
  remain in effect until the next language-mode transition.
- English mode does not preserve Chinese punctuation behavior.

## 11. Status Window and Config

The status window is the main runtime toggle surface.

Config file:

- `config.ini`
- stored in the per-user data directory
- written atomically by the Broker and revision-broadcast to DLL instances

Currently persisted values include:

- four-code auto-commit
- fifth-code commit-first-candidate
- show uncommon candidates
- replace Chinese period with `.` after digits
- use English punctuation in Chinese mode
- disable Chinese dash on `Shift+-`
- candidate sort mode
- UI font percent
- status window position

Important note:

- Chinese/English, full/half width, and current punctuation are host-session
  state and are not part of the Broker global config
- focus restoration sets Chinese mode again

## 12. User Words, Delete, Uncommon, Stats

These features are overlay operations in `user_dict.db`, not edits to the base
dictionary file.

All mutations are serialized by the Broker. Successful mutations increment a
dictionary revision and notify every connected TIP; active compositions then
request a fresh candidate snapshot.

Broker config and dictionary revisions are process-local. Clients must reset
their revision baselines after reconnecting because a replacement Broker starts
again at revision 1; treating that rollback as stale creates a candidate-query
feedback loop and severe window flicker.

Main entry points:

- add custom word:
  - `ime_dict::add_custom_word`
- delete candidate:
  - `ime_dict::delete_candidate`
  - writes to `blocked_words`
- mark uncommon:
  - `ime_dict::mark_candidate_uncommon`
- selection statistics:
  - `ime_dict::record_candidate_selected`

If a deleted candidate "comes back", first verify that the runtime is reading
and writing the same deployed `user_dict.db`.

## 13. Registration and Modern Host Support

Two important areas in `ime/register.cpp`:

### 1. AppContainer ACL handling

During registration, the code grants read/execute access to the install tree so
modern AppContainer hosts can load the IME.

Registration records the installed Broker path for identity validation. x86
and x64 registrations are tracked separately, and the DLL verifies the
connected Broker executable path. Legacy HKCU Run entries are removed; normal
TIP clients connect first and launch the per-user Broker only after the pipe is
unavailable.

This is important for hosts such as:

- SearchHost
- Sticky Notes
- Settings and similar modern text boxes

### 2. TSF categories

The IME currently registers capabilities including:

- keyboard TIP
- immersive support
- UIElement-enabled support

Do not remove those casually.

### 3. Installer registration

- `installer/zime_registrar.cpp` is built for x86 and x64.
- Inno Setup launches these helpers without waiting in `[Run]`, polls their
  result files while pumping window messages, and streams registration trace
  lines into the installing-page log box.
- Setup logging is always enabled; DLL registration phases are also appended
  to `{app}\\zime_install.log`.
- AppContainer ACLs are non-inheritable and applied only to the path chain and
  critical runtime files. Do not use `SetNamedSecurityInfo` on drive roots or
  Program Files: its automatic child propagation made registration take about
  one minute per DLL on a populated drive.

## 14. What `ime_trace` Is

`ime/ime_trace.h` and `ime/ime_trace.cpp` define a small tracing helper:

- function:
  - `ime_tracef(const wchar_t* event, const wchar_t* fmt, ...)`
- output targets:
  - `OutputDebugStringW`
  - `%TEMP%\\zime_ui_trace.log`
- each log line includes:
  - local timestamp
  - pid
  - tid
  - process name
  - event name

Current status of `ime_trace`:

- the files exist in the working tree
- UI, activation, positioning, and Broker paths have active call sites
- detailed trace calls are compiled only in Debug builds
- `Release` and `RelWithDebInfo` remove normal trace calls at preprocessing time
- Debug tracing requires `ZIME_TRACE=1` and writes the existing trace files
- production errors write only to capped `%TEMP%\zime_error.log` and
  `%TEMP%\zime_broker_error.log`
- error logs never include composition text, candidates, or custom-word text

If tracing is needed, useful insertion points include:

- `OnKeyDown`
- `HandleCharacter`
- `UpdateCompositionInContext`
- `ShowCandidates`
- `UpdateCandidateWindowPosition`
- `OnLayoutChange`
- `CommitFirstCandidateOrComposition`

## 15. Where To Edit For Common Requests

### Change key behavior

Start with:

- `ime/text_service_key_processor.cpp`

### Change candidate sorting, auto-commit, pinyin filtering

Start with:

- `ime/text_service_key_processor.cpp`
- `ime/ime_dictionary.cpp`

### Change candidate positioning or host compatibility

Start with:

- `ime/text_service_ui_controller.cpp`
- `ime/text_service.cpp`
- `ime/text_service_composition_controller.cpp`

### Change status menu or persisted settings

Start with:

- `ime/status_window.*`
- `ime/text_service_ui_controller.cpp`

### Change custom word, delete word, or overlay DB behavior

Start with:

- `ime/ime_dictionary.cpp`
- `ime/text_service_ui_controller.cpp`

## 16. Practical Maintenance Advice

### 1. Always inspect both key interception and key handling

That means:

- `OnTestKeyDown`
- `OnKeyDown`

### 2. Do not treat internal composition state and TSF composition object as
the same thing

They can diverge depending on host behavior.

### 3. Debug candidate problems by layer

Check which layer is actually wrong:

- no candidate data
- candidate data exists but UIElement/custom window visibility is wrong
- window is visible but position is wrong
- initial position is wrong but `OnLayoutChange` later fixes it

### 4. Verify deployment and data directories first

Especially for:

- `dict.idx`
- `zime_broker.exe`
- `user_dict.db`
- `config.ini`

### 5. If timing is unclear, add tracing

Prefer `ime_tracef` over guessing.

### 6. Preserve the host thread's `WM_QUIT`

`WM_QUIT` bypasses `PeekMessage` window and message-range filters. Any callback
queue cleanup that removes messages must detect and repost it, otherwise the
host process can remain alive after its last window closes.

## 17. Local Build

Common commands:

```powershell
cmake -S . -B build
cmake --build build
```

This repo currently does not have a full automated test pipeline.
Validation is mainly:

- successful local build
- manual regression in target hosts

## 18. Minimum Reading Order For New Agents

If you need to onboard fast, read in this order:

1. `ime/text_service.h`
2. `ime/text_service_key_processor.cpp`
3. `ime/text_service_composition_controller.cpp`
4. `ime/text_service_ui_controller.cpp`
5. `ime/text_service.cpp`
6. `ime/ime_dictionary.h`
7. `ime/ime_dictionary.cpp`

That gives the quickest mental model for:

- key handling
- composition
- candidate retrieval
- UI flow
- commit behavior
- sorting and overlays

# Send Replays to PC (design, not built yet)

**Status: proposal.** A player's Slippi replays (`.slp`) and SlippWii's logs go from the Wii to a
PC on the same network when the player asks, from the loader's menu, and land where Slippi Launcher
reads them. Nothing is sent during a game, nothing runs unless asked, and starting a game is never
held up.

## What the player sees

On the *Slippi Settings* page:

| Row | What it does |
|---|---|
| **Send Replays to PC** | An action, as *Check for Updates* is: finds the PC, sends what the PC doesn't have yet, shows progress. **B** cancels at any time. |
| **After Sending** | *Keep on card* (default) or *Remove from card*: what happens to a file once the PC has confirmed it. |

The action's screen, as the updater's:

```
Send Replays to PC
  Finding your PC...                    found 192.168.2.93
  12 replays and 3 logs to send (31 MB)
  Sending 5 of 15: Game_..._20261121T211403.slp   62%
  Done: 15 sent, 15 removed from the SD card (31 MB freed).
```

Messages for what goes wrong: no network; no PC found (with "is the receiver running? is port 51460
open in the PC's firewall?"); the PC refused a file (and why); a file that could not be read (left
on the card, named). Nothing on the card changes for any of these.

On the PC each game is a folder Slippi Launcher reads:

```
~/Slippi/Wii/2026-11-21/21-14-03/
  game.slp      the replay, byte for byte the card's
  wii.log       that game's lines of the Diagnostic Log (when there is one)
  summary.txt   from the log's end-of-game lines: frames, waited frames, ping, rollbacks, quality, checksums
~/Slippi/Wii/reports/   crash-N.txt and desync-N.txt as they were on the card
```

## The parts

| Part | Where | What |
|---|---|---|
| The action and its screen | `loader/source/replay_send.c` (new), a row in `menu.c` | the network (`network_start`, as `update.c`), the transfer, the screen |
| The transfer's core | `loader/source/replay/` (new): no libogc in it | which files to send, the messages, the answers, what may be removed: built and tested on a PC (as `update/upd_core.c` is) |
| The setting | `CommonConfig.h`: one free bit of `NIN_CFG` | *After Sending* |
| Diagnostic Log *Between games* | `kernel/slippi_online/so_logfile.c` | a third value of the switch: lines are kept in the ring during a match and written only when no match is running (see below) |
| The receiver | `tools/replay_receiver.py` (new) | a small service on the PC: TCP 51460, saves as above |

No file is added to the release zip: `update_allows` and `upd_name_allowed` are untouched.

## Finding the PC

1. `replay_host=IP[:port]` in `slippwii/config.ini`, when it is there.
2. Otherwise a broadcast: the Wii sends `SLPWII WHO 1` to UDP 51460 on its subnet and takes the first
   receiver that answers `SLPWII HERE 1 <tcp port> <name>`. Two answers: the first one (named on the
   screen).

## The transfer

One TCP connection, messages framed as `<type:4><length:4><payload>` (big-endian), version 1.

```
Wii → PC   HELO  version, console id (the MAC in the replays' names), SlippWii version
Wii → PC   LIST  for each file on the card: kind (replay/log), name, size, CRC-32
PC  → Wii  WANT  for each: skip (have it whole) | from <offset> (have a part, or 0)
Wii → PC   FILE  name, size, CRC-32, offset; then the bytes from offset, in 16 KB pieces
PC  → Wii  SAVD  name: written, size and CRC checked, synced to disk   | REFU name, reason
Wii → PC   DONE
```

* **Resumable.** The receiver keeps a partial file (`.part`, named by the file's name and CRC) and
  answers `from <offset>`; nothing is sent twice.
* **The PC is the record of what was sent.** The Wii keeps no list: `LIST`/`WANT` decide it each
  time, so a card used with another PC, or files sent and kept, never confuse it.
* **Removal** (*Remove from card*) only after `SAVD` for that file. `REFU`, a dropped connection, a
  cancel or a timeout leave the file where it is.

## What may be sent and removed

* Replays: `Slippi/Game_<12 hex>_<YYYYMMDD>T<HHMMSS>.slp` on the drive the game is not on (as
  `SlippiFileWriter.c` names them), SD or USB.
* Logs: `slippwii/logs/{crash,desync,log}-N.txt` on the game's drive. Never `number.txt` or
  `newest.txt` (the kernel's numbering, `so_logfile.c`): the slots go round 1 to 8 and a removed
  slot is written again as a new file.
* Nothing else is ever read or removed: not the games, the account, the settings or the app.

A replay still being written cannot be on the card at this point (the action runs only in the
loader, before any game), but one cut short by a crash or a power cut can be: it is sent as it is
and the receiver names it `game.incomplete.slp` when its length word is 0 (`writeHeader`'s
placeholder), so it is never mistaken for a whole game.

## One game, one log

On the card there is one replay a game but one log a boot (`log-N.txt`, every game of that session).
The receiver cuts the log into games. Each line starts with the seconds since the kernel started
(`812.345 replay: saving ...`, `so_platform.c`), and the replay writer names each game's file as it
starts and ends (`SlippiFileWriter.c`):

```
812.345 replay: saving /Slippi/Game_001CBEB58782_20261121T211403.slp (on the SD card)
...
1015.902 game over: 12108 frames in ...        (online games: so_exi.c, log_game, at GAME!)
1016.010 replay: saved 3145728 bytes, last frame 10842
```

A game's `wii.log` is its lines from `replay: saving <its name>` to `replay: saved`, and the
end-of-game lines (`game ...`, from `log_game`) logged within 5 s either side of `replay: saved`:
the two come from different threads, in either order. Never past the next `replay: saving`. What is
outside every game (menus, matchmaking, the boot) goes to that day's `session.log`. A crash or
desync report goes into the folder of the game it happened in when its lines name that game's
replay, and to `reports/` otherwise. The pairing is by the file's name, never by the Wii's clock,
which is often wrong.

Tested on host with made-up sessions: five games (offline and online) cut exactly; summary lines
before and after `replay: saved`; a game with no `replay: saved` (a crash: up to the next game or
the end, named incomplete); a log that starts mid-game (the ring overflowed); lines with no game; a
replay with no log at all; a log line naming a replay that isn't on the card. And that the log's
scrubbing (`so_logfile_scrub`) leaves a replay's name in a line as it was.

## Diagnostic Log: *Between games*

Today *Yes* writes the log to the card between games and every 5 s in one, and those in-game writes
can hold up a pad for some milliseconds. *Between games* writes only when no match is running
(`so_exi_in_match()` and the replay writer's state), so a match is never held up for it. The ring
holds 16 KB: a match that logs more than that loses its earliest lines (the file says so); the
end-of-game lines, which `summary.txt` is made from, come last and are always kept.

## Rules, and the test of each

| Rule | Test |
|---|---|
| Nothing is removed without the PC's `SAVD` for it | core: the connection cut before, during and after each message; only `SAVD` removes |
| A wrong size or CRC is never saved | receiver: truncated, padded and altered uploads are refused, nothing left but the `.part` |
| A resumed upload is the same file | receiver + core: cut at random offsets, resumed, byte for byte the card's |
| Names stay names | receiver: `../`, `/`, `\`, NUL, overlong, wrong pattern: refused |
| Limits | receiver: a replay over 16 MB, a log over 1 MB, more than 4096 files in a `LIST`: refused |
| Sending twice is harmless | receiver: a whole file sent again is skipped by `WANT` |
| *Keep on card* never removes | core |
| Only the files above are touched | core: a card with other files (the counters, the account, games) is the same after |
| Removing logs keeps the kernel's numbering | kernel host test: logs removed, the next log is the next slot |
| *Between games* never writes in a match | kernel host test: writes only while no match runs; the ring's overflow is said in the file |
| Nothing changes for a player who never presses it | all existing tests; the menu as before |

Where they run:

1. **Host unit tests** (Linux or Windows, plain gcc): the core, beside `update/`'s; the kernel's
   change in `Nintendont/kernel/slippi_online/tests/`.
2. **Receiver tests**: `tools/test_replay_receiver.py`.
3. **End to end on a PC**: `tools/test_replay_send.py` builds the core against fake SD/USB folders
   (the stand-ins in `loader/tests/host/`) and sends to the real receiver over localhost: whole,
   cut and resumed, refused, removal on and off.
4. **On a Wii**, written up for the pull request: offline and online sessions, then send; every
   replay on the PC identical to the card's copy taken before; the cable pulled mid-send (only
   confirmed files removed); *Between games* against *No* in per-game numbers (waited frames,
   ping, rollbacks).

## Open

* Whether the loader's screen and network allow cancelling with **B** mid-piece without waiting out
  a send (a non-blocking socket, as `upd_plat_connect` already uses).
* How fast a Wii sends (a guess: 0.3 to 1 MB/s): a first measurement sets the progress screen's
  expectations.
* A free `NIN_CFG` bit, or `config.ini`, for *After Sending*.

#pragma once

/* Minimal SD-card logger + settings store.
 *
 * exlaunch builds with `-nostartfiles` and an empty LIBS, so the module has no
 * way to write files by default and `svcOutputDebugString` output is invisible
 * on retail hardware without a debugger attached.  This links libnx for the
 * `fs` service wrappers and writes plain text next to the game.
 *
 * Never call Open() from the render hook every frame -- it mounts a
 * filesystem.  Call it once through Step(), then append small lines.
 *
 * Line() opens and closes the file per line, so every line is flushed to the
 * card before it returns: a crash or a forced power-off costs at most the line
 * in flight.  That is six service calls per line, so the writer holds a token
 * bucket (12 lines per 2 s) and a cap (8000 lines / 2 MB).  LineLow() is for
 * periodic chatter and is refused while the bucket is half spent, so a
 * heartbeat can never crowd out an event line.  Before the channel is open
 * every call is a no-op. */

namespace sdlog {

/* Tries the SD card root only.  Returns false when it is not writable; safe to
 * call again (each retry repeats only the part that failed).
 *
 * There used to be two extra channels here -- /switch and the game's own save
 * data -- and both were removed after SDLOG-1 froze a console on hardware:
 *   - the save-data fallback calls fsOpen_SaveData, which blocks forever while
 *     the game already holds that save (i.e. always, once play has started).
 *     That was the hang.
 *   - the create/write/read-back probe cost three extra fs round trips and
 *     left 0-byte files behind when a step failed.
 * The log is now written directly: create, open, write, close. */
bool Open();
bool IsOpen();
void Line(const char* fmt, ...);
/* v613: the write gate.  The update hook closes it while there is no world.
 *
 * The console froze on the character-creation screen -- music playing, picture
 * stopped, log ending mid-session.  That screen is where the game hands the
 * display to the system software keyboard, an applet switch, and this channel
 * costs fs IPCs on the main thread.  Keeping the card quiet until a location
 * exists removes the channel from that window entirely. */
void SetGate(bool on);/* Periodic chatter: same file, but refused when the bucket is half spent. */
void LineLow(const char* fmt, ...);
/* v620: bypasses the token bucket entirely.  For the stage marks only -- the
 * freeze bisect needs the LAST mark to be the truth, and the bucket's silent
 * drops made "last line" ambiguous (a frame could lose u10 but keep u9).
 * Volume: ~13 short lines per frame, ~20 KB/s against a 16 MB cap. */
void LineForce(const char* fmt, ...);
void Close();

/* v646: release the WHOLE fs channel, not just the write handle.
 *
 * v645 proved on hardware that the character-creation hang disappears when
 * smInitialize()/fsInitialize()/fsOpenSdCardFileSystem() never run.  SetGate
 * (false) only closed the FsFile; the sm:/fsp-srv/SD-card filesystem sessions
 * survived the trip back to the title screen and were still held during the
 * system-keyboard applet switch.
 *
 * Call this once when the player leaves a world.  It closes the write handle,
 * the filesystem, and both service sessions, then resets the step machine so
 * a later world can open a fresh channel from step 0.  After it returns the
 * module holds NO fs session and every Line*() is a no-op (g_wrote=false),
 * which is exactly the cold-boot state that never freezes.
 *
 * v647: this is now a ONE-WAY door.  A play session with v646 showed five
 * open/close cycles (one per long menu or trip to the title), followed by a
 * flash-exit with no erpt report.  Repeatedly cycling sm:/fsp-srv sessions is
 * the only behaviour v646 added, so the channel now opens once and never
 * reopens after Shutdown().  The log covers the first world of the process;
 * restarting the game starts a fresh one.
 *
 * Safe to call repeatedly (idempotent) and when nothing was ever opened. */
void Shutdown();
bool ShutdownDone();

/* Advances the open sequence by exactly one blocking fs call, and never loops.
 *
 * Call it once per frame until IsOpen(); it stops by itself on the first
 * failure.
 *
 * SDLOG-4 note: the on-screen readout never worked (`kMapStage == 14` against
 * kMapStage = 327 compiles the block away), so the SD card carries the whole
 * diagnosis.  Each step drops a marker file whose name holds the result, e.g.
 * `sdv_05_cr_00000000`, and `sdv_p5` is dropped before the step runs.  A
 * directory listing therefore says both what failed and, from the last `p`
 * marker, what hung.
 *
 * This used to start a worker thread (SDLOG-2) so that a blocking call could
 * not freeze the picture.  That crashed the title: libnx builds every IPC
 * request in the buffer returned by armGetTls(), which is TPIDRRO_EL0, and a
 * raw svcCreateThread thread cannot be given its own -- the register is
 * per-thread, set by the kernel, and userland has no SVC for it.  The worker
 * therefore shared the game's IPC buffer. IPC stays on the owning thread. */
void Step();

/* One-byte on/off setting stored next to the log (`sdv_map.cfg`).
 *
 * The FastAnimations checkbox is an element we append to the options list
 * ourselves, so it is not part of GameOptions and its checked state is
 * recreated from scratch every launch.  Mirroring the value into a file is
 * the only way to make "ticked" survive a restart.
 *
 * ReadSetting() returns -1 when there is no readable file (first run), so a
 * caller can tell "off" from "never chosen".  WriteSetting() is meant to be
 * called once when the box is toggled, not every frame; it re-reads what it
 * wrote and returns false if the value did not stick. */
int ReadSetting();
bool WriteSetting(int value);

/* v528: the auto sell box keep percentage (0 / 25 / 50 / 75 / 100), stored next
 * to the log in `sdv_keep.cfg` as a fixed four bytes ("050\n").
 *
 * This is deliberately NOT carried in a netcode field any more: the switch mask
 * uses Farmer.disconnectDay, and hardware showed that the top byte of that int
 * is rewritten by something we do not control (v393: 5A5A0EFE read back as
 * 985A0EFE).  The keep ratio lived up there in v524-v527 and was silently reset
 * to 0 %, which is exactly why a 50 % box still sold everything.
 *
 * ReadKeepPct() returns -1 when there is no readable file (first run). */
int ReadKeepPct();
bool WriteKeepPct(unsigned pct);

/* Which store Open() settled on: 0 = none, 1 = SD root, 2 = SD /switch,
 * 3 = save data.  Anything other than 1 is worth reporting, because the SD
 * root is where a log file is expected to appear. */
int Channel();

/* Result codes of each mount step, so a failed mount can be reported on
 * screen when there is no log file to read. 0 = that step succeeded.
 *   DiagSm / DiagInit / DiagOpenFs / DiagCreate / DiagSaveData */
unsigned DiagSm();
unsigned DiagInit();
unsigned DiagOpenFs();
unsigned DiagCreate();
unsigned DiagSaveData();

/* Session-quota probe results, reported on screen because a failed quota raise
 * is the one failure that leaves no file to read.
 *   DiagRl     svcGetInfo(InfoType_ResourceLimit) result. 0 = handle obtained.
 *   DiagRlCur  SessionCountMax in use           (low 32 bits, -1 = unread)
 *   DiagRlLim  SessionCountMax upper bound      (low 32 bits, -1 = unread)
 *   DiagRlSet  svcSetResourceLimitLimitValue result. 0 = the raise went in.
 *
 * The three reads are separate from DiagSm so that a console which refuses the
 * READ can be told apart from one that refuses the WRITE: they are different
 * SVC numbers and the deployed npdm does not necessarily grant all of them. */
unsigned DiagRl();
unsigned DiagRlCur();
unsigned DiagRlLim();
unsigned DiagRlSet();

/* Accessors kept for the record.  Nothing draws them any more: the on-screen
 * block that used to print them was gated on `kMapStage == 14` against
 * kMapStage = 327, so it was compiled out and never appeared on hardware.
 * The marker files listed on Step() are the real channel.
 *   DiagOpenFile  fsFsOpenFile(FsOpenMode_Write) result
 *   DiagWrite     fsFileWrite result
 *   DiagPhase     the step currently running (set before it starts):
 *                 1 quota  2 sm:  3 fs-init  4 sd-open  5 create
 *                 6 file-open  7 set-size  8 write  9 flush  0xA done
 *                 0xE? = that step returned a failure code */
unsigned DiagOpenFile();
unsigned DiagWrite();
unsigned DiagPhase();

}  // namespace sdlog

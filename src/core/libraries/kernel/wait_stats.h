// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel::WaitStats {

/// SHADPS4_LOG_WAITS=<seconds>: every so many seconds, log where the title's threads blocked in
/// the kernel (sleeps, event queues, semaphores, condition variables, event flags, joins): the
/// thread, the call, the title's call stack (return addresses found on the stack), how often per
/// second, and how long in total, on average and at most. Off (no cost) when unset.
bool Enabled();

/// Put one at the top of a blocking kernel call (use WAIT_STATS_SCOPE); only the outermost one on
/// a thread records. `return_address` is the call's own return address, where the search for the
/// title's frames starts.
class Scope {
public:
    Scope(const char* what, void* return_address);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* what;
    void* return_address;
    s64 start_ns{-1};
};

/// Super Stardust Ultra VR (a debugging aid): stops (on) or restarts the game's world while the
/// game keeps drawing it. Stopping makes the main loop skip every world update (its jump that
/// skips updates after a slow one, at 0x426f09, becomes unconditional) and sets the update step
/// (frame timer at [0x9b6250] + 0xc) near zero, which also scales the loop's after-update call.
/// Repeated calls are cheap. Returns false when the game's timer is not where expected; when only
/// the jump is not as expected it logs that and freezes with the step alone.
bool SsuFreezeWorld(bool on);

/// Super Stardust Ultra VR: SHADPS4_SSU_FPS=<hz>|headset sets the game's own frame rate, i.e. its
/// fixed update step and its frame cap (frame timer at [0x9b6250]: +8 rate, +0xc 1/rate, +0x10 cap
/// rate, +0x14 1/cap; the main loop reads the step and cap every frame, the level-start code copies
/// the rate into the cap). "headset" follows the headset's refresh rate (snapped to 60/72/80/90/
/// 120/144 Hz when within 1.5%), 60-240 Hz. Unset: the game's own 59.94. The game's few settings
/// counted in frames (score multiplier countdown, Lurker speed and centipede head-scale factors
/// a frame) are rescaled to last as long in seconds. Call once per vblank;
/// does its work at most every 100 ms and not while SsuFreezeWorld holds the world.
void SsuFollowGameRate();

/// Super Stardust Ultra VR: SHADPS4_SSU_DRAW_FPS=<hz>|headset draws the game that many frames a
/// second while its world keeps its own 59.94 updates a second (and so plays exactly as at 60).
/// The main loop (sub_426e10) does one world update and then waits out the minimum frame from a
/// time mark that each update moves on by the update step; patched here at load, it moves the mark
/// on in ticks of updates/ticks steps (1/2 at 120 Hz), waits out the minimum frame times that, and
/// does the world update (still with the whole step) on that many of the ticks only. The loop's
/// after-update calls (the game logic's per-frame part and two more, given the frame's time) count
/// some things in frames (a Rock Crusher shot lives 44 of them), so they run only in frames with a
/// world update, given the time since they last ran: once per update, as at 60. Every frame draws.
/// The head pose is
/// read in the world update (Game_Logic_Impl::update, sub_261610, its block 0x26184a-0x261b14: the
/// tracker result for the next flip into the record the cameras read, every frame); a frame
/// without an update runs that block alone first, so every frame is drawn from a fresh pose. Call
/// once when the game's executable is loaded, before it runs; does nothing for other titles or
/// code.
void SsuPatchFrameLoop();

/// Super Stardust Ultra VR (CUSA05951): its particle systems (Particle_System_Impl, updated once a
/// drawn frame by sub_422d50 for each view's systems and the background's) always age by a fixed
/// 1/59.94 s (rodata 0x5b90f4, read only at 0x422e1a and 0x422e80), and a Rock Crusher shot lives
/// only as long as its effect's particles. Drawing more often than 59.94 Hz (SHADPS4_SSU_DRAW_FPS)
/// or running the world faster (SHADPS4_SSU_FPS) so aged them faster: shots died at half range at
/// 120 Hz. With either set, those two loads read a slot instead (SsuSetParticleStep: the time one
/// drawn frame stands for). Call once when the game's executable is loaded, before
/// SsuPatchFrameLoop.
void SsuPatchParticleStep();

/// The particle step of SsuPatchParticleStep, in seconds a drawn frame (does nothing without it).
void SsuSetParticleStep(float step);

/// Super Stardust Ultra VR (CUSA05951), with SHADPS4_SSU_FPS: the few lerp fractions written into
/// the code rather than registered as debug values (centipede and token headings, a 2D follow, a
/// versus marker, a menu slide; ssuvr-work\analysis\hardcoded_lerps.py) are redirected to slots
/// SsuFollowGameRate rescales with the world rate. Call once when the game's executable is loaded.
void SsuPatchRateConstants();

/// Super Stardust Ultra VR (CUSA05951), with SHADPS4_SSU_FPS: the Gold Melter beam places each segment
/// one update's travel beyond its newer neighbour while it adds segments 60 times a second at any rate,
/// so above 60 Hz the beam came out short. Moves that placement to a cave that keeps the 60 Hz gap and
/// catches up k = 59.94/rate of the way per update (factors set by SsuFollowGameRate, 1 at 60 Hz).
/// Call once when the game's executable is loaded.
void SsuPatchBeamSpacing();

/// Super Stardust Ultra VR test knob: SHADPS4_SSU_COUNT=<eboot offset hex>,... counts the calls of
/// those functions (each gets a jump at its start to a counter stub in the executable's padding;
/// its first instructions must not be relative). SsuReportCalls logs the calls a second and the
/// last caller every 5 s. Call SsuCountCalls once at load, after SsuPatchFrameLoop.
void SsuCountCalls();
void SsuReportCalls();

/// Super Stardust Ultra VR: its own "calibrate" (the calibrate screen, the pause menu) stores minus
/// the headset's position of that moment at 0xcde454 (x, y, z) and adds it to every position
/// after; it never looks at the console's reset of the view. So after a reset of the view here
/// (the headset's own, the PS button, OPTIONS held, ...) the title is calibrated the same way, from
/// its own latest tracker result, a few results after the reset: where the head is then is the
/// centre again. Call with Runtime::Recenters() on each of the headset's tracker results; does
/// nothing for other titles.
void SsuFollowRecenter(u32 recenters);

/// Test knob SHADPS4_SSU_SCAN=<a>,<b>,<until> (seconds on the input script's clock): finds the float
/// triples of planet / flying / camera lengths that hold still between scans at <a> and <b>, watches
/// them until <until> and writes the ones that moved to user/log/ssu-moved.csv (start, end, angle).
/// Finds the player's position and how far it flew. Call once per vblank (starts its own thread).
void SsuProbe();

} // namespace Libraries::Kernel::WaitStats

#define WAIT_STATS_SCOPE(name)                                                                     \
    ::Libraries::Kernel::WaitStats::Scope wait_stats_scope {                                       \
        name, __builtin_return_address(0)                                                          \
    }

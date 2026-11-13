#!/usr/bin/env python3
"""Automated proof that the quality controller gives up the denoiser before it gives up anything else.

The adaptive controller has three things it can trade away when the frame rate slips: the SVGF
filter, the samples per pixel, and the trace resolution. On this machine the filter is by far the
most expensive of the three at a large window - at 3840x2100 it is the difference between 5.6 fps
and 58.8 - yet an earlier build never surrendered it, because its cost had been folded into a
per-sample constant and the escape hatch was gated on conditions the oscillation kept resetting.

This test runs the real renderer in a real window and reads the decisions back out of the log file
(the binary is a GUI-subsystem build: its log goes to build\\bin\\raytracer.log, not to stdout).

Two cases:

  large    3840x2100 for 25 s - must surrender the denoiser, first, and then average at least
                               50 fps for the run
  control  1280x720  for 20 s - must leave the denoiser alone and still run at about 60 fps

Judgement is made on run aggregates (the frame count and average fps of the auto-exit summary over
20-25 seconds) rather than on instantaneous readings, so the window creation, the first frame's
shader dispatches and the reallocations a quality change performs - a couple of seconds of settling
on this machine - cannot decide the outcome by themselves. The log carries no timestamps and none
are needed.

`--selfcheck` runs the judgement against invented logs instead of the renderer, to show that the
ordering assertion rejects a controller that shrinks the image first and accepts one that surrenders
the filter first. A test that cannot fail is not evidence, and the real runs cannot be relied on to
produce the wrong order on demand.

Exits non-zero, printing the relevant log lines, on any failure.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'bin', 'raytracer.exe')
LOG = os.path.join(ROOT, 'build', 'bin', 'raytracer.log')

# The decision lines are the machine-readable contract of the controller. They are emitted for every
# change of the denoiser, the sample count or the trace scale, and nothing else writes them.
RE_START = re.compile(r'^quality start: denoise (on|off)')
RE_OFF = re.compile(r'^quality decision: denoise off \(')
RE_ON = re.compile(r'^quality decision: denoise on \(')
RE_SPP = re.compile(r'^quality decision: samples (\d+) -> (\d+) \(')
RE_SCALE = re.compile(r'^quality decision: scale ([0-9.]+) -> ([0-9.]+) \(')
RE_EXIT = re.compile(r'^auto-exit after ([0-9.]+) s: (\d+) frames \(([0-9.]+) fps average\)')


class Run:
    """A run of the renderer - or an invented one - and the decisions it left in the log."""

    def __init__(self, args=(), seconds=0.0):
        self.args = list(args)
        self.seconds = seconds
        self.lines = []
        self.rc = 0
        self.exit = None      # (seconds, frames, fps) from the auto-exit summary
        self.start = None     # 'on' / 'off' from the start-up line
        self.decisions = []   # (line index, kind, line) in log order

    def decisions_of(self, kind):
        """(line index, line text) for every decision of this kind, in log order."""
        return [(i, ln) for i, k, ln in self.decisions if k == kind]

    def first(self, kind):
        got = self.decisions_of(kind)
        return got[0] if got else None


def parse(r, lines):
    """Fill a Run from log text. Kept separate from launching so invented logs can be judged too."""
    r.lines = [ln.rstrip('\r\n') for ln in lines]
    for i, ln in enumerate(r.lines):
        m = RE_EXIT.match(ln)
        if m:
            r.exit = (float(m.group(1)), int(m.group(2)), float(m.group(3)))
        m = RE_START.match(ln)
        if m and r.start is None:
            r.start = m.group(1)
        m = RE_OFF.match(ln)
        if m:
            r.decisions.append((i, 'off', ln))
            continue
        m = RE_ON.match(ln)
        if m:
            r.decisions.append((i, 'on', ln))
            continue
        m = RE_SPP.match(ln)
        if m:
            r.decisions.append((i, 'spp', ln))
            if int(m.group(2)) < int(m.group(1)):
                r.decisions.append((i, 'spp-down', ln))
            continue
        m = RE_SCALE.match(ln)
        if m:
            r.decisions.append((i, 'scale', ln))
            if float(m.group(2)) < float(m.group(1)) and float(m.group(2)) < 1.0:
                # Only a target smaller than the window counts as pixels given up. A move from 0.80
                # to 0.90 is the controller handing some of the resolution back, which is a climb and
                # is allowed any time.
                r.decisions.append((i, 'scale-down', ln))
            continue
    return r


def kill_strays():
    """Stop-Process on anything left over: a stray window would race on the shared log file."""
    try:
        subprocess.run(['powershell', '-NoProfile', '-Command',
                        'Get-Process raytracer -ErrorAction SilentlyContinue | Stop-Process -Force'],
                       capture_output=True, timeout=30)
    except Exception as e:                       # a missing shell must not fake a pass
        print('note: could not sweep stray processes (%s)' % e)
    time.sleep(0.4)


def run_renderer(args, seconds):
    kill_strays()
    try:
        os.remove(LOG)
    except FileNotFoundError:
        pass
    if not os.path.exists(EXE):
        raise SystemExit('no executable at %s - build with .\\build.ps1 first' % EXE)
    r = Run(args, seconds)
    # cwd matters: the executable looks for scene.pak next to itself and writes raytracer.log there.
    with open(os.devnull, 'wb') as sink:
        p = subprocess.Popen([EXE] + list(args), cwd=os.path.dirname(EXE),
                             stdout=sink, stderr=sink)
        # --autoexit terminates the run by itself; the extra allowance is for the shutdown, which
        # waits for the device and would hang the test rather than fail it.
        deadline = time.time() + seconds + 45
        while p.poll() is None and time.time() < deadline:
            time.sleep(0.25)
        if p.poll() is None:
            p.kill()
            raise SystemExit('raytracer %s did not exit within %.0f s'
                             % (' '.join(args), seconds + 45))
    r.rc = p.returncode
    with open(LOG, 'r', encoding='utf-8', errors='replace') as f:
        parse(r, f)
    return r


def judge(r, want_off, min_fps):
    """Everything the two real cases check, as a list of complaints. Empty means the run is correct.

    Split out from launching so that --selfcheck can put invented decision orders through the very
    same code the real verdict comes from.
    """
    problems = []
    if r.rc != 0:
        problems.append('raytracer exited with code %s' % r.rc)
        return problems
    if r.exit is None:
        problems.append('no auto-exit summary line: the run never reached its --autoexit deadline')
        return problems
    if r.exit[1] < 100:
        problems.append('only %d frames rendered in %.1f s - the run never really got going'
                        % (r.exit[1], r.exit[0]))
    if r.start is None:
        # Carried on regardless: a controller that emits no decision lines at all is failing three
        # separate requirements - the log line, the give-up and the frame rate - and reporting only
        # the first of them hides the two that matter.
        problems.append("no 'quality start:' line, so the run never said whether the denoiser was on")
    elif r.start != 'on':
        problems.append('the denoiser should be on at the start of a run, saw %r' % r.start)

    off = r.first('off')
    if want_off and off is None:
        problems.append('no "quality decision: denoise off" line, where the filter costs more than '
                        'the rest of the frame put together')
    if not want_off and off is not None:
        problems.append('gave up the denoiser at a size where the frame is nowhere near the budget: '
                        '%s' % off[1])
    if want_off and off is not None:
        # No scale reduction below the window's own size and no cut in samples may precede it.
        earlier = [d for d in r.decisions_of('scale-down') + r.decisions_of('spp-down')
                   if d[0] < off[0]]
        if earlier:
            problems.append('the denoiser was still on while pixels were already being given up: %s '
                            'came before the denoise-off decision at line %d'
                            % (earlier[0][1], off[0] + 1))
    fps = r.exit[2]
    if fps < min_fps:
        problems.append('averaged %.1f fps, want at least %.0f' % (fps, min_fps))
    return problems


def show(r, title):
    print('--- %s%s' % (title, ': raytracer ' + ' '.join(r.args) if r.args else ''))
    print('    exit code %s, %d log lines' % (r.rc, len(r.lines)))
    if r.start:
        print('    start-up: denoiser %s' % r.start)
    if r.exit:
        print('    ran %.1f s, %d frames, %.1f fps average' % r.exit)
    print('    decisions:')
    if not r.decisions:
        print('      (none)')
    for i, kind, ln in r.decisions:
        print('      line %4d  %-10s %s' % (i + 1, kind, ln))


def tail(r, n=12):
    print('    last %d log lines:' % n)
    for ln in r.lines[-n:]:
        print('      ' + ln)


def fail(r, title, problems):
    show(r, title)
    tail(r)
    print('\nFAIL: %s' % '; '.join(problems))
    sys.exit(1)


def case(title, args, seconds, want_off, min_fps):
    print('=== %s' % title)
    r = run_renderer(args, seconds)
    problems = judge(r, want_off, min_fps)
    if problems:
        fail(r, title, problems)
    off = r.first('off')
    detail = ('denoiser on at start, given up at log line %d with nothing given up before it'
              % (off[0] + 1)) if off else 'denoiser kept on for the whole run'
    print('PASS: %s, run averaged %.1f fps over %.1f s' % (detail, r.exit[2], r.exit[0]))
    return r


def summary(label, r):
    print('  %-16s %s: %.1f fps, %d frames, %d decision lines'
          % (label, ' '.join(r.args), r.exit[2], r.exit[1], len(r.decisions)))


# ---------------------------------------------------------------- the self-check
# Invented logs, judged by the same judge() the real verdicts come from. Each one differs from the
# correct order in exactly one way, so a pass here means that particular wrongness would be caught.

GOOD_LARGE = [
    'quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: denoise off (late fraction 0.31, scale 1.00, spp 1)',
    'quality decision: samples 1 -> 3 (gpu 6.0 ms, budget 9.2 ms)',
    'auto-exit after 25.0 s: 1400 frames (56.0 fps average)',
]
SHRINKS_PIXELS_FIRST = [
    'quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: scale 1.00 -> 0.79 (gpu 11.0 ms, budget 9.2 ms)',
    'quality decision: denoise off (late fraction 0.31, scale 0.79, spp 1)',
    'auto-exit after 25.0 s: 1400 frames (56.0 fps average)',
]
CUTS_SAMPLES_FIRST = [
    'quality start: denoise on, scale 1.00, spp 4, filter 0.86 ms per megapixel',
    'quality decision: samples 4 -> 2 (gpu 12.4 ms, budget 9.2 ms)',
    'quality decision: denoise off (late fraction 0.31, scale 1.00, spp 2)',
    'auto-exit after 25.0 s: 1400 frames (56.0 fps average)',
]
NEVER_SURRENDERS = [
    'quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: scale 1.00 -> 0.42 (gpu 11.0 ms, budget 9.2 ms)',
    'auto-exit after 25.0 s: 1400 frames (56.0 fps average)',
]
STAYS_SLOW = [
    'quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: denoise off (late fraction 0.31, scale 1.00, spp 1)',
    'auto-exit after 25.0 s: 400 frames (16.0 fps average)',
]
CONTROL_GIVES_UP = [
    'quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: denoise off (late fraction 0.10, scale 1.00, spp 1)',
    'auto-exit after 20.0 s: 1170 frames (58.5 fps average)',
]
HANDS_RESOLUTION_BACK = [
    'quality start: denoise on, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: denoise off (late fraction 0.31, scale 1.00, spp 1)',
    'quality decision: scale 0.80 -> 0.95 (gpu 4.0 ms, budget 9.2 ms)',
    'auto-exit after 25.0 s: 1400 frames (56.0 fps average)',
]
STARTS_WITHOUT_FILTER = [
    'quality start: denoise off, scale 1.00, spp 1, filter 0.86 ms per megapixel',
    'quality decision: denoise off (late fraction 0.31, scale 1.00, spp 1)',
    'auto-exit after 25.0 s: 1400 frames (56.0 fps average)',
]


def selfcheck():
    """Prove judge() rejects the wrong orders. Nothing here touches the renderer."""
    print('=== self-check: does the judgement reject the behaviour it is supposed to reject?')
    cases = [
        # (name, log lines, want_off, min_fps, must_be_accepted)
        ('correct order at a large window', GOOD_LARGE, True, 50.0, True),
        ('correct order, resolution handed back afterwards', HANDS_RESOLUTION_BACK, True, 50.0, True),
        ('shrinks the image before surrendering the filter', SHRINKS_PIXELS_FIRST, True, 50.0, False),
        ('cuts samples before surrendering the filter', CUTS_SAMPLES_FIRST, True, 50.0, False),
        ('never surrenders the filter at all', NEVER_SURRENDERS, True, 50.0, False),
        ('surrenders the filter but stays slow', STAYS_SLOW, True, 50.0, False),
        ('surrenders the filter where it need not', CONTROL_GIVES_UP, False, 55.0, False),
        ('starts with the filter already off', STARTS_WITHOUT_FILTER, True, 50.0, False),
    ]
    bad = []
    for name, lines, want_off, min_fps, accept in cases:
        problems = judge(parse(Run(), lines), want_off, min_fps)
        ok = (not problems) if accept else bool(problems)
        print('    %-46s %s' % (name, 'accepted' if not problems else 'rejected'))
        if not ok:
            bad.append('%s: expected %s, judge said %r'
                       % (name, 'accept' if accept else 'reject', problems or 'nothing wrong'))
    if bad:
        for b in bad:
            print('FAIL: self-check: %s' % b)
        sys.exit(1)
    print('PASS: the judgement rejects every wrong order and accepts the right one')


def main():
    if '--selfcheck' in sys.argv[1:]:
        selfcheck()
        return 0
    if not os.path.exists(EXE):
        raise SystemExit('no executable at %s - build with .\\build.ps1 first' % EXE)
    selfcheck()
    print()
    large = case('large window: the denoiser must go first, and the frame rate must recover',
                 ['--window', '3840x2100', '--verbose', '--autoexit', '25'], 25, True, 50.0)
    control = case('control: at 1280x720 this machine is comfortable, so nothing should be surrendered',
                   ['--window', '1280x720', '--verbose', '--autoexit', '20'], 20, False, 55.0)
    kill_strays()
    print('\nAll checks passed.')
    summary('large', large)
    summary('control', control)
    return 0


if __name__ == '__main__':
    sys.exit(main())

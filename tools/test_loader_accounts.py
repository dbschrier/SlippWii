"""Runs the loader's Slippi accounts (Nintendont/loader/source/accounts.c) on a PC, against fake SD
and USB folders with scripted buttons: the header line, signing in, out and forgetting, the code
histories that go with each account, damaged and duplicate files, a drive that can't be written, a
power cut half way through a sign-in, the copy script's old backups, and the question at Melee's
start. The loader's libogc/FatFS/screen calls are stand-ins (Nintendont/loader/tests/host/); the
screen is printed. No disc image is needed.

    python tools/test_loader_accounts.py [--cc gcc] [-v]
"""
import argparse
import json
import os
import shlex
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOADER = ROOT / 'Nintendont/loader'


class Fail(Exception):
    pass


def account(name, code, uid=None):
    return json.dumps({'uid': uid or f'uid-{code}', 'playKey': f'secret-key-{code}', 'connectCode': code,
                       'displayName': name}).encode()


def history(*codes):
    """A direct-codes.bin as the kernel writes it (so_exi.c): "SLDC", a count, then per code its
    length in bytes and 16 bytes of Shift-JIS."""
    out = bytearray(b'SLDC' + bytes([len(codes)]))
    for c in codes:
        sj = c.translate({ord('#'): '＃'} | {i: i + 0xFEE0 for i in range(0x21, 0x7F) if i != ord('#')}).encode('shift_jis')
        out += bytes([len(sj)]) + sj.ljust(16, b'\0')
    return bytes(out)


ALICE = account('Alice', 'ALIC#123')
BOB = account('Bob', 'BOBB#456')
SAM = account('Sam', 'SAMM#789')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('-v', action='store_true', help='print every screen')
    args = ap.parse_args()
    failures = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = tmp / 'accounts_host.exe'
        host = LOADER / 'tests/host'
        subprocess.run([*shlex.split(args.cc), '-Wall', '-Werror', '-O1', '-std=gnu89', '-DSW_NO_MAIN',
                        '-I', str(host / 'include'), '-I', str(host), '-I', str(LOADER / 'include'),
                        str(host / 'loader_stubs.c'), str(host / 'accounts_main.c'),
                        str(LOADER / 'source/accounts.c'), '-o', str(exe)], check=True)
        cards = 0

        def card(user=None, saved=(), usb=False, files=None):
            """A fake SD card (and USB drive): user.json, accounts/ (name -> bytes), any other files."""
            nonlocal cards
            cards += 1
            sd = tmp / f'sd{cards}'
            (sd / 'slippwii').mkdir(parents=True)
            if user is not None:
                (sd / 'slippwii/user.json').write_bytes(user)
            if saved:
                (sd / 'slippwii/accounts').mkdir()
                for name, data in saved:
                    (sd / 'slippwii/accounts' / name).write_bytes(data)
            for rel, data in (files or {}).items():
                (sd / rel).parent.mkdir(parents=True, exist_ok=True)
                (sd / rel).write_bytes(data)
            u = None
            if usb:
                u = tmp / f'usb{cards}'
                (u / 'slippwii').mkdir(parents=True)
            return sd, u

        def run(what, sd, usb=None, answers='', game_dev='sd'):
            env = dict(os.environ, SW_SD=str(sd), SW_GAME_DEV=game_dev, SW_ANSWERS=answers)
            if usb:
                env['SW_USB'] = str(usb)
            res = subprocess.run([str(exe), what], env=env, capture_output=True, text=True)
            if args.v:
                print(res.stdout)
            out = res.stdout
            if res.returncode != 0:
                raise Fail(f'exit {res.returncode}\n{out}{res.stderr}')
            if 'secret-key' in out:
                raise Fail('a play key was shown on the screen\n' + out)
            return out

        def header(out):
            return [line for line in out.splitlines() if line.startswith('HEADER')][-1]

        def case(title, fn):
            nonlocal failures
            try:
                fn()
                print(f'ok   {title}')
            except Fail as e:
                print(f'FAIL {title}: {e}')
                failures += 1

        def expect(cond, text, out=''):
            if not cond:
                raise Fail(text + ('\n' + out if out else ''))

        # ---- the header ----

        def header_signed_in():
            sd, _ = card(user=ALICE)
            out = run('header', sd)
            expect(header(out) == 'HEADER+ Account : Alice (ALIC#123)', 'header', out)
        case('header: who is signed in, in green', header_signed_in)

        def header_none():
            sd, _ = card()
            expect(header(run('header', sd)) == 'HEADER! Account : none (online off)', 'nothing on the card')
            sd, _ = card(saved=[('BOBB-456.json', BOB), ('SAMM-789.json', SAM)])
            expect(header(run('header', sd)) == 'HEADER! Account : none (2 saved)', 'saved, nobody signed in')
        case('header: nobody signed in, in red (and how many are saved)', header_none)

        def header_damaged():
            sd, _ = card(user=b'{"connectCode":"X#1"}')
            expect(header(run('header', sd)) == 'HEADER! Account : user.json damaged', 'damaged')
        case('header: a damaged user.json', header_damaged)

        def header_long_name():
            sd, _ = card(user=account('A very long display name', 'LONG#999'))
            line = header(run('header', sd))
            expect(line == 'HEADER+ Account : LONG#999', line)
        case('header: a name too long for its place shows the connect code only', header_long_name)

        # ---- signing in ----

        def switch_saves_first():
            sd, _ = card(user=ALICE, saved=[('BOBB-456.json', BOB), ('BOBB-456.direct.bin', history('SAMM#789'))],
                         files={'slippwii/direct-codes.bin': history('ZZZZ#001', 'YYYY#002'),
                                'slippwii/teams-codes.bin': history('TEAM#001')})
            out = run('menu', sd, answers='DAB')
            expect('Signed in as Bob (BOBB#456).' in out, 'no message', out)
            expect(header(out) == 'HEADER+ Account : Bob (BOBB#456)', 'header after', out)
            expect((sd / 'slippwii/user.json').read_bytes() == BOB, 'user.json is not Bob')
            expect((sd / 'slippwii/accounts/ALIC-123.json').read_bytes() == ALICE, 'Alice was not saved first')
            expect((sd / 'slippwii/accounts/ALIC-123.direct.bin').read_bytes() == history('ZZZZ#001', 'YYYY#002'),
                   "Alice's Direct codes were not kept")
            expect((sd / 'slippwii/accounts/ALIC-123.teams.bin').read_bytes() == history('TEAM#001'),
                   "Alice's Teams codes were not kept")
            expect((sd / 'slippwii/direct-codes.bin').read_bytes() == history('SAMM#789'), "Bob's Direct codes not in place")
            expect(not (sd / 'slippwii/teams-codes.bin').exists(), "Alice's Teams codes left for Bob")
        case("switching: the account signed in is saved first, with its codes; the new one's codes take their place",
             switch_saves_first)

        def sign_in_already():
            sd, _ = card(user=ALICE)
            out = run('menu', sd, answers='AB')
            expect('Alice (ALIC#123) is signed in.' in out, 'no message', out)
            expect((sd / 'slippwii/user.json').read_bytes() == ALICE, 'changed')
        case('A on the account signed in: nothing changes', sign_in_already)

        def damaged_not_signed_in():
            sd, _ = card(saved=[('broken.json', b'{"uid":"x"}'), ('BOBB-456.json', BOB)])
            out = run('menu', sd, answers='DAB')  # (by name: Bob, then broken.json)
            expect('broken.json' in out and 'damaged' in out, 'not shown as damaged', out)
            expect('broken.json is not a Slippi account.' in out, 'no message', out)
            expect(not (sd / 'slippwii/user.json').exists(), 'signed in with a damaged file')
        case('a damaged file in accounts/: shown as damaged, and not signed in with', damaged_not_signed_in)

        def duplicates_once():
            sd, _ = card(user=BOB, saved=[('BOBB-456.json', BOB), ('bob-copy.json', BOB)])
            out = run('menu', sd, answers='B')
            rows = [line for line in out.splitlines() if 'BOBB#456' in line and line.startswith('SCREEN')]
            expect(len(rows) == 1, 'the same account listed twice', out)
        case('the same account saved twice: listed once', duplicates_once)

        def empty_help():
            sd, _ = card()
            out = run('menu', sd, answers='B')
            expect('No accounts saved on this card yet.' in out and 'slippwii/accounts/ on the SD card.' in out,
                   'no help', out)
        case('nothing saved: the screen says how to add an account', empty_help)

        def scrolling():
            saved = [(f'P{i:03d}-000.json', account(f'Player {i:02d}', f'P{i:03d}#000')) for i in range(14)]
            sd, _ = card(saved=saved)
            out = run('menu', sd, answers='D' * 13 + 'AB')
            expect('(more below)' in out and '(more above)' in out, 'no scrolling markers', out)
            expect(header(out) == 'HEADER+ Account : P013#000', 'the last one was not signed in', out)
        case('more accounts than fit: the list scrolls', scrolling)

        def write_fails():
            sd, _ = card(user=ALICE, saved=[('BOBB-456.json', BOB)])
            acc = sd / 'slippwii/accounts'
            acc.chmod(stat.S_IRUSR | stat.S_IXUSR)
            try:
                out = run('menu', sd, answers='DAB')
            finally:
                acc.chmod(stat.S_IRWXU)
            expect('Could not save the account signed in.' in out, 'no message', out)
            expect((sd / 'slippwii/user.json').read_bytes() == ALICE, 'switched without saving Alice')
            expect(header(out) == 'HEADER+ Account : Alice (ALIC#123)', 'header', out)
        case("a drive that can't be written: still signed in as before, and told", write_fails)

        def power_cut():
            sd, _ = card(saved=[('BOBB-456.json', BOB)], files={'slippwii/user.json.new': BOB})
            out = run('header', sd)
            expect(header(out) == 'HEADER+ Account : Bob (BOBB#456)', 'not recovered', out)
            expect((sd / 'slippwii/user.json').read_bytes() == BOB and not (sd / 'slippwii/user.json.new').exists(),
                   'user.json.new not put in place')
        case('a power cut between writing and placing user.json: finished at the next start', power_cut)

        # ---- signing out ----

        def sign_out_both_drives():
            sd, usb = card(user=ALICE, saved=[('ALIC-123.json', ALICE)], usb=True,
                           files={'slippwii/direct-codes.bin': history('ZZZZ#001')})
            (usb / 'slippwii/user.json').write_bytes(ALICE)
            (usb / 'slippwii/direct-codes.bin').write_bytes(history('XXXX#003'))
            out = run('menu', sd, usb=usb, game_dev='usb', answers='XB')
            expect('Signed out. Alice is still saved here.' in out, 'no message', out)
            expect(header(out) == 'HEADER! Account : none (1 saved)', 'header', out)
            for d in (sd, usb):
                expect(not (d / 'slippwii/user.json').exists(), f'user.json left on {d.name}')
                expect(not (d / 'slippwii/direct-codes.bin').exists(), f'the codes left on {d.name}')
            expect((sd / 'slippwii/accounts/ALIC-123.direct.bin').read_bytes() == history('XXXX#003'),
                   "the game drive's codes (the USB drive's) were not the ones kept")
        case('sign out: user.json and the codes off both drives; the account and its codes kept',
             sign_out_both_drives)

        def sign_out_unsaved():
            sd, _ = card(user=ALICE)
            out = run('menu', sd, answers='XB')
            expect((sd / 'slippwii/accounts/ALIC-123.json').read_bytes() == ALICE, 'Alice lost by signing out', out)
        case('sign out of an account only in user.json: it is saved first', sign_out_unsaved)

        def sign_out_and_back():
            sd, _ = card(user=ALICE, files={'slippwii/direct-codes.bin': history('ZZZZ#001')})
            run('menu', sd, answers='XB')
            out = run('menu', sd, answers='AB')
            expect(header(out) == 'HEADER+ Account : Alice (ALIC#123)', 'not signed in again', out)
            expect((sd / 'slippwii/direct-codes.bin').read_bytes() == history('ZZZZ#001'), 'her codes did not come back')
        case('sign out, then in again: the same account, with its codes', sign_out_and_back)

        def sign_out_nobody():
            sd, _ = card(saved=[('BOBB-456.json', BOB)])
            out = run('menu', sd, answers='XB')
            expect('Nobody is signed in.' in out, 'no message', out)
        case('sign out with nobody signed in: told so', sign_out_nobody)

        # ---- forgetting ----

        def forget_confirmed():
            sd, _ = card(user=ALICE, saved=[('ALIC-123.json', ALICE), ('BOBB-456.json', BOB),
                                            ('BOBB-456.direct.bin', history('SAMM#789'))])
            out = run('menu', sd, answers='DYAB')
            expect('Forget Bob (BOBB#456)?' in out and 'Bob was removed from this Wii.' in out, 'no question', out)
            expect(not (sd / 'slippwii/accounts/BOBB-456.json').exists(), 'Bob still saved')
            expect(not (sd / 'slippwii/accounts/BOBB-456.direct.bin').exists(), "Bob's codes still there")
            expect((sd / 'slippwii/user.json').read_bytes() == ALICE, 'Alice signed out')
        case('forget, A: the account and its codes are deleted', forget_confirmed)

        def forget_kept():
            sd, _ = card(saved=[('BOBB-456.json', BOB)])
            out = run('menu', sd, answers='YBB')
            expect('Forget Bob (BOBB#456)?' in out, 'no question', out)
            expect((sd / 'slippwii/accounts/BOBB-456.json').exists(), 'deleted after B')
        case('forget, B: nothing is deleted', forget_kept)

        def forget_signed_in():
            sd, usb = card(user=ALICE, saved=[('ALIC-123.json', ALICE)], usb=True,
                           files={'slippwii/direct-codes.bin': history('ZZZZ#001')})
            (usb / 'slippwii/user.json').write_bytes(ALICE)
            out = run('menu', sd, usb=usb, answers='YAB')
            expect(header(out) == 'HEADER! Account : none (online off)', 'still signed in', out)
            for d in (sd, usb):
                expect(not (d / 'slippwii/user.json').exists(), f'user.json left on {d.name}')
            expect(not (sd / 'slippwii/accounts/ALIC-123.json').exists(), 'still saved')
            expect(not (sd / 'slippwii/direct-codes.bin').exists(), 'her codes left')
        case('forget the account signed in: signed out too, on both drives', forget_signed_in)

        def forget_damaged():
            sd, _ = card(saved=[('broken.json', b'not json')])
            out = run('menu', sd, answers='YAB')
            expect('Delete broken.json?' in out, 'no question', out)
            expect(not (sd / 'slippwii/accounts/broken.json').exists(), 'not deleted')
        case('forget a damaged file: it is deleted', forget_damaged)

        # ---- the copy script's old backups ----

        def old_backups():
            sd, _ = card(user=ALICE, files={'slippwii/user.json.old-20261101-120000': BOB,
                                            'slippwii/user.json.old-20261102-120000': BOB,
                                            'slippwii/user.json.old-20261103-120000': ALICE})
            out = run('menu', sd, answers='B')
            expect((sd / 'slippwii/accounts/BOBB-456.json').read_bytes() == BOB, 'Bob not saved', out)
            names = sorted(p.name for p in (sd / 'slippwii/accounts').iterdir())
            expect(names == ['BOBB-456.json'], f'accounts/: {names}')
            left = sorted(p.name for p in (sd / 'slippwii').iterdir() if p.name.startswith('user.json.old'))
            expect(len(left) == 2, f'left: {left} (a backup of an account saved already stays)')
        case("the copy script's user.json.old-* backups: moved to accounts/, once each", old_backups)

        # ---- at Melee's start ----

        def start_pick():
            sd, _ = card(saved=[('BOBB-456.json', BOB), ('SAMM-789.json', SAM)])
            out = run('start', sd, answers='DA')
            expect('Who is playing?' in out and 'PICKED 1' in out, 'no question', out)
            expect((sd / 'slippwii/user.json').read_bytes() == SAM, 'Sam not signed in')
        case("Melee's start, saved accounts, nobody signed in: A signs in the one picked", start_pick)

        def start_none():
            sd, _ = card(saved=[('BOBB-456.json', BOB)])
            out = run('start', sd, answers='B')
            expect('PICKED -1' in out and not (sd / 'slippwii/user.json').exists(), 'B', out)
            sd, _ = card()
            out = run('start', sd)
            expect('PICKED 0' in out and 'Who is playing?' not in out, 'asked with nothing saved', out)
            sd, _ = card(user=ALICE)
            out = run('start', sd)
            expect('PICKED 1' in out and 'Who is playing?' not in out, 'asked when signed in', out)
        case("Melee's start: B is no account; not asked with nothing saved, or someone signed in", start_none)

    print('PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

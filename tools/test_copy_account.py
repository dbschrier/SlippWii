"""Runs release/slippwii/copy-account.ps1 (what "Copy my Slippi account to SD.bat" starts) against a
fake Slippi Launcher folder and a fake SD card: the account saved in slippwii/accounts, signed in when
nobody is, the Direct and Teams codes Slippi Launcher remembers turned into the Wii's files (the bytes
the kernel writes: so_exi.c), merged with the Wii's own, and a damaged history file left out.

Needs PowerShell: Windows PowerShell, or PowerShell 7 ("pwsh") on macOS or Linux.

    python tools/test_copy_account.py [--powershell pwsh] [-v]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'release/slippwii/copy-account.ps1'


class Fail(Exception):
    pass


def account(name, code):
    return json.dumps({'uid': f'uid-{code}', 'playKey': f'key-{code}', 'connectCode': code,
                       'displayName': name}).encode()


def full_width(code):
    """As Slippi Launcher keeps a code in direct-codes.json."""
    return ''.join(chr(ord(c) + 0xFEE0) if '!' <= c <= '~' else c for c in code)


def history(*codes):
    """direct-codes.bin as the kernel writes it (so_exi.c)."""
    out = bytearray(b'SLDC' + bytes([len(codes)]))
    for c in codes:
        sj = full_width(c).encode('shift_jis')
        out += bytes([len(sj)]) + sj.ljust(16, b'\0')
    return bytes(out)


ALICE = account('Alice', 'ALIC#123')
BOB = account('Bob', 'BOBB#456')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--powershell', default='powershell' if os.name == 'nt' else 'pwsh')
    ap.add_argument('-v', action='store_true')
    args = ap.parse_args()
    if not shutil.which(args.powershell):
        print(f'{args.powershell} not found: this test needs PowerShell (on macOS: brew install powershell)')
        return 2
    failures = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        n = 0

        def setup(pc_account, direct=None, teams=None, card_files=None):
            nonlocal n
            n += 1
            appdata = tmp / f'appdata{n}'
            launcher = appdata / 'Slippi Launcher/netplay/User/Slippi'
            launcher.mkdir(parents=True)
            (launcher / 'user.json').write_bytes(pc_account)
            for name, value in (('direct-codes.json', direct), ('teams-codes.json', teams)):
                if isinstance(value, (bytes, str)):
                    (launcher / name).write_text(value if isinstance(value, str) else value.decode(), encoding='utf-8')
                elif value is not None:
                    (launcher / name).write_text(json.dumps(
                        [{'connectCode': full_width(c), 'lastPlayed': t} for c, t in value], ensure_ascii=False),
                        encoding='utf-8')
            card = tmp / f'card{n}'
            (card / 'slippwii').mkdir(parents=True)
            (card / 'slippwii/netplay.gct').write_bytes(b'')
            for rel, data in (card_files or {}).items():
                (card / rel).parent.mkdir(parents=True, exist_ok=True)
                (card / rel).write_bytes(data)
            return appdata, card

        def run(appdata, card):
            env = dict(os.environ, APPDATA=str(appdata))
            res = subprocess.run([args.powershell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(SCRIPT),
                                  '-Here', str(card)], env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)
            if args.v:
                print(res.stdout, res.stderr)
            if res.returncode != 0:
                raise Fail(f'exit {res.returncode}\n{res.stdout}{res.stderr}')
            return res.stdout

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

        def first_account():
            appdata, card = setup(ALICE, direct=[('OLDD#001', 100), ('NEWW#002', 300), ('MIDD#003', 200)],
                                  teams=[('TEAM#001', 50)])
            out = run(appdata, card)
            s = card / 'slippwii'
            expect((s / 'accounts/ALIC-123.json').read_bytes() == ALICE, 'not saved in accounts', out)
            expect((s / 'user.json').read_bytes() == ALICE, 'not signed in', out)
            expect((s / 'direct-codes.bin').read_bytes() == history('NEWW#002', 'MIDD#003', 'OLDD#001'),
                   'Direct codes: not the bytes the kernel writes, newest first', out)
            expect((s / 'teams-codes.bin').read_bytes() == history('TEAM#001'), 'Teams codes', out)
            expect('4 connect code(s)' in out, 'not said', out)
        case('a card with nobody signed in: saved, signed in, its codes in place (newest first)', first_account)

        def someone_else():
            appdata, card = setup(ALICE, direct=[('NEWW#002', 300)],
                                  card_files={'slippwii/user.json': BOB, 'slippwii/direct-codes.bin': history('BOBS#999')})
            out = run(appdata, card)
            s = card / 'slippwii'
            expect((s / 'user.json').read_bytes() == BOB, 'Bob was signed out', out)
            expect((s / 'direct-codes.bin').read_bytes() == history('BOBS#999'), "Bob's codes changed", out)
            expect((s / 'accounts/ALIC-123.json').read_bytes() == ALICE, 'Alice not saved', out)
            expect((s / 'accounts/ALIC-123.direct.bin').read_bytes() == history('NEWW#002'), "Alice's codes not saved", out)
            expect('Slippi Account' in out and 'Bob (BOBB#456) is signed in' in out, 'not told how to switch', out)
            expect(not list(s.glob('user.json.old-*')), 'an old-style backup was made')
        case('someone else signed in: saved beside them, not signed in; told how to switch on the Wii', someone_else)

        def again_merged():
            wii = history('WIII#001', 'WIII#002')
            pc = [(f'PCPC#{i:03d}', 1000 - i) for i in range(10)] + [('WIII#002', 5000)]
            appdata, card = setup(ALICE, direct=pc, card_files={'slippwii/user.json': ALICE,
                                                                'slippwii/direct-codes.bin': wii})
            out = run(appdata, card)
            expected = history('WIII#001', 'WIII#002', *[f'PCPC#{i:03d}' for i in range(6)])
            expect((card / 'slippwii/direct-codes.bin').read_bytes() == expected,
                   "merged: the Wii's codes first, then the PC's newest, no repeats, 8 at most", out)
        case("the account signed in, run again: the PC's codes added after the Wii's, 8 at most", again_merged)

        def same_account_file_reused():
            appdata, card = setup(ALICE, card_files={'slippwii/accounts/alice-from-wii.json': ALICE})
            run(appdata, card)
            names = sorted(p.name for p in (card / 'slippwii/accounts').iterdir())
            expect(names == ['alice-from-wii.json'], f'accounts/: {names}')
        case('an account saved already: its file is updated, not a second one made', same_account_file_reused)

        def damaged_history():
            appdata, card = setup(ALICE, direct='{not json')
            out = run(appdata, card)
            expect((card / 'slippwii/user.json').read_bytes() == ALICE, 'the account was not copied', out)
            expect(not (card / 'slippwii/direct-codes.bin').exists(), 'a history from nothing', out)
            expect('could not be read' in out, 'not said', out)
        case("a damaged direct-codes.json on the PC: the account still copied, its codes left out", damaged_history)

    print('PASS' if not failures else f'{failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

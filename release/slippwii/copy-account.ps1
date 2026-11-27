# SlippWii: copies your Slippi account (user.json, from Slippi Launcher on
# this PC) to the SD card, for SlippWii to log in with. Started by
# "Copy my Slippi account to SD.bat". The account is saved in slippwii\accounts
# (one file each: several people can keep theirs on one card), with the
# connect codes Slippi Launcher remembers for Direct and Teams, and signed in
# when nobody is. Only those files are copied, and only to the card; nothing
# is sent anywhere.
param([string]$Here = '')
$ErrorActionPreference = 'Stop'

function Stop-Copy($Message) {
    Write-Host ''
    Write-Host $Message -ForegroundColor Yellow
    exit 1
}

function Read-Account($Path) {
    try {
        $a = Get-Content -Raw -Encoding UTF8 $Path | ConvertFrom-Json
    } catch { return $null }
    if (-not $a.uid -or -not $a.playKey -or -not $a.connectCode) { return $null }
    return $a
}

function Describe($a) {
    $name = if ($a.displayName) { $a.displayName } else { '(no name)' }
    return "$name ($($a.connectCode))"
}

# A connect code as Melee and the Wii keep it: Shift-JIS, 2 bytes a character
# (docs/EXI_DEVICE_SPEC.md, 0xB4). Slippi Launcher keeps it full-width.
function Code-Bytes([string]$Code) {
    $out = New-Object System.Collections.Generic.List[byte]
    foreach ($ch in $Code.ToCharArray()) {
        $c = [int]$ch
        if ($c -ge 0xFF01 -and $c -le 0xFF5E) { $c -= 0xFEE0 }
        if ($c -ge 0x30 -and $c -le 0x39) { $sj = 0x824F + $c - 0x30 }
        elseif ($c -ge 0x41 -and $c -le 0x5A) { $sj = 0x8260 + $c - 0x41 }
        elseif ($c -ge 0x61 -and $c -le 0x7A) { $sj = 0x8281 + $c - 0x61 }
        elseif ($c -eq 0x23) { $sj = 0x8194 }
        else { return $null }
        $out.Add([byte]($sj -shr 8))
        $out.Add([byte]($sj -band 0xFF))
    }
    if ($out.Count -eq 0 -or $out.Count -gt 16) { return $null }
    return ,$out.ToArray()
}

# The Wii's code history (direct-codes.bin, as the kernel writes it: so_exi.c):
# "SLDC", a count, then per code its length in bytes and 16 bytes, most
# recent first. Returns the codes, as hex strings.
function Read-History($Path) {
    $codes = @()
    if (-not (Test-Path -LiteralPath $Path)) { return ,$codes }
    $b = [IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 5 -or [Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'SLDC') { return ,$codes }
    for ($i = 0; $i -lt $b[4] -and $i -lt 8 -and 5 + ($i + 1) * 17 -le $b.Length; $i++) {
        $len = $b[5 + $i * 17]
        if ($len -eq 0 -or $len -gt 16 -or $len % 2) { break }
        $codes += [BitConverter]::ToString($b, 6 + $i * 17, $len)
    }
    return ,$codes
}

function Write-History($Path, $Codes) {
    $out = New-Object System.Collections.Generic.List[byte]
    $out.AddRange([Text.Encoding]::ASCII.GetBytes('SLDC'))
    $out.Add([byte]$Codes.Count)
    foreach ($hex in $Codes) {
        $bytes = [byte[]]($hex -split '-' | ForEach-Object { [Convert]::ToByte($_, 16) })
        $out.Add([byte]$bytes.Length)
        $out.AddRange($bytes)
        for ($i = $bytes.Length; $i -lt 16; $i++) { $out.Add([byte]0) }
    }
    [IO.File]::WriteAllBytes($Path, $out.ToArray())
}

# Slippi Launcher's history (direct-codes.json: connectCode, lastPlayed), added
# after the Wii's own codes in the file at $Path (the Wii's are newer), 8 at most.
# Returns how many came from the PC.
function Merge-History($Json, $Path) {
    if (-not (Test-Path -LiteralPath $Json)) { return 0 }
    try {
        $pc = @(Get-Content -Raw -Encoding UTF8 -LiteralPath $Json | ConvertFrom-Json)
    } catch {
        Write-Host "($(Split-Path -Leaf $Json) on this PC could not be read: its codes are left out.)"
        return 0
    }
    $codes = Read-History $Path
    $added = 0
    foreach ($e in ($pc | Where-Object { $_.connectCode } | Sort-Object { [double]$_.lastPlayed } -Descending)) {
        $bytes = Code-Bytes $e.connectCode
        if (-not $bytes -or $codes.Count -ge 8) { continue }
        $hex = [BitConverter]::ToString($bytes)
        if ($codes -notcontains $hex) {
            $codes += $hex
            $added++
        }
    }
    if ($codes.Count -gt 0) { Write-History $Path $codes }
    return $added
}

Write-Host 'SlippWii: copy your Slippi account to the SD card'
Write-Host '(SlippWii is unofficial: not made by Project Slippi. https://github.com/2gifts/SlippWii)'
Write-Host ''

# 1. The account: the one Slippi Launcher on this PC is logged in to.
$candidates = @()
foreach ($sub in @('netplay', 'netplay-beta')) {
    $p = Join-Path $env:APPDATA "Slippi Launcher\$sub\User\Slippi\user.json"
    if (Test-Path $p) { $candidates += $p }
}
$accounts = @()
foreach ($p in $candidates) {
    $a = Read-Account $p
    if ($a -and -not ($accounts | Where-Object { $_.Account.uid -eq $a.uid })) {
        $accounts += [pscustomobject]@{ Path = $p; Account = $a }
    }
}
if ($accounts.Count -eq 0) {
    Stop-Copy ("No Slippi account was found on this PC.`n" +
        "  1. Install Slippi Launcher (https://slippi.gg) and log in (or create an account).`n" +
        "  2. Click Play once, so it sets up Slippi Dolphin.`n" +
        "  3. Run this again.")
}
$pick = $accounts[0]
if ($accounts.Count -gt 1) {
    Write-Host 'Slippi accounts on this PC:'
    for ($i = 0; $i -lt $accounts.Count; $i++) { Write-Host "  $($i + 1). $(Describe $accounts[$i].Account)" }
    $n = Read-Host 'Type the number of the account for the Wii and press Enter'
    if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $accounts.Count) { $pick = $accounts[[int]$n - 1] }
}

# 2. The SD card: where this script is (the zip extracted to the card), or a
#    drive that has SlippWii on it.
$cards = @()
if ($Here -and (Test-Path (Join-Path $Here 'slippwii\netplay.gct'))) {
    $cards += (Resolve-Path $Here).Path
} else {
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.IsReady -and (Test-Path (Join-Path $d.RootDirectory.FullName 'slippwii\netplay.gct'))) {
            $cards += $d.RootDirectory.FullName
        }
    }
}
if ($cards.Count -eq 0) {
    Stop-Copy ("SlippWii was not found on any SD card.`n" +
        "Put the Wii's SD card in this PC, extract the SlippWii zip onto it (so it has`n" +
        "the apps and slippwii folders), then run this again from the SD card.")
}
$card = $cards[0]
if ($cards.Count -gt 1) {
    Write-Host 'Drives with SlippWii:'
    for ($i = 0; $i -lt $cards.Count; $i++) { Write-Host "  $($i + 1). $($cards[$i])" }
    $n = Read-Host 'Type the number of the Wii''s SD card and press Enter'
    if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $cards.Count) { $card = $cards[[int]$n - 1] }
}

# 3. Save it in slippwii\accounts (over its earlier copy there, if any: the
#    login may be newer), with its codes; sign in with it when nobody is.
$slippwii = Join-Path $card 'slippwii'
$saved = Join-Path $slippwii 'accounts'
New-Item -ItemType Directory -Force -Path $saved | Out-Null
$file = $null
foreach ($f in Get-ChildItem -LiteralPath $saved -Filter '*.json' -File) {
    $a = Read-Account $f.FullName
    if ($a -and $a.uid -eq $pick.Account.uid) { $file = $f.Name; break }
}
if (-not $file) {
    $stem = ($pick.Account.connectCode -replace '[^A-Za-z0-9]', '-')
    $file = "$stem.json"
    for ($n = 2; Test-Path -LiteralPath (Join-Path $saved $file); $n++) { $file = "$stem-$n.json" }
}
Copy-Item -LiteralPath $pick.Path -Destination (Join-Path $saved $file) -Force
$stem = [IO.Path]::GetFileNameWithoutExtension($file)

$user = Join-Path $slippwii 'user.json'
$current = if (Test-Path -LiteralPath $user) { Read-Account $user } else { $null }
$signedIn = $current -and $current.uid -eq $pick.Account.uid
$signIn = -not $current
$launcher = Split-Path -Parent $pick.Path
$codes = 0
foreach ($kind in @('direct', 'teams')) {
    # (the account signed in plays with the codes in slippwii\; the others' wait in accounts\)
    $target = if ($signedIn) { Join-Path $slippwii "$kind-codes.bin" } else { Join-Path $saved "$stem.$kind.bin" }
    $codes += Merge-History (Join-Path $launcher "$kind-codes.json") $target
}
if ($signedIn -or $signIn) {
    Copy-Item -LiteralPath $pick.Path -Destination $user -Force
}
if ($signIn) {
    foreach ($kind in @('direct', 'teams')) {
        $from = Join-Path $saved "$stem.$kind.bin"
        $to = Join-Path $slippwii "$kind-codes.bin"
        if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination $to -Force }
        elseif (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to }
    }
}
try { Write-VolumeCache -DriveLetter $card[0] -ErrorAction SilentlyContinue } catch {}
Write-Host ''
Write-Host "Done: your Slippi account $(Describe $pick.Account) is saved on the SD card ($card)." -ForegroundColor Green
if ($codes -gt 0) { Write-Host "$codes connect code(s) you played in Direct or Teams came with it." }
if (-not $signedIn -and -not $signIn) {
    Write-Host ''
    Write-Host "$(Describe $current) is signed in on this card. To play as $(Describe $pick.Account):"
    Write-Host 'on the Wii, Settings (B on the game list) > Slippi Settings > Slippi Account.'
}
Write-Host ''
Write-Host 'Next: safely eject the SD card, put it in your Wii, and start SlippWii'
Write-Host 'from the Homebrew Channel. (Keep the card private: the accounts on it are'
Write-Host 'Slippi logins.)'
exit 0

# pull_logs.ps1 -- copies Ocarina-Of-Time-X's log files (a tester build's: setup.py --tester) off the Xbox over its dashboard's FTP server, into a new folder named
# after the time and the build, then prints the lines that matter (build, crashes, hangs, memory lows, warp test end).
#
#   powershell -ExecutionPolicy Bypass -File tools\pull_logs.ps1 -Xbox 192.168.1.50
#
# Set $DefaultXbox below to the console's IP once and -Xbox can be left off. The files are in T:, which for
# Ocarina-Of-Time-X (title ID 54520020) is E:\TDATA\54520020.
param(
    [string]$Xbox = "",
    [string]$User = "xbox",
    [string]$Password = "xbox",
    [string]$Out = (Join-Path $env:USERPROFILE "Desktop\Ocarina-Of-Time-X logs")
)

$DefaultXbox = "192.168.1.50" # the console's IP: edit to yours
if ($Xbox -eq "") { $Xbox = $DefaultXbox }

$titleDir = "TDATA/54520020"
$files = "lusx.log", "lusx_part.log", "lusx_prev.log", "lusx_box.txt"
$dir = Join-Path $Out (Get-Date -Format "yyyy-MM-dd_HH-mm-ss")
New-Item -ItemType Directory -Force -Path $dir | Out-Null

# dashboards name the hard disk's E: partition /E/ (XBMC, UnleashX, Avalaunch) or /e/
$got = 0
foreach ($f in $files) {
    $dest = Join-Path $dir $f
    $ok = $false
    foreach ($drive in "E", "e") {
        & curl.exe -s --fail --connect-timeout 5 -u "${User}:${Password}" "ftp://$Xbox/$drive/$titleDir/$f" -o $dest
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    }
    if ($ok) {
        $got++
        "{0,-14} {1,9:N0} bytes" -f $f, (Get-Item $dest).Length
    } else {
        Remove-Item -ErrorAction SilentlyContinue $dest
        "{0,-14} not there (or the Xbox at $Xbox didn't answer)" -f $f
    }
}
if ($got -eq 0) {
    Remove-Item -ErrorAction SilentlyContinue $dir
    "Nothing copied: check the IP, that the dashboard's FTP server is on, and its login ($User)."
    exit 1
}

# the folder takes the build's stamp, so logs say which ISO they came from
$log = Join-Path $dir "lusx.log"
$built = if (Test-Path $log) { Select-String -Path $log -Pattern "lus-x: built (.+)$" | Select-Object -First 1 } else { $null }
if ($built) {
    $stamp = $built.Matches[0].Groups[1].Value.Trim() -replace "[: ]+", "-"
    $named = "$dir built $stamp"
    Rename-Item -Path $dir -NewName (Split-Path $named -Leaf)
    $dir = $named
}
""
"Saved in: $dir"

# what matters, from the older part of a long run first, then this run
$all = @("lusx_part.log", "lusx.log") | ForEach-Object { Join-Path $dir $_ } | Where-Object { Test-Path $_ }
if ($all) {
    ""
    Select-String -Path $all -Pattern "lus-x: (built|at main|exception|  eax|  stack:|uncaught|std::terminate|watchdog|Link's animation asked|warp test: done)" |
        ForEach-Object { $_.Line -replace "^.*?lus-x: ", "" }
    $lows = Select-String -Path $all -Pattern "the lowest yet"
    if ($lows) { "lowest free: " + ($lows[-1].Line -replace "^.*?down to ", "") }
}
$box = Join-Path $dir "lusx_box.txt"
if (Test-Path $box) {
    ""
    (Get-Content $box -Raw) -replace "`0", "" | Write-Output
}

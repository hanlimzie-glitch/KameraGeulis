# Symbolizes an RVA in gpupixel.dll against the linker .map file produced by
# the CI build (target_compile_options /Zi + target_link_options /DEBUG /MAP).
#
# Parses ONLY the "Publics by Value" section of the map (up to the first blank
# line after entries begin). Two address shapes are accepted:
#   A) seg:offset  sym  abs16  [rva]     e.g. 0001:00001000 ?sym@@... 0000000140001000 1000
#   B) abs16       sym  abs16  [rva]     e.g. 0000000140001000 ?sym@@... 0000000140001000 1000
# The explicit "Rva" column (small hex, when present) is preferred; otherwise
# RVA = abs16 - "Preferred load address". The containing function is the last
# public with RVA <= target.
# Exit: 0 = resolved (prints symbol + ::notice::) - 2 = unresolved (::error::)
param(
  [Parameter(Mandatory = $true)][string]$MapPath,
  [Parameter(Mandatory = $true)][string]$DllPath,
  [Parameter(Mandatory = $true)][uint32]$Rva
)
$ErrorActionPreference = 'Stop'

foreach ($f in @($MapPath, $DllPath)) {
  if (-not (Test-Path $f)) { Write-Host "::error::file not found: $f"; exit 2 }
}
$lines = Get-Content $MapPath
Write-Host "== $MapPath ($($lines.Count) lines) =="

$preferred = $null
foreach ($l in $lines) {
  if ($l -match 'Preferred load address is ([0-9A-Fa-f]+)') {
    $preferred = [Convert]::ToUInt64($Matches[1], 16); break
  }
}
if ($preferred) { Write-Host ("Preferred load address: 0x{0:X}" -f $preferred) }
Write-Host ("Target RVA: 0x{0:X}" -f $Rva)

# --- slice the "Publics by Value" section ---
$hdr = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($lines[$i] -match 'Publics by Value') { $hdr = $i; break }
}
if ($hdr -lt 0) { Write-Host "::error::'Publics by Value' section not found in $MapPath"; exit 2 }

$entries = New-Object System.Collections.Generic.List[object]
for ($i = $hdr + 1; $i -lt $lines.Count; $i++) {
  $l = $lines[$i]
  if ($l -match '^\s*$') { if ($entries.Count -gt 0) { break } else { continue } }
  if ($l -match 'Static symbols|Section Allocation|^Start\s+Length') { break }
  $t = $l.Trim() -split '\s+'
  if ($t.Count -lt 3) { continue }
  # t[0] = seg:offset or abs16 - must look like an address
  if ($t[0] -notmatch '^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8,16}$' -and
      $t[0] -notmatch '^[0-9A-Fa-f]{16}$') { continue }
  # t[1] = symbol (decorated C++ names contain ?/@ so never pure hex)
  $sym = $t[1]
  if ($sym -notmatch '[?@A-Za-z_]') { continue }
  # t[2] = Rva+Base (abs16) in both formats; fall back to t[0] when it is abs16
  $abs = $null
  if ($t[2] -match '^[0-9A-Fa-f]{16}$') { $abs = [Convert]::ToUInt64($t[2], 16) }
  elseif ($t[0] -match '^[0-9A-Fa-f]{16}$') { $abs = [Convert]::ToUInt64($t[0], 16) }
  # t[3] = explicit Rva column (optional, small hex).
  # NOTE: never name this $rva - PowerShell variables are case-insensitive
  # and $rva would clobber the $Rva parameter (that bug once symbolized
  # against the last entry instead of the requested RVA).
  $entryRva = $null
  if ($t.Count -ge 4 -and $t[3] -match '^[0-9A-Fa-f]{1,8}$') {
    $entryRva = [Convert]::ToUInt64($t[3], 16)
  } elseif ($abs -ne $null -and $preferred -ne $null) {
    if ($abs -ge $preferred) { $entryRva = $abs - $preferred }
  }
  if ($entryRva -eq $null) { continue }
  $entries.Add([pscustomobject]@{ Sym = $sym; Rva = $entryRva })
}
Write-Host ("publics entries parsed: {0}" -f $entries.Count)
if ($entries.Count -eq 0) {
  Write-Host "::error::no publics parsed from $MapPath - cannot symbolize RVA"
  exit 2
}

$target = [uint64]$Rva
$best = $null
foreach ($e in $entries) {
  if ($e.Rva -le $target -and ($null -eq $best -or $e.Rva -gt $best.Rva)) { $best = $e }
}
if (-not $best) {
  Write-Host ("::error::no publics entry at or below RVA 0x{0:X} - cannot resolve" -f $Rva)
  exit 2
}

Write-Host ("CONTAINING FUNCTION (last public at or below RVA): {0}  (starts at RVA 0x{1:X})" -f $best.Sym, $best.Rva)
Write-Host ("::notice::gpupixel.dll RVA 0x{0:X} -> {1}" -f $Rva, $best.Sym)
# window around the RVA for manual cross-check
$entries | Sort-Object Rva | Where-Object { $_.Rva -ge ($target - 0x200) -and $_.Rva -le ($target + 0x200) } |
  ForEach-Object { Write-Host ("  RVA 0x{0:X}  {1}" -f $_.Rva, $_.Sym) }
exit 0

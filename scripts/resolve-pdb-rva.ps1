# Symbolizes an RVA in gpupixel.dll against the linker .map file produced by
# the CI build (target_compile_options /Zi + target_link_options /DEBUG /MAP).
#
# Parses BOTH "Publics by Value" and "Static symbols" sections (a non-public
# function such as a COMDAT member can live in the latter). Two address shapes
# are accepted per entry line:
#   A) seg:offset  sym  abs16  [rva]   e.g. 0001:00001000 ?sym@@... 0000000140001000 1000
#   B) abs16       sym  abs16  [rva]   e.g. 0000000140001000 ?sym@@... 0000000140001000 1000
# The explicit "Rva" column (small hex, when present) is preferred; otherwise
# RVA = abs16 - "Preferred load address". The containing function is the last
# symbol with RVA <= target.
#
# CAVEAT: Release links with /OPT:ICF - identical functions fold, so the
# reported name can be an ICF alias of the real one.
#
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

# --- walk the whole map, parsing Publics + Static sections ---
# NOTE: never name a local $rva - PowerShell variables are case-insensitive
# and $rva would clobber the $Rva parameter (that bug once symbolized
# against the last entry instead of the requested RVA).
$entries = New-Object System.Collections.Generic.List[object]
$mode = 'none'
foreach ($l in $lines) {
  if ($l -match 'Publics by Value') { $mode = 'publics'; continue }
  if ($l -match 'Static symbols')    { $mode = 'static';  continue }
  if ($l -match 'Finished symbol|^Libraries|^Section Allocation|^Start\s+Length') { $mode = 'none'; continue }
  if ($mode -eq 'none') { continue }

  $t = $l.Trim() -split '\s+'
  if ($t.Count -lt 3) { continue }
  # t[0] = seg:offset or abs16 - must look like an address
  if ($t[0] -notmatch '^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8,16}$' -and
      $t[0] -notmatch '^[0-9A-Fa-f]{16}$') { continue }
  # t[1] = symbol (decorated C++ names contain ?/@ so never pure hex)
  $sym = $t[1]
  if ($sym -notmatch '[?@A-Za-z_]') { continue }
  # t[2] = Rva+Base (abs16); fall back to t[0] when it is abs16.
  # REAL map line (verified via annotation dump):
  #   0001:00059fc0  ?DoRender@BrightnessFilter@... 0000000180057530 f  brightness_filter.obj
  # Tokens after abs16 are FLAGS ('f', 'i') + .obj name - NOT an RVA column.
  # ('f' matches a naive /^[0-9A-Fa-f]{1,8}$/ test and would parse as 0xF!)
  # RVA = abs16 - preferred load address.
  $abs = $null
  if ($t[2] -match '^[0-9A-Fa-f]{16}$') { $abs = [Convert]::ToUInt64($t[2], 16) }
  elseif ($t[0] -match '^[0-9A-Fa-f]{16}$') { $abs = [Convert]::ToUInt64($t[0], 16) }
  if ($abs -eq $null -or $preferred -eq $null) { continue }
  if ($abs -lt $preferred) { continue }
  $entryRva = $abs - $preferred
  $raw = $l.Trim()
  if ($raw.Length -gt 700) { $raw = $raw.Substring(0, 700) + ' ...[truncated]' }
  $entries.Add([pscustomobject]@{ Sym = $sym; Rva = $entryRva; Section = $mode; Raw = $raw })
}
$pub = @($entries | Where-Object Section -eq 'publics').Count
$sta = @($entries | Where-Object Section -eq 'static').Count
Write-Host ("symbols parsed: {0} (publics {1}, static {2})" -f $entries.Count, $pub, $sta)
if ($entries.Count -eq 0) {
  Write-Host "::error::no symbols parsed from $MapPath - cannot symbolize RVA"
  exit 2
}

$target = [uint64]$Rva
# Prefer real function symbols: skip compiler/ABI metadata ($LN labels,
# $unwind$, $ip2state$, $cppxdata$, $chain$) - those are not function starts,
# so "last symbol <= target" over them would name a label inside a function.
$funcs = @($entries | Where-Object { $_.Sym -notmatch '^\$' } | Sort-Object Rva)
$best = $null
$above = $null
foreach ($e in $funcs) {
  if ($e.Rva -le $target) { $best = $e }
  elseif ($above -eq $null) { $above = $e }
}
if (-not $best) {
  Write-Host ("::error::no function symbol at or below RVA 0x{0:X} - cannot resolve" -f $Rva)
  exit 2
}

# Extra evidence: where do the known crash-candidate functions start?
$probe = @($entries | Where-Object { $_.Sym -notmatch '^\$' -and $_.Sym -match 'DoRender|BeautyFaceUnitFilter' } |
           Sort-Object Rva | Select-Object -First 8)
foreach ($p in $probe) {
  Write-Host ("probe: {0}  RVA 0x{1:X}  [{2}]" -f $p.Sym, $p.Rva, $p.Section)
}

Write-Host ("CONTAINING FUNCTION (last function at/below RVA): {0}  (starts RVA 0x{1:X} [{2}])" -f $best.Sym, $best.Rva, $best.Section)
if ($above) { Write-Host ("next function above: {0}  (RVA 0x{1:X})" -f $above.Sym, $above.Rva) }
$probeNote = ''
if ($probe.Count -gt 0) {
  $probeNote = '; probes: ' + (($probe | ForEach-Object { '{0} @0x{1:X}' -f $_.Sym, $_.Rva }) -join ', ')
}
# ---- annotations (GitHub caps ~10 notices per step - keep it tight) ----
# 1) containing function (best non-metadata symbol at/below the RVA)
Write-Host ("::notice::gpupixel.dll RVA 0x{0:X} -> {1} [{2}, starts 0x{3:X}]" -f $Rva, $best.Sym, $best.Section, $best.Rva)
if ($preferred) { Write-Host ("::notice::map preferred=0x{0:X}" -f $preferred) }
Write-Host ("::notice::map raw best: {0}" -f $best.Raw)
# 2) NEIGHBORHOOD: every parsed symbol in [target-0x200, target+0x200] plus the
#    next symbol above - decisive for "is there a missing/folded fn in between?"
$win = @($entries | Sort-Object Rva | Where-Object { $_.Rva -ge ($target - 0x200) -and $_.Rva -le ($target + 0x200) })
$winNote = ($win | ForEach-Object { '{0} @0x{1:X} [{2}]' -f $_.Sym, $_.Rva, $_.Section }) -join ' || '
Write-Host ("::notice::window around 0x{0:X} ({1} syms): {2}" -f $Rva, $win.Count, $winNote)
if ($above) { Write-Host ("::notice::next symbol above: {0} @0x{1:X} [{2}]" -f $above.Sym, $above.Rva, $above.Section) }
# 3) key fns of BeautyFaceUnitFilter + direct unparsed line search
$key = @($entries | Where-Object { $_.Sym -match '^\?(DoRender|Init|Create)@BeautyFaceUnitFilter' } | Sort-Object Rva)
$keyNote = ($key | ForEach-Object { '{0} @0x{1:X}' -f $_.Sym, $_.Rva }) -join ' || '
Write-Host ("::notice::key fns BeautyFaceUnitFilter ({0}): {1}" -f $key.Count, $keyNote)
$direct = @($lines | Where-Object { $_ -match 'DoRender@BeautyFaceUnitFilter' } | Select-Object -First 2)
Write-Host ("::notice::map lines containing DoRender@BeautyFaceUnitFilter: {0}" -f $direct.Count)
foreach ($d in $direct) {
  $dd = $d.Trim()
  if ($dd.Length -gt 600) { $dd = $dd.Substring(0, 600) + ' ...[truncated]' }
  Write-Host ("::notice::map direct: {0}" -f $dd)
}
Write-Host ("symbols parsed: {0}; best=0x{1:X}; above={2}" -f $entries.Count, $best.Rva, $(if ($above) { '0x{0:X}' -f $above.Rva } else { 'none' }))
exit 0

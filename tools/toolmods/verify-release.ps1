param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string]$ReportPath
)
$ErrorActionPreference = 'Stop'
$cacheText = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
if ($cacheText -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') {
    throw 'Expected a Release configuration.'
}
$entries = Get-Content -LiteralPath (Join-Path $BuildDirectory 'compile_commands.json') -Raw | ConvertFrom-Json
$coreEntries = @($entries | Where-Object {
    $_.file.Replace('\', '/') -match '/src/(core|video_core|dynarmic|shader_recompiler|audio_core|hid_core|yuzu)/' -and
    $_.file -match '\.(c|cc|cpp|cxx)$'
})
if ($coreEntries.Count -lt 700) {
    throw "Incomplete compiler command database: $($coreEntries.Count) relevant entries."
}
$optimizationPattern = '(?<!\S)(/O[12dx]|-O[0123sgz]|/clang:-O[0123sgz])(?=\s|$)'
$failures = @(foreach ($entry in $coreEntries) {
    $command = if ($entry.command) { $entry.command } else { $entry.arguments -join ' ' }
    $optimization = [regex]::Matches($command, $optimizationPattern)
    $effective = if ($optimization.Count) { $optimization[$optimization.Count - 1].Value } else { '' }
    if ($effective -notin @('/O2', '/Ox', '-O2', '-O3', '/clang:-O2', '/clang:-O3') -or
        $command -notmatch '(?<!\S)[-/]DNDEBUG(?=\s|$)') {
        [pscustomobject]@{ file = $entry.file; effective_optimization = $effective }
    }
})
$report = [ordered]@{
    checked_at = (Get-Date).ToString('o')
    build_directory = $BuildDirectory
    configuration = 'Release'
    checked_entries = $coreEntries.Count
    failed_entries = $failures.Count
    failures = $failures
    cache_sha256 = (Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Algorithm SHA256).Hash
}
if ($ReportPath) {
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}
if ($failures.Count) {
    $failures | Select-Object -First 5 | Format-Table | Out-String | Write-Output
    throw "Release optimization check failed for $($failures.Count)/$($coreEntries.Count) compilation entries."
}
Write-Output "Release optimization check passed: $($coreEntries.Count) compiler commands have optimization and NDEBUG."

param(
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory
)
$ErrorActionPreference = 'Stop'
$metadata = Get-Content -Raw -Encoding UTF8 -LiteralPath 'dist/toolmods-release.json' | ConvertFrom-Json
$root = 'dist/tool_mods/04007EF01F8AE000'
$itemsPath = Join-Path $root 'item-add/items.json'
$items = Get-Content -Raw -Encoding UTF8 -LiteralPath $itemsPath | ConvertFrom-Json
if ($items.Count -ne $metadata.item_catalog_count -or
    (Get-FileHash -LiteralPath $itemsPath -Algorithm SHA256).Hash -ne $metadata.item_catalog_sha256) {
    throw 'Item catalog count/hash does not match the release metadata.'
}
if (@($items.iid | Sort-Object -Unique).Count -ne $items.Count -or
    @($items | Where-Object {
        -not $_.iid.StartsWith('IID_') -or
        [string]::IsNullOrWhiteSpace($_.name_jp) -or
        [string]::IsNullOrWhiteSpace($_.name_zh_tw) -or
        [string]::IsNullOrWhiteSpace($_.name_zh_cn)
    }).Count -ne 0) { throw 'Invalid or duplicate catalog entries.' }
if (-not ($items.miid_labels -contains 'MIID_JuniorLicense')) { throw 'Missing examination certificate.' }
$friendship = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'friendship-adjust/manifest.json') | ConvertFrom-Json
if ($friendship.ui.type -ne 'numeric_action') { throw 'Missing numeric-action UI.' }
$characters = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root 'friendship-adjust/characters.json') | ConvertFrom-Json
if ($characters.Count -lt 1) { throw 'Empty character catalog.' }
$runtimeExe = (Resolve-Path -LiteralPath (Join-Path $RuntimeDirectory 'python.exe')).Path
$runtimeVersion = & $runtimeExe -c 'import platform; print(platform.python_version())'
if ($LASTEXITCODE -ne 0 -or $runtimeVersion.Trim() -ne $metadata.python_version) { throw 'Wrong Python runtime.' }
$savedPath = $env:Path
try {
    $env:Path = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($id in @('item-add', 'friendship-adjust')) {
        $package = (Resolve-Path -LiteralPath (Join-Path $root $id)).Path
        $manifest = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $package 'manifest.json') | ConvertFrom-Json
        if ($manifest.id -ne $id -or $manifest.api_version -ne 1 -or
            $manifest.runtime -ne '../../../python-runtime/python.exe') { throw "Invalid manifest: $id" }
        $resolved = [IO.Path]::GetFullPath((Join-Path $package $manifest.runtime))
        if ($resolved -ne [IO.Path]::GetFullPath((Join-Path 'dist/python-runtime' 'python.exe'))) {
            throw "Invalid package-relative runtime path: $id"
        }
        foreach ($name in @($manifest.entrypoint, 'README.md')) {
            if (-not (Test-Path -LiteralPath (Join-Path $package $name))) { throw "Missing package file: $id/$name" }
        }
        foreach ($script in Get-ChildItem -LiteralPath $package -Filter '*.py' -File) {
            & $runtimeExe -B -X utf8 -c 'import pathlib,sys; p=pathlib.Path(sys.argv[1]); compile(p.read_text(encoding=''utf-8-sig''),str(p),''exec'')' $script.FullName
            if ($LASTEXITCODE -ne 0) { throw "Invalid Python syntax: $($script.Name)" }
        }
        # --help parses/imports the actual packaged entrypoint without touching
        # a guest debugger. The runtime is addressed explicitly with PATH stripped.
        & $runtimeExe -B -X utf8 (Join-Path $package $manifest.entrypoint) --help
        if ($LASTEXITCODE -ne 0) { throw "Entrypoint smoke check failed: $id" }
        $transient = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object {
            $_.Extension -in @('.pyc', '.log') -or $_.Name -in @('cancel.request', 'inventory_context.json')
        })
        if ($transient.Count -gt 0) { throw "Transient runtime files in distribution: $id" }
    }
}
finally { $env:Path = $savedPath }
Write-Output "Package checks passed: $($items.Count) items; bundled Python $runtimeVersion; both entrypoints work without system Python on PATH."

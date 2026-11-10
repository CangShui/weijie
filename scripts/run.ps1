param(
    [ValidateSet('Capture', 'Settings', 'Background', 'Quit')]
    [string]$Mode = 'Capture'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$resource = Get-Content -LiteralPath (Join-Path $root 'cpp\src\app.rc') -Raw -Encoding UTF8
$name = [regex]::Match($resource, 'VALUE\s+"OriginalFilename",\s*"([^"]+)"').Groups[1].Value
$exe = Join-Path $root "Release\weijie\$name"
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: $root\build.bat" }
switch ($Mode) {
    'Quit' { & $exe --quit }
    'Capture' { Start-Process -FilePath $exe -ArgumentList '--capture' }
    'Settings' { Start-Process -FilePath $exe -ArgumentList '--settings' }
    'Background' { Start-Process -FilePath $exe -WindowStyle Hidden }
}

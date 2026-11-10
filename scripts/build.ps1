# Keep this file ASCII-only for Windows PowerShell 5.1 compatibility.
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Package', 'Test', 'All', 'Clean')]
    [string]$Task = 'Build',
    [ValidateSet('Core', 'CPU', 'GPU', 'UI', 'All')]
    [string]$Suite = 'Core',
    [string]$RuntimeSource = $env:JJ_RUNTIME_SOURCE,
    [string]$DocsSource = $env:JJ_DOCS_SOURCE,
    [switch]$SkipRuntime,
    [switch]$Zip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Task -eq 'All' -and -not $PSBoundParameters.ContainsKey('Suite')) { $Suite = 'All' }
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $RuntimeSource) {
    $localRuntime = Join-Path $Root 'runtime'
    if (Test-Path -LiteralPath $localRuntime) { $RuntimeSource = $localRuntime }
}
$Release = Join-Path $Root 'Release'
$App = Join-Path $Release 'weijie'
$Intermediate = Join-Path $Release '.build'
$Tests = Join-Path $Release 'Tests'
$Reports = Join-Path $Release 'verification'
$RuntimeNames = @('emoji')
$AppSources = @('main', 'overlay', 'annotate', 'capture', 'longshot', 'ocr',
    'fast_ocr', 'style_popup', 'ocr_setup', 'ppocr_setup', 'translate', 'pin', 'output',
    'settings', 'common', 'emoji_renderer', 'emoji_popup') |
    ForEach-Object { Join-Path $Root "cpp\src\$_.cpp" }
$Resource = Join-Path $Root 'cpp\src\app.rc'
$ResourceText = Get-Content -LiteralPath $Resource -Raw -Encoding UTF8
$NameMatch = [regex]::Match($ResourceText, 'VALUE\s+"OriginalFilename",\s*"([^"]+)"')
if (-not $NameMatch.Success) { throw 'OriginalFilename missing in cpp/src/app.rc' }
$AppExe = Join-Path $App $NameMatch.Groups[1].Value
$CommonLibraries = @('user32.lib', 'gdi32.lib', 'gdiplus.lib', 'shell32.lib',
    'ole32.lib', 'winhttp.lib', 'dwmapi.lib', 'shcore.lib', 'windowsapp.lib',
    'advapi32.lib', 'comdlg32.lib')

function Assert-InRelease([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if (-not $absolute.StartsWith($Release + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing file operation outside Release: $absolute"
    }
    return $absolute
}

function Ensure-Directory([string]$Path) {
    [void](New-Item -ItemType Directory -Path $Path -Force)
}

function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '')
        } finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}

function Invoke-Native([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Initialize-Compiler {
    if ($env:VCToolsInstallDir -and $env:WindowsSdkDir -and
            (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
            (Get-Command rc.exe -ErrorAction SilentlyContinue)) { return }

    $devcmd = $env:JJ_VSDEV_CMD
    $programRoots = @(${env:ProgramFiles(x86)}, $env:ProgramFiles) |
        Where-Object { $_ } | Select-Object -Unique
    if (-not $devcmd) {
        foreach ($programRoot in $programRoots) {
            $vswhere = Join-Path $programRoot 'Microsoft Visual Studio\Installer\vswhere.exe'
            if (Test-Path -LiteralPath $vswhere) {
                $found = @(& $vswhere -latest -products '*' -requires `
                    Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                    -find 'Common7\Tools\VsDevCmd.bat')
                if ($found.Count -gt 0) { $devcmd = $found[0]; break }
            }
        }
    }
    if (-not $devcmd) {
        foreach ($programRoot in $programRoots) {
            $visualStudio = Join-Path $programRoot 'Microsoft Visual Studio'
            if (-not (Test-Path -LiteralPath $visualStudio)) { continue }
            $candidates = @(Get-ChildItem -LiteralPath $visualStudio -Directory |
                ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory } |
                ForEach-Object { Join-Path $_.FullName 'Common7\Tools\VsDevCmd.bat' } |
                Where-Object { Test-Path -LiteralPath $_ } | Sort-Object -Descending)
            if ($candidates.Count -gt 0) { $devcmd = $candidates[0]; break }
        }
    }
    if (-not $devcmd -or -not (Test-Path -LiteralPath $devcmd)) {
        throw 'MSVC x64 / Windows SDK not found. Install C++ Build Tools or set JJ_VSDEV_CMD.'
    }

    # Only import a compiler environment. No filesystem operations are delegated
    # to cmd.exe; copying, moving and cleaning stay in PowerShell.
    $savedModulePath = $env:PSModulePath
    $environment = @(& $env:ComSpec /d /s /c `
        ('call "' + $devcmd + '" -no_logo -arch=x64 -host_arch=x64 >nul && set'))
    if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd.bat failed' }
    foreach ($entry in $environment) {
        $equals = $entry.IndexOf('=')
        if ($equals -gt 0) {
            $name = $entry.Substring(0, $equals)
            # VsDevCmd can replace PSModulePath with Visual Studio paths; that
            # breaks cmdlet autoload (for example Get-FileHash) later.
            if ($name -ine 'PSModulePath') {
                [Environment]::SetEnvironmentVariable($name,
                    $entry.Substring($equals + 1), 'Process')
            }
        }
    }
    if ($savedModulePath) { $env:PSModulePath = $savedModulePath }
    foreach ($tool in @('cl.exe', 'rc.exe')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "Compiler setup did not expose $tool"
        }
    }
    Write-Host "MSVC: $env:VCToolsInstallDir"
    Write-Host "SDK:  $env:WindowsSDKVersion"
}

function Assert-Unlocked([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    try {
        $file = [IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $file.Dispose()
    } catch {
        throw "Output is in use. Exit the running Release application, then retry: $Path"
    }
}

function Compile-Target([string]$Name, [string[]]$Sources, [string]$Output,
                        [bool]$Windows, [bool]$WithResource) {
    $objects = Join-Path $Intermediate $Name
    Ensure-Directory $objects
    Ensure-Directory (Split-Path -Parent $Output)
    Assert-Unlocked $Output
    $inputs = @($Sources)
    if ($WithResource) {
        $res = Join-Path $objects 'app.res'
        Invoke-Native 'rc.exe' @('/nologo', '/fo', $res, $Resource)
        $inputs += $res
    }
    $flags = @('/nologo', '/std:c++20', '/EHa', '/O2', '/MT', '/utf-8', '/W3',
        '/DUNICODE', '/D_UNICODE', ("/Fo$objects\"), ("/Fd$objects\"))
    $subsystem = if ($Windows) { 'WINDOWS' } else { 'CONSOLE' }
    Invoke-Native 'cl.exe' ($flags + $inputs + @('/link',
        "/SUBSYSTEM:$subsystem", "/OUT:$Output", ("/MAP:$objects\$Name.map")) + $CommonLibraries)
}

function Copy-Tree([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Runtime folder missing: $Source"
    }
    Ensure-Directory $Destination
    & robocopy.exe $Source $Destination /E /XJ /R:1 /W:1 /NFL /NDL /NJH /NJS `
        /XD __pycache__ /XF '*.pyc' | Out-Host
    if ($LASTEXITCODE -gt 7) { throw "Runtime copy failed: $Source" }
}

function Ensure-OcrRuntime {
    $dir = Join-Path $Root 'runtime\ocr-ncnn'
    if (Test-Path -LiteralPath (Join-Path $dir 'ocr_ncnn.exe')) { return $dir }
    $zip = Join-Path $Root 'runtime\weijie-ocr-ncnn.zip'
    if (-not (Test-Path -LiteralPath $zip)) {
        throw 'runtime\weijie-ocr-ncnn.zip is missing; cannot stage OCR tests.'
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'ocr_ncnn.exe'))) {
        throw 'Failed to extract runtime\weijie-ocr-ncnn.zip'
    }
    return $dir
}

function Write-Manifest {
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($AppExe).FileVersion
    $entries = @(Get-ChildItem -LiteralPath $App -Recurse -File |
        Where-Object {
            $_.Name -ne 'manifest.json' -and $_.Extension -ne '.pyc' -and
            $_.FullName -notmatch '[\\/](ov-cache|__pycache__)[\\/]'
        } | ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($App.Length + 1)
                size = $_.Length
                sha256 = Get-Sha256 $_.FullName
            }
        })
    [ordered]@{
        version = $version
        built = [DateTimeOffset]::Now.ToString('o')
        architecture = 'x64'
        files = $entries
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $App 'manifest.json') -Encoding UTF8
}

function Package-Application {
    if (-not (Test-Path -LiteralPath $AppExe)) {
        throw 'No executable. Run Build first.'
    }
    # Remove legacy runtime folders that are no longer part of the package.
    foreach ($legacy in @('ocr-fast', 'ocr-ncnn', 'ocr-runtime', 'ocr-runtime-gpu')) {
        if ($legacy -in $RuntimeNames) { continue }
        $stale = Assert-InRelease (Join-Path $App $legacy)
        if (Test-Path -LiteralPath $stale) {
            Remove-Item -LiteralPath $stale -Recurse -Force
        }
    }
    if ($RuntimeSource) {
        $source = (Resolve-Path -LiteralPath $RuntimeSource).Path
        if (-not $source.Equals($App, [StringComparison]::OrdinalIgnoreCase)) {
            foreach ($name in $RuntimeNames) {
                Copy-Tree (Join-Path $source $name) (Join-Path $App $name)
            }
            if (Test-Path -LiteralPath (Join-Path $source 'licenses')) {
                Copy-Tree (Join-Path $source 'licenses') (Join-Path $App 'licenses')
            }
        }
    }
    if (-not $SkipRuntime) {
        $required = @('emoji\00.png')
        foreach ($file in $required) {
            if (-not (Test-Path -LiteralPath (Join-Path $App $file))) {
                throw "Missing $file. Use -RuntimeSource <complete-package> or -SkipRuntime for an EXE-only build."
            }
        }
    }
    if (Test-Path -LiteralPath (Join-Path $App 'ocr-fast')) {
        Copy-Item -LiteralPath (Join-Path $Root 'cpp\ocr-fast\ocr_sidecar.py') `
            -Destination (Join-Path $App 'ocr-fast\ocr_sidecar.py') -Force
    }
    $releaseReadme = Join-Path $Root 'docs\RELEASE.md'
    if (-not (Test-Path -LiteralPath $releaseReadme) -and $DocsSource) {
        $releaseReadme = Join-Path $DocsSource 'README.md'
    }
    if (Test-Path -LiteralPath $releaseReadme) {
        Copy-Item -LiteralPath $releaseReadme -Destination (Join-Path $App 'README.md') -Force
    }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($legacyShortcut in @('Capture.lnk', 'Settings.lnk')) {
        $stale = Join-Path $App $legacyShortcut
        if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }
    }
    $captureName = -join ([char[]](0x4E00, 0x952E, 0x622A, 0x56FE))
    $settingsName = -join ([char[]](0x8BBE, 0x7F6E))
    foreach ($item in @(@($captureName, '--capture'), @($settingsName, '--settings'))) {
        $shortcut = $shell.CreateShortcut((Join-Path $App ($item[0] + '.lnk')))
        $shortcut.TargetPath = $AppExe
        $shortcut.WorkingDirectory = $App
        $shortcut.Arguments = $item[1]
        $shortcut.Save()
    }
    Write-Manifest
}

function Invoke-CoreTest([string]$Argument, [string]$Label) {
    $temporary = Join-Path $App '_core_tests.exe'
    $sources = @('settings', 'common', 'output', 'longshot', 'pin', 'ocr',
        'fast_ocr', 'style_popup', 'ocr_setup') |
        ForEach-Object { Join-Path $Root "cpp\src\$_.cpp" }
    if (-not $script:CoreCompiled) {
        Compile-Target 'core' (@((Join-Path $Root 'cpp\tests\core_tests.cpp')) + $sources) `
            $temporary $false $false
        $script:CoreCompiled = $true
    }
    $log = Join-Path $Reports ("core-$Label.txt")
    if ($Argument) { & $temporary $Argument 2>&1 | Tee-Object -FilePath $log }
    else { & $temporary 2>&1 | Tee-Object -FilePath $log }
    if ($LASTEXITCODE -ne 0) { throw "Core test failed: $Label (see $log)" }
}

function Invoke-UITest {
    $temporary = Join-Path $App '_ui_preview.exe'
    $sources = @($AppSources | Where-Object {
        [IO.Path]::GetFileName($_) -notin @('main.cpp', 'style_popup.cpp')
    })
    Compile-Target 'ui' (@((Join-Path $Root 'cpp\tests\ui_preview.cpp')) + $sources) `
        $temporary $false $false
    & $temporary (Join-Path $Reports 'ui-previews') 2>&1 |
        Tee-Object -FilePath (Join-Path $Reports 'ui-tests.txt')
    if ($LASTEXITCODE -ne 0) { throw 'UI test failed' }
}

function Test-Application {
    $script:CoreCompiled = $false
    Ensure-Directory $Tests
    Ensure-Directory $Reports
    $scratch = Join-Path $Reports 'scratch'
    Ensure-Directory $scratch
    $oldTemp = $env:TEMP
    $oldTmp = $env:TMP
    $ocrStage = Join-Path $App 'ocr-ncnn'
    $ocrSource = Ensure-OcrRuntime
    if (Test-Path -LiteralPath $ocrStage) {
        $safeOcr = Assert-InRelease $ocrStage
        Remove-Item -LiteralPath $safeOcr -Recurse -Force
    }
    Copy-Tree $ocrSource $ocrStage
    try {
        # The existing tests isolate APPDATA underneath GetTempPath().
        $env:TEMP = $scratch
        $env:TMP = $scratch
        if ($Suite -in @('Core', 'All')) { Invoke-CoreTest '' 'basic' }
        if ($Suite -in @('CPU', 'All')) { Invoke-CoreTest '--fast' 'cpu' }
        if ($Suite -in @('GPU', 'All')) { Invoke-CoreTest '--fast-gpu' 'gpu' }
        if ($Suite -in @('UI', 'All')) { Invoke-UITest }
    } finally {
        $env:TEMP = $oldTemp
        $env:TMP = $oldTmp
        foreach ($pair in @(@('_core_tests.exe', 'core_tests.exe'), @('_ui_preview.exe', 'ui_preview.exe'))) {
            $file = Join-Path $App $pair[0]
            if (Test-Path -LiteralPath $file) {
                $safeSource = Assert-InRelease $file
                $safeTarget = Assert-InRelease (Join-Path $Tests $pair[1])
                Move-Item -LiteralPath $safeSource -Destination $safeTarget -Force
            }
        }
        if (Test-Path -LiteralPath $scratch) {
            $safeScratch = Assert-InRelease $scratch
            $pending = New-Object 'System.Collections.Generic.Stack[string]'
            $pending.Push($safeScratch)
            while ($pending.Count -gt 0) {
                foreach ($child in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                    if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                        throw "Refusing to clean test links: $($child.FullName)"
                    }
                    if ($child.PSIsContainer) { $pending.Push($child.FullName) }
                }
            }
            Remove-Item -LiteralPath $safeScratch -Recurse -Force
        }
        if (Test-Path -LiteralPath $ocrStage) {
            $safeOcr = Assert-InRelease $ocrStage
            Remove-Item -LiteralPath $safeOcr -Recurse -Force
        }
    }
    Write-Manifest
}

function Write-Zip {
    Ensure-Directory (Join-Path $Release 'archives')
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($AppExe).FileVersion
    $path = Join-Path $Release "archives\weijie-$version-win-x64.zip"
    Compress-Archive -LiteralPath $App -DestinationPath $path -Force
    Write-Host "Archive: $path"
}

try {
    if ($Task -eq 'Clean') {
        $safe = Assert-InRelease $Intermediate
        if (Test-Path -LiteralPath $safe) {
            $item = Get-Item -LiteralPath $safe
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Refusing to clean a linked intermediate directory'
            }
            $pending = New-Object 'System.Collections.Generic.Stack[string]'
            $pending.Push($safe)
            while ($pending.Count -gt 0) {
                foreach ($child in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
                    if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                        throw "Refusing to clean a tree containing links: $($child.FullName)"
                    }
                    if ($child.PSIsContainer) { $pending.Push($child.FullName) }
                }
            }
            Remove-Item -LiteralPath $safe -Recurse -Force
        }
        Write-Host 'Intermediate files removed. Release packages and user data were preserved.'
        exit 0
    }
    Ensure-Directory $Release
    Ensure-Directory $App
    Push-Location (Join-Path $Root 'cpp')
    try {
        if ($Task -in @('Build', 'Test', 'All')) { Initialize-Compiler }
        if ($Task -in @('Build', 'All')) {
            Compile-Target 'app' $AppSources $AppExe $true $true
        }
        Package-Application
        if ($Task -in @('Test', 'All')) { Test-Application }
        if ($Zip) { Write-Zip }
    } finally { Pop-Location }
    Write-Host "SUCCESS: $Task"
    Write-Host "Application: $AppExe"
    Write-Host "Reports:     $Reports"
    exit 0
} catch {
    [Console]::Error.WriteLine("BUILD FAILED: " + $_.Exception.Message)
    exit 1
}

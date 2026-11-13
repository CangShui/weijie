# Keep this file ASCII-only for Windows PowerShell 5.1 compatibility.
[CmdletBinding()]
param(
    [string]$Target = 'C:\Program Files\weijie',
    [switch]$Build,
    [switch]$NoStart,
    [switch]$Elevated
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'images-layout.ps1')
$Source = Join-Path $Root 'Release\weijie'
$Resource = Join-Path $Root 'cpp\src\app.rc'
$LogDir = Join-Path $Root 'logs'
[void](New-Item -ItemType Directory -Path $LogDir -Force)
$Log = Join-Path $LogDir ('deploy-' + (Get-Date -Format 'yyyyMMdd') + '.log')
$ModulePath = $env:PSModulePath

function Log([string]$Message) {
    $line = '[' + (Get-Date -Format 'HH:mm:ss.fff') + '] ' + $Message
    Write-Host $line
    Add-Content -LiteralPath $Log -Value $line -Encoding UTF8
}

function Is-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return [Security.Principal.WindowsPrincipal]::new($identity).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
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

$ResourceText = Get-Content -LiteralPath $Resource -Raw -Encoding UTF8
$NameMatch = [regex]::Match($ResourceText, 'VALUE\s+"OriginalFilename",\s*"([^"]+)"')
if (-not $NameMatch.Success) { throw 'OriginalFilename missing in cpp/src/app.rc' }
$ExeName = $NameMatch.Groups[1].Value
$ExeBase = [IO.Path]::GetFileNameWithoutExtension($ExeName)
$SourceExe = Join-Path $Source $ExeName
$TargetExe = Join-Path $Target $ExeName

Log ('Deploy start: source=' + $SourceExe + ' target=' + $TargetExe + ' elevated=' + (Is-Elevated))

if ($Build) {
    Log 'Building before deploy'
    & (Join-Path $PSScriptRoot 'build.ps1') -Task Build
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    $env:PSModulePath = $ModulePath
}
if (-not (Test-Path -LiteralPath $SourceExe)) {
    throw "Missing build output: $SourceExe. Run build.ps1 -Task Build first."
}
if (-not (Test-Path -LiteralPath $Target)) {
    throw "Installation folder not found: $Target"
}

$SourceHash = Get-Sha256 $SourceExe
$TargetHash = if (Test-Path -LiteralPath $TargetExe) {
    Get-Sha256 $TargetExe
} else { '' }

$stopped = @()
$running = @(Get-Process -Name $ExeBase -ErrorAction SilentlyContinue)
foreach ($process in $running) {
    $path = $null
    try { $path = $process.Path } catch { $path = $null }
    if ($path -and -not $path.Equals($TargetExe, [StringComparison]::OrdinalIgnoreCase)) {
        Log ('Skip unrelated instance pid=' + $process.Id + ' path=' + $path)
        continue
    }
    try {
        # Stop-Process can report access denied for an elevated instance that
        # still permits PROCESS_TERMINATE; Process.Kill uses that right directly.
        try {
            $native = [Diagnostics.Process]::GetProcessById($process.Id)
            $native.Kill()
            [void]$native.WaitForExit(3000)
        } catch {
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
        }
        $stopped += $process.Id
        Log ('Stopped instance pid=' + $process.Id)
    } catch {
        Log ('Stop failed pid=' + $process.Id + ' error=' + $_.Exception.Message)
        if (-not $Elevated) {
            Log 'Instance runs elevated; relaunching deploy with administrator rights'
            $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
                '-Target', $Target, '-Elevated')
            if ($NoStart) { $arguments += '-NoStart' }
            Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -Wait
            exit 0
        }
        throw
    }
}

if ($stopped.Count -gt 0) {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $handle = [IO.File]::Open($TargetExe, 'Open', 'ReadWrite', 'None')
            $handle.Dispose()
            break
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
}

$firstInstall = -not (Test-Path -LiteralPath $TargetExe)
Copy-Item -LiteralPath $SourceExe -Destination $TargetExe -Force
Initialize-ImagesLayout -SourceImages (Join-Path $Source 'images') -TargetImages (Join-Path $Target 'images') `
    -ManifestPath (Join-Path $Target 'manifest.json') -FirstInstall $firstInstall
Log 'Image groups synchronized; existing contents and user deletions preserved.'
foreach ($name in @('README.md', 'manifest.json')) {
    $from = Join-Path $Source $name
    if (Test-Path -LiteralPath $from) {
        Copy-Item -LiteralPath $from -Destination (Join-Path $Target $name) -Force
    }
}
$installedHash = Get-Sha256 $TargetExe
Log ('Replaced executable: changed=' + ($SourceHash -ne $TargetHash) + ' sha256=' + $installedHash.Substring(0, 16))
if ($installedHash -ne $SourceHash) { throw 'Installed executable does not match build output' }

if (-not $NoStart) {
    $task = Get-ScheduledTask -ErrorAction SilentlyContinue |
        Where-Object { $_.TaskName -like 'weijie-logon-*' -and
            $_.Actions.Execute -eq $TargetExe } | Select-Object -First 1
    if ($task) {
        Log ('Starting via elevated logon task: ' + $task.TaskName)
        Start-ScheduledTask -TaskName $task.TaskName
    } else {
        Log 'No elevated logon task; starting normally (confirm UAC if prompted)'
        Start-Process -FilePath $TargetExe
    }
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        $started = @(Get-Process -Name $ExeBase -ErrorAction SilentlyContinue)
        if ($started.Count -gt 0) {
            Log ('Started pid=' + (($started | ForEach-Object { $_.Id }) -join ','))
            break
        }
    }
}
Log 'Deploy finished'

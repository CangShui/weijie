# Keep this file ASCII-only for Windows PowerShell 5.1 compatibility.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'images-layout.ps1')
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixture = Join-Path $root ('Release\verification\images-layout-' + $PID)
$source = Join-Path $fixture 'source\images'
$target = Join-Path $fixture 'installed\images'
$manifest = Join-Path $fixture 'installed\manifest.json'
$log = Join-Path $root 'logs\images-layout-tests.log'
$checks = 0
function Check([bool]$Result,[string]$Name) {
    $line = ('traceId=images-layout-' + $PID + ' ' + $(if ($Result) { 'PASS ' } else { 'FAIL ' }) + $Name)
    Write-Host $line
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
    if (-not $Result) { throw $Name }
    $script:checks++
}
try {
    [void](New-Item -ItemType Directory -Path (Join-Path $source 'default') -Force)
    [void](New-Item -ItemType Directory -Path $target -Force)
    [void](New-Item -ItemType Directory -Path (Join-Path $target 'default') -Force)
    Set-Content -LiteralPath (Join-Path $source 'default\a.png') -Value 'bundled a'
    Set-Content -LiteralPath (Join-Path $source 'default\b.png') -Value 'bundled b'
    Set-Content -LiteralPath (Join-Path $target 'a.png') -Value 'existing edited a'
    Set-Content -LiteralPath (Join-Path $target 'custom.png') -Value 'loose user picture'
    Set-Content -LiteralPath $manifest -Value '{"files":[]}'
    Initialize-ImagesLayout -SourceImages $source -TargetImages $target -ManifestPath $manifest
    Check ((Test-Path -LiteralPath (Join-Path $target 'default\a.png')) -and
        -not (Test-Path -LiteralPath (Join-Path $target 'a.png'))) 'old default image moves into existing default group'
    Check ((Get-Content -LiteralPath (Join-Path $target 'default\a.png')) -eq 'existing edited a') 'edited default image contents preserved'
    Check (-not (Test-Path -LiteralPath (Join-Path $target 'default\b.png'))) 'previously deleted bundled image is not restored during relocation'
    Check (Test-Path -LiteralPath (Join-Path $target 'custom.png')) 'custom root picture remains Unclassified'
    Set-Content -LiteralPath $manifest -Value '{"imagesLayout":"groups","files":[]}'
    Remove-Item -LiteralPath (Join-Path $target 'default\a.png')
    Initialize-ImagesLayout -SourceImages $source -TargetImages $target -ManifestPath $manifest
    Check (-not (Test-Path -LiteralPath (Join-Path $target 'default\a.png'))) 'grouped install keeps default image deletion'
    Remove-Item -LiteralPath (Join-Path $target 'default')
    Initialize-ImagesLayout -SourceImages $source -TargetImages $target -ManifestPath $manifest
    Check (-not (Test-Path -LiteralPath (Join-Path $target 'default'))) 'grouped install keeps entire default folder deletion'
    Set-Content -LiteralPath (Join-Path $target 'a.png') -Value 'new loose user picture'
    Initialize-ImagesLayout -SourceImages $source -TargetImages $target -ManifestPath $manifest
    Check ((Test-Path -LiteralPath (Join-Path $target 'a.png')) -and
        -not (Test-Path -LiteralPath (Join-Path $target 'default'))) 'new root image with bundled filename is never reclassified after upgrade'
    $fresh = Join-Path $fixture 'fresh\images'
    Initialize-ImagesLayout -SourceImages $source -TargetImages $fresh `
        -ManifestPath (Join-Path $fixture 'fresh\manifest.json') -FirstInstall $true
    Check ((Test-Path -LiteralPath (Join-Path $fresh 'default\a.png')) -and
        (Test-Path -LiteralPath (Join-Path $fresh 'default\b.png'))) 'fresh install receives grouped bundled images'
    Write-Host ('Image layout checks: ' + $checks)
    exit 0
} catch {
    Add-Content -LiteralPath $log -Value ('traceId=images-layout-' + $PID + ' ERROR ' + $_.Exception.Message) -Encoding UTF8
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}

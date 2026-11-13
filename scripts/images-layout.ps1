# Keep this file ASCII-only for Windows PowerShell 5.1 compatibility.
function Initialize-ImagesLayout {
    param(
        [Parameter(Mandatory=$true)][string]$SourceImages,
        [Parameter(Mandatory=$true)][string]$TargetImages,
        [Parameter(Mandatory=$true)][string]$ManifestPath,
        [bool]$FirstInstall = $false
    )
    $source = [IO.Path]::GetFullPath($SourceImages)
    $target = [IO.Path]::GetFullPath($TargetImages)
    if ([IO.Path]::GetFileName($target) -ne 'images') { throw 'Expected an images directory' }
    $logDir = Join-Path $PSScriptRoot '..\logs'
    [void](New-Item -ItemType Directory -Path $logDir -Force)
    $logFile = Join-Path $logDir ('images-layout-' + (Get-Date -Format 'yyyyMMdd') + '.log')
    $writeLog = {
        param([string]$Message)
        $line = '[' + (Get-Date -Format 'HH:mm:ss.fff') + '] traceId=images-layout-' + $PID + ' ' + $Message
        Write-Host $line
        Add-Content -LiteralPath $logFile -Value $line -Encoding UTF8
    }
    & $writeLog ('Image layout request: source=' + $source + ' target=' + $target + ' firstInstall=' + $FirstInstall)
    $grouped = $false
    if (Test-Path -LiteralPath $ManifestPath) {
        $manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $property = $manifest.PSObject.Properties['imagesLayout']
        $grouped = $property -and $property.Value -eq 'groups'
    }
    if ($grouped) {
        & $writeLog 'Grouped image layout retained; missing pictures and folders stay deleted.'
        return
    }
    if ($FirstInstall -and -not (Test-Path -LiteralPath $target)) {
        if (Test-Path -LiteralPath $source) {
            [void](New-Item -ItemType Directory -Path (Split-Path -Path $target -Parent) -Force)
            Copy-Item -LiteralPath $source -Destination $target -Recurse
            & $writeLog 'Initial images copied with default group.'
        } else {
            & $writeLog 'No initial images supplied; empty image library retained.'
        }
        return
    }
    $sourceDefault = Join-Path $source 'default'
    $targetDefault = [IO.Path]::GetFullPath((Join-Path $target 'default'))
    if (-not $targetDefault.StartsWith($target + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw 'Default directory escaped images' }
    $moved = 0
    if ((Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $sourceDefault)) {
        foreach ($file in Get-ChildItem -LiteralPath $sourceDefault -File) {
            $old = [IO.Path]::GetFullPath((Join-Path $target $file.Name))
            $destination = [IO.Path]::GetFullPath((Join-Path $targetDefault $file.Name))
            if (-not $old.StartsWith($target + [IO.Path]::DirectorySeparatorChar,
                    [StringComparison]::OrdinalIgnoreCase) -or
                    -not $destination.StartsWith($targetDefault + [IO.Path]::DirectorySeparatorChar,
                    [StringComparison]::OrdinalIgnoreCase)) { throw 'Image move escaped target directory' }
            if (Test-Path -LiteralPath $old -PathType Leaf) {
                if (Test-Path -LiteralPath $destination) {
                    & $writeLog ('Both root and default contain ' + $file.Name + '; both user files preserved.')
                    continue
                }
                [void](New-Item -ItemType Directory -Path $targetDefault -Force)
                Move-Item -LiteralPath $old -Destination $destination
                $moved++
                & $writeLog ('Moved existing default image: ' + $file.Name)
            }
        }
    }
    & $writeLog ('Image layout complete: moved=' + $moved + '; no deleted image was restored or overwritten.')
}

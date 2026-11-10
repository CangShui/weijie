$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = [IO.Path]::GetFullPath((Join-Path $root 'cpp\build'))
$expected = [IO.Path]::GetFullPath((Join-Path $root 'cpp\build'))
if ($build -ne $expected) { throw 'Unexpected build path' }
$link = Join-Path $build 'ocr-fast'
if (Test-Path -LiteralPath $link) {
    $item = Get-Item -LiteralPath $link -Force
    if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'ocr-fast is not a junction'
    }
    [IO.Directory]::Delete($link)
}
$pending = New-Object 'System.Collections.Generic.Stack[string]'
$pending.Push($build)
while ($pending.Count -gt 0) {
    foreach ($child in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
        if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to remove link: $($child.FullName)"
        }
        if ($child.PSIsContainer) { $pending.Push($child.FullName) }
    }
}
[IO.Directory]::Delete($build, $true)
Write-Host 'Removed old cpp/build after verification.'

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$ForbiddenExtensions = @(
    '.dll', '.exe', '.lib', '.a', '.pdb', '.obj', '.o', '.pyc',
    '.idb', '.i64', '.id0', '.id1', '.id2', '.id3', '.id4',
    '.onnx', '.tflite', '.task', '.dmp', '.dump', '.log',
    '.whl', '.zip', '.jpg', '.jpeg', '.png', '.mp4'
)
$Violations = Get-ChildItem -LiteralPath $RepositoryRoot -Recurse -File -Force |
    Where-Object {
        $_.FullName -notmatch '[\\/]\.git[\\/]' -and
        $_.FullName -notmatch '[\\/](build(?:-[^\\/]+)?|dist|artifacts|\.deps|\.cache)[\\/]' -and
        $ForbiddenExtensions -contains $_.Extension.ToLowerInvariant()
    }
if ($Violations) {
    $Violations | ForEach-Object { Write-Error "Forbidden source-tree artifact: $($_.FullName)" }
    throw 'Source-tree artifact policy failed.'
}

Write-Host '[OK] Source tree contains only allowed source and metadata files.'

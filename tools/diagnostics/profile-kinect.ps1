[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ConfigPath,
    [string] $ToolchainRoot = $env:ANYGEAR_TOOLCHAIN_ROOT,
    [ValidateRange(5, 300)]
    [int] $Seconds = 30,
    [switch] $SkipBuild,
    [switch] $AllowHarnessFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
$BuildRoot = Join-Path $RepositoryRoot 'build'
$BinRoot = Join-Path $BuildRoot 'bin'
$Plugin = Join-Path $BinRoot 'dance_around_anygear_kinect.dll'
$Harness = Join-Path $BinRoot 'anygear_vp4u_harness.exe'
$ReportRoot = Join-Path $BuildRoot 'diagnostics\kinect-profile'

if (@(Get-Process -Name 'dancearound' -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'dancearound.exe is running. Stop it before profiling Kinect in isolation.'
}
if (-not (Test-Path -LiteralPath "$env:SystemRoot\System32\Kinect10.dll" -PathType Leaf)) {
    throw 'Kinect for Windows SDK 1.8 runtime is not installed.'
}
if (-not $SkipBuild) {
    & (Join-Path $RepositoryRoot 'tools\build.ps1') -ToolchainRoot $ToolchainRoot
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed with exit code $LASTEXITCODE."
    }
}
foreach ($required in @($Plugin, $Harness)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Build output not found: $required"
    }
}

New-Item -ItemType Directory -Path $ReportRoot -Force | Out-Null
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$StdoutPath = Join-Path $ReportRoot "$Stamp.stdout.log"
$StderrPath = Join-Path $ReportRoot "$Stamp.stderr.log"
$SummaryPath = Join-Path $ReportRoot "$Stamp.summary.txt"

Write-Host "[PROFILE] Kinect VP4U backend only, $Seconds seconds; Unity is not started."
$Process = Start-Process `
    -FilePath $Harness `
    -ArgumentList @(
        ('"{0}"' -f $Plugin),
        ('"{0}"' -f $ConfigPath),
        [string]$Seconds
    ) `
    -WorkingDirectory $RepositoryRoot `
    -RedirectStandardOutput $StdoutPath `
    -RedirectStandardError $StderrPath `
    -NoNewWindow `
    -PassThru

$Watch = [Diagnostics.Stopwatch]::StartNew()
$Process.Refresh()
$StartCpuSeconds = $Process.TotalProcessorTime.TotalSeconds
$PeakPrivateBytes = 0L
$PeakWorkingSetBytes = 0L
while (-not $Process.HasExited) {
    Start-Sleep -Milliseconds 250
    try {
        $Process.Refresh()
        $PeakPrivateBytes = [Math]::Max($PeakPrivateBytes, $Process.PrivateMemorySize64)
        $PeakWorkingSetBytes = [Math]::Max($PeakWorkingSetBytes, $Process.WorkingSet64)
    }
    catch {
        # The harness can exit between HasExited and Refresh.
    }
}
$Process.WaitForExit()
$Watch.Stop()
$Process.Refresh()

$CpuSeconds = [Math]::Max(
    0.0,
    $Process.TotalProcessorTime.TotalSeconds - $StartCpuSeconds)
$NormalizedCpu = if ($Watch.Elapsed.TotalSeconds -gt 0) {
    100.0 * $CpuSeconds / $Watch.Elapsed.TotalSeconds /
        [Math]::Max(1, [Environment]::ProcessorCount)
} else { 0.0 }
$Stderr = if (Test-Path -LiteralPath $StderrPath) {
    Get-Content -Raw -Encoding UTF8 -LiteralPath $StderrPath
} else { '' }

$PoseRate = 'not parsed'
$PreviewRate = 'not parsed'
if ($Stderr -match 'pose frames: .* = ([0-9.]+) fps') {
    $PoseRate = "$($Matches[1]) fps"
}
if ($Stderr -match 'preview frames: .* = ([0-9.]+) fps') {
    $PreviewRate = "$($Matches[1]) fps"
}

$Summary = @(
    'dance-around-anygear Kinect isolated profile',
    "Timestamp:        $Stamp",
    "DLL SHA-256:      $((Get-FileHash -LiteralPath $Plugin -Algorithm SHA256).Hash)",
    ('Wall time:        {0:N2} s' -f $Watch.Elapsed.TotalSeconds),
    ('CPU time:         {0:N2} s' -f $CpuSeconds),
    ('CPU normalized:   {0:N2}%' -f $NormalizedCpu),
    ('Peak private:     {0:N1} MiB' -f ($PeakPrivateBytes / 1MB)),
    ('Peak working set: {0:N1} MiB' -f ($PeakWorkingSetBytes / 1MB)),
    "Pose rate:        $PoseRate",
    "Preview rate:     $PreviewRate",
    "Harness exit:     $($Process.ExitCode)",
    "Harness log:      $StderrPath"
)
$Summary | Set-Content -LiteralPath $SummaryPath -Encoding UTF8
$Summary | ForEach-Object { Write-Host $_ }
Write-Host "[REPORT] $SummaryPath"

if ($Process.ExitCode -ne 0 -and -not $AllowHarnessFailure) {
    throw "Kinect harness exited with code $($Process.ExitCode). See $StderrPath"
}

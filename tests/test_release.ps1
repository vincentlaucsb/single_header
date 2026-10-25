param([string] $SourceRoot, [string] $BinaryRoot, [string] $ProjectVersion)
$ErrorActionPreference = "Stop"

# Exercise the release workflow's actual check against isolated CMake metadata.
$workflow = Get-Content -LiteralPath (Join-Path $SourceRoot ".github/workflows/release.yml") -Raw
$match = [regex]::Match($workflow, '(?s)      - name: Verify release version.*?        run: \|\r?\n(.*?)(?=\r?\n      - name:)')
if (-not $match.Success) { throw "Release version check was not found" }
$script = (($match.Groups[1].Value -split '\r?\n' | ForEach-Object { $_ -replace '^          ', '' }) -join "`n")
$work = Join-Path $BinaryRoot ("test-output/release-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path (Join-Path $work "build") -Force | Out-Null
Push-Location $work
try {
  "CMAKE_PROJECT_VERSION:STATIC=$ProjectVersion" | Set-Content -LiteralPath build/CMakeCache.txt
  foreach ($tag in @($ProjectVersion, "v$ProjectVersion")) {
    $env:RELEASE_TAG = $tag
    & ([scriptblock]::Create($script))
  }
  foreach ($tag in @("v0.0.0", "main", "", "v$ProjectVersion-extra")) {
    $env:RELEASE_TAG = $tag
    $rejected = $false
    try { & ([scriptblock]::Create($script)) } catch {
      if ($_.Exception.Message -notlike '*does not match configured version*') { throw }
      $rejected = $true
    }
    if (-not $rejected) { throw "Mismatched release tag '$tag' was accepted" }
  }
  foreach ($metadata in @("UNRELATED:STRING=value", "CMAKE_PROJECT_VERSION:STATIC=$ProjectVersion`nCMAKE_PROJECT_VERSION:STATIC=0.0.0")) {
    $metadata | Set-Content -LiteralPath build/CMakeCache.txt
    $rejected = $false
    try { & ([scriptblock]::Create($script)) } catch {
      if ($_.Exception.Message -notlike '*Missing configured project version*') { throw }
      $rejected = $true
    }
    if (-not $rejected) { throw "Invalid release version metadata was accepted" }
  }
} finally {
  Pop-Location
}

param([string] $SourceRoot, [string] $BinaryRoot)
$ErrorActionPreference = "Stop"

# Exercise the actual composite-action run block with a recording executable.
$action = Get-Content -LiteralPath (Join-Path $SourceRoot "action.yml") -Raw
$block = ($action -split '      run: \|\r?\n')[-1]
$script = (($block -split '\r?\n' | ForEach-Object { $_ -replace '^        ', '' }) -join "`n")
$work = Join-Path $BinaryRoot ("test-output/action-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work -Force | Out-Null
$recorder = Join-Path $work "record.ps1"
'ConvertTo-Json -InputObject @($args) | Set-Content -LiteralPath $env:ACTION_ARGS_OUTPUT' |
  Set-Content -LiteralPath $recorder
$env:SINGLE_HEADER_EXECUTABLE = $recorder
$env:ACTION_ARGS_OUTPUT = Join-Path $work "args.json"
Get-ChildItem Env:INPUT_* | Remove-Item
$env:INPUT_CONFIG = "configured.json"

function Check-Forwarding([string] $Enabled, [string] $Guard, [string[]] $Expected, [string] $Mixed = "") {
  $env:INPUT_DEDUPLICATE_INCLUDES = $Enabled
  $env:INPUT_INCLUDE_GUARD = $Guard
  $env:INPUT_ALLOW_MIXED_INCLUDE = $Mixed
  & ([scriptblock]::Create($script))
  $actual = @(Get-Content -LiteralPath $env:ACTION_ARGS_OUTPUT -Raw | ConvertFrom-Json)
  if (($actual -join '|') -cne ($Expected -join '|')) {
    throw "Unexpected action arguments: $($actual -join '|')"
  }
}
Check-Forwarding "" "" @("--config", "configured.json")
Check-Forwarding "true" "ACTION_GUARD" @("--config", "configured.json", "--deduplicate-includes", "--include-guard", "ACTION_GUARD")
Check-Forwarding "false" "OTHER_GUARD" @("--config", "configured.json", "--no-deduplicate-includes", "--include-guard", "OTHER_GUARD")
Check-Forwarding "true" "ACTION_GUARD" @("--config", "configured.json", "--deduplicate-includes", "--include-guard", "ACTION_GUARD", "--allow-mixed-include", "<thread>", "--allow-mixed-include", '"custom.hpp"') " <thread> `n`"custom.hpp`"`n"
Check-Forwarding "" "" @("--config", "configured.json", "--allow-mixed-include", "<thread>") "<thread>"
$env:INPUT_DEDUPLICATE_INCLUDES = "invalid"
$rejected = $false
try { & ([scriptblock]::Create($script)) } catch {
  if ($_.Exception.Message -notlike '*deduplicate-includes expects true or false*') { throw }
  $rejected = $true
}
if (-not $rejected) { throw "Invalid action boolean was accepted" }

$env:INPUT_DEDUPLICATE_INCLUDES = ""
$env:INPUT_ALLOW_MIXED_INCLUDE = ""
$env:INPUT_MERGE_NAMESPACES = "true"
Check-Forwarding "" "" @("--config", "configured.json", "--merge-namespaces")
$env:INPUT_MERGE_NAMESPACES = "false"
Check-Forwarding "" "" @("--config", "configured.json", "--no-merge-namespaces")
$env:INPUT_MERGE_NAMESPACES = "invalid"
$rejected = $false
try { & ([scriptblock]::Create($script)) } catch {
  if ($_.Exception.Message -notlike '*merge-namespaces expects true or false*') { throw }
  $rejected = $true
}
if (-not $rejected) { throw "Invalid namespace action boolean was accepted" }

$env:INPUT_MERGE_NAMESPACES = ""
$env:INPUT_INLINE_FUNCTIONS = "true"
Check-Forwarding "" "" @("--config", "configured.json", "--inline-functions")
$env:INPUT_INLINE_FUNCTIONS = "false"
Check-Forwarding "" "" @("--config", "configured.json", "--no-inline-functions")
$env:INPUT_INLINE_FUNCTIONS = "invalid"
$rejected = $false
try { & ([scriptblock]::Create($script)) } catch {
  if ($_.Exception.Message -notlike '*inline-functions expects true or false*') { throw }
  $rejected = $true
}
if (-not $rejected) { throw "Invalid inline action boolean was accepted" }

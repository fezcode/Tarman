# Package the approved transparent master (requires Python and Pillow).
$ErrorActionPreference = 'Stop'
python (Join-Path $PSScriptRoot 'make-icon.py') @args
if ($LASTEXITCODE -ne 0) { throw 'Icon export failed (requires Python and Pillow).' }

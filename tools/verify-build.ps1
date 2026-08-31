[CmdletBinding()]
param(
    [string]$BuildPath = (Join-Path $PSScriptRoot '..\build\verified')
)

$ErrorActionPreference = 'Stop'
$fqbn = 'esp32:esp32:waveshare_esp32_s3_touch_lcd_7:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=8M,PartitionScheme=huge_app,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none'
$sketchPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
if ($cliCommand) {
    $cliPath = $cliCommand.Source
} else {
    $ideCli = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
    if (-not (Test-Path -LiteralPath $ideCli)) {
        throw 'arduino-cli was not found. Install Arduino IDE 2.x or add arduino-cli to PATH.'
    }
    $cliPath = $ideCli
}

New-Item -ItemType Directory -Force -Path $BuildPath | Out-Null
Write-Host "Compiling $sketchPath"
Write-Host "Target: $fqbn"
& $cliPath compile --fqbn $fqbn --build-path $BuildPath $sketchPath
if ($LASTEXITCODE -ne 0) {
    throw "Arduino compile failed with exit code $LASTEXITCODE"
}

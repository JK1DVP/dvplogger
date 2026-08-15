$ErrorActionPreference = "Stop"

# ============================================================
# DVPlogger Windows setup
#
# Prepares Arduino-ESP32 2.0.17 and the dvplogger_ext Arduino
# component link required by the build.
#
# Usage:
#   cd <dvplogger-devel>
#   .\setup-windows.ps1
#
# This script:
#   1. Checks that git is available.
#   2. Installs Arduino-ESP32 2.0.17 under:
#        dvplogger\components\arduino
#      when it is not already present.
#   3. Creates a Windows Junction:
#        dvplogger_ext\components\arduino
#          -> dvplogger\components\arduino
#
# A Junction is used instead of a Windows symbolic link so that
# Administrator privileges / Developer Mode are normally not required.
# ============================================================

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrWhiteSpace($RootDir)) {
    $RootDir = (Get-Location).Path
}

$MainDir       = Join-Path $RootDir "dvplogger"
$ExtDir        = Join-Path $RootDir "dvplogger_ext"
$MainComponents = Join-Path $MainDir "components"
$ExtComponents  = Join-Path $ExtDir "components"

$MainArduino = Join-Path $MainComponents "arduino"
$ExtArduino  = Join-Path $ExtComponents "arduino"

$ArduinoRepo = "https://github.com/espressif/arduino-esp32"
$ArduinoTag  = "2.0.17"


function Die {
    param([string]$Message)
    Write-Error "ERROR: $Message"
    exit 1
}


function Require-Command {
    param([string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Die "Required command not found: $Name"
    }
}


function Test-ArduinoTree {
    param([string]$Path)

    return (
        (Test-Path -LiteralPath (Join-Path $Path "CMakeLists.txt") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Path "cores\esp32\Arduino.h") -PathType Leaf)
    )
}


function Remove-LinkOrPlaceholder {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $item = Get-Item -LiteralPath $Path -Force

    # Junction / symbolic link: remove only the link itself.
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        Write-Host "Removing existing link: $Path"
        cmd /c rmdir "`"$Path`""
        if ($LASTEXITCODE -ne 0) {
            Die "Failed to remove existing link: $Path"
        }
        return
    }

    # On Windows, a Git symlink may have been checked out as a tiny text
    # file containing the relative target. It is safe to replace that file.
    if (-not $item.PSIsContainer) {
        $text = ""
        try {
            $text = (Get-Content -LiteralPath $Path -Raw -ErrorAction Stop).Trim()
        } catch {
            Die "Existing path is a regular file and could not be inspected: $Path"
        }

        if ($text -eq "../../dvplogger/components/arduino" -or
            $text -eq "..\..\dvplogger\components\arduino") {
            Write-Host "Removing Git symlink placeholder file: $Path"
            Remove-Item -LiteralPath $Path -Force
            return
        }

        Die "Refusing to replace unexpected regular file: $Path"
    }

    # Never recursively delete a real directory automatically.
    # This avoids destroying a user's independent Arduino checkout.
    if (Test-ArduinoTree $Path) {
        Die @"
$Path is a real Arduino directory, not a Junction.
Move or remove it manually, then run this script again.
The intended Windows layout is:
  $ExtArduino -> $MainArduino
"@
    }

    Die "Refusing to remove unexpected directory: $Path"
}


Write-Host ""
Write-Host "============================================================"
Write-Host " DVPlogger Windows setup"
Write-Host "============================================================"
Write-Host "Repository root : $RootDir"
Write-Host "Arduino target  : $MainArduino"
Write-Host "EXT Junction    : $ExtArduino"
Write-Host ""

Require-Command "git.exe"

if (-not (Test-Path -LiteralPath $MainDir -PathType Container)) {
    Die "Directory not found: $MainDir"
}

if (-not (Test-Path -LiteralPath $ExtDir -PathType Container)) {
    Die "Directory not found: $ExtDir"
}

New-Item -ItemType Directory -Force -Path $MainComponents | Out-Null
New-Item -ItemType Directory -Force -Path $ExtComponents | Out-Null


# ------------------------------------------------------------
# 1. Arduino-ESP32 2.0.17
# ------------------------------------------------------------

if (Test-ArduinoTree $MainArduino) {
    Write-Host "[OK] Arduino component already exists."
} else {
    if (Test-Path -LiteralPath $MainArduino) {
        Die @"
Arduino target exists but does not look complete:
  $MainArduino
Remove/fix that directory and run setup again.
"@
    }

    Write-Host "[SETUP] Cloning Arduino-ESP32 $ArduinoTag ..."
    & git.exe clone `
        --branch $ArduinoTag `
        --depth 1 `
        --recursive `
        $ArduinoRepo `
        $MainArduino

    if ($LASTEXITCODE -ne 0) {
        Die "git clone of Arduino-ESP32 failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-ArduinoTree $MainArduino)) {
        Die "Arduino checkout completed but Arduino.h/CMakeLists.txt was not found."
    }

    Write-Host "[OK] Arduino-ESP32 $ArduinoTag installed."
}


# ------------------------------------------------------------
# 2. dvplogger_ext/components/arduino Junction
# ------------------------------------------------------------

$junctionAlreadyOK = $false

if (Test-Path -LiteralPath $ExtArduino) {
    $item = Get-Item -LiteralPath $ExtArduino -Force

    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        # Resolve via actual content rather than relying only on Target,
        # which differs across PowerShell versions.
        if (Test-ArduinoTree $ExtArduino) {
            $junctionAlreadyOK = $true
            Write-Host "[OK] EXT Arduino link already works."
        }
    }
}

if (-not $junctionAlreadyOK) {
    Remove-LinkOrPlaceholder $ExtArduino

    Write-Host "[SETUP] Creating Arduino Junction ..."
    cmd /c mklink /J "`"$ExtArduino`"" "`"$MainArduino`""

    if ($LASTEXITCODE -ne 0) {
        Die "Failed to create Junction: $ExtArduino"
    }

    if (-not (Test-ArduinoTree $ExtArduino)) {
        Die "Junction was created but Arduino.h cannot be reached through it."
    }

    Write-Host "[OK] Arduino Junction created."
}


# ------------------------------------------------------------
# 3. Final checks
# ------------------------------------------------------------

$MainArduinoH = Join-Path $MainArduino "cores\esp32\Arduino.h"
$ExtArduinoH  = Join-Path $ExtArduino  "cores\esp32\Arduino.h"

Write-Host ""
Write-Host "Final checks:"
Write-Host "  Main Arduino.h : $(Test-Path -LiteralPath $MainArduinoH)"
Write-Host "  EXT  Arduino.h : $(Test-Path -LiteralPath $ExtArduinoH)"

if (-not (Test-Path -LiteralPath $MainArduinoH -PathType Leaf)) {
    Die "Main Arduino.h check failed."
}

if (-not (Test-Path -LiteralPath $ExtArduinoH -PathType Leaf)) {
    Die "EXT Arduino.h check failed."
}

Write-Host ""
Write-Host "============================================================"
Write-Host " Windows setup completed successfully"
Write-Host "============================================================"
Write-Host ""
Write-Host "Next step:"
Write-Host "  Open an ESP-IDF 4.4.x PowerShell, then run:"
Write-Host ""
Write-Host "    cd `"$RootDir`""
Write-Host "    .\build-all.ps1"
Write-Host ""

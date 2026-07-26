<#
.SYNOPSIS
    One-shot dev setup for building/flashing/debugging this nf-interpreter
    (LEDTREES) fork on a fresh Windows machine.

.DESCRIPTION
    Reproduces the whole toolchain on a new computer:
      1. Clones ESP-IDF v5.5.4 (the version this fork requires) if missing.
      2. Installs its toolchains + Python env (idf5.5) and adds kconfiglib.
      3. Installs the nanoff firmware flasher (dotnet global tool).
      4. Generates the machine-specific config files that are gitignored:
         config/user-tools-repos.json, config/user-prefs.json,
         .vscode/settings.json  (paths, cmake.environment, idf.* for the extension)
      5. Sets the per-user (HKCU) ESP-IDF environment variables + PATH.

    launch.json and tasks.json are already portable (they use ${userHome}) and are
    committed in git, so they need no generation.

    Re-runnable (idempotent). Nothing here needs administrator rights EXCEPT the
    one-time JTAG WinUSB driver, which is printed at the end as a manual step.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install-scripts\setup-ledtrees-dev.ps1
#>
[CmdletBinding()]
param(
    [string]$IdfTag    = 'v5.5.4',
    [string]$IdfBase   = (Join-Path $env:USERPROFILE 'esp'),
    [string]$ToolsPath = (Join-Path $env:USERPROFILE '.espressif'),
    [switch]$SkipIdfInstall,
    [switch]$SkipNanoff,
    [switch]$SkipEnv
)

$ErrorActionPreference = 'Stop'
function Info($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "    $m" -ForegroundColor Green }
function Warn($m) { Write-Host "    $m" -ForegroundColor Yellow }

$RepoRoot = Split-Path $PSScriptRoot -Parent
$IdfPath  = Join-Path (Join-Path $IdfBase $IdfTag) 'esp-idf'          # ...\esp\v5.5.4\esp-idf
$IdfFwd   = ($IdfPath -replace '\\','/')                              # forward-slash form for CMake match

Info "Repo:       $RepoRoot"
Info "ESP-IDF:    $IdfPath   ($IdfTag)"
Info "Tools path: $ToolsPath"

# --------------------------------------------------------------------------
# 1. clone ESP-IDF
# --------------------------------------------------------------------------
if (-not $SkipIdfInstall) {
    if (-not (Test-Path (Join-Path $IdfPath '.git'))) {
        Info "Cloning ESP-IDF $IdfTag (a few GB with submodules)..."
        New-Item -ItemType Directory -Force -Path (Split-Path $IdfPath) | Out-Null
        git clone --branch $IdfTag https://github.com/espressif/esp-idf.git --depth 1 --recursive $IdfPath
        if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
        Ok "cloned"
    } else { Ok "ESP-IDF already present - skipping clone" }

    # 2. install tools + python env (clear any inherited venv so install.ps1 doesn't refuse)
    Info "Installing ESP-IDF tools for esp32s3..."
    Remove-Item Env:VIRTUAL_ENV, Env:IDF_PYTHON_ENV_PATH, Env:ESP_IDF_VERSION -ErrorAction SilentlyContinue
    $env:IDF_TOOLS_PATH = $ToolsPath
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & (Join-Path $IdfPath 'install.ps1') esp32s3
    # riscv gdb is not part of the esp32s3 tool set, but the ESP-IDF VS Code
    # extension validates it as a required tool
    $idfBasePy = Get-ChildItem (Join-Path $ToolsPath 'tools\idf-python') -Directory | Select-Object -First 1
    & (Join-Path $idfBasePy.FullName 'python.exe') (Join-Path $IdfPath 'tools\idf_tools.py') install riscv32-esp-elf-gdb
    $ErrorActionPreference = $prev
    Ok "tools installed"
}

# resolve the idf5.5 python env dir (name is fixed by IDF, but discover to be safe)
$VenvDir = Get-ChildItem (Join-Path $ToolsPath 'python_env') -Directory -ErrorAction SilentlyContinue |
           Where-Object Name -like 'idf5.5*' | Select-Object -First 1
if (-not $VenvDir) { throw "idf5.5 python env not found under $ToolsPath\python_env - did the tools install succeed?" }
$VenvPy = Join-Path $VenvDir.FullName 'Scripts\python.exe'

# 3. kconfiglib (nanoFramework Kconfig system requirement)
Info "Installing kconfiglib into the idf5.5 python env..."
& $VenvPy -m pip install "kconfiglib>=14.1.0" | Out-Null
Ok "kconfiglib ready"

# --------------------------------------------------------------------------
# 4. activate IDF once to capture the exact tool paths for settings.json
# --------------------------------------------------------------------------
Info "Activating ESP-IDF to capture tool paths..."
Remove-Item Env:VIRTUAL_ENV, Env:IDF_PYTHON_ENV_PATH, Env:ESP_IDF_VERSION, Env:OPENOCD_SCRIPTS, Env:ESP_ROM_ELF_DIR -ErrorAction SilentlyContinue
$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$basePy = Get-ChildItem (Join-Path $ToolsPath 'tools\idf-python') -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if ($basePy) { $env:PATH = "$($basePy.FullName);$env:PATH" }
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
& (Join-Path $IdfPath 'export.ps1') | Out-Null
$ErrorActionPreference = $prev

$toolBins = ($env:PATH -split ';' | Where-Object { $_ -and $_.ToLower().StartsWith($ToolsPath.ToLower()) })
if (-not $toolBins) { throw "export.ps1 did not add any tool paths - activation failed" }
$customExtraPaths = ($toolBins -join ';')
$pythonEnvPath    = $env:IDF_PYTHON_ENV_PATH
$openocdScripts   = $env:OPENOCD_SCRIPTS
$espRomElfDir     = $env:ESP_ROM_ELF_DIR
Ok "captured $($toolBins.Count) tool paths"

# --------------------------------------------------------------------------
# 5. generate gitignored config files
# --------------------------------------------------------------------------
Info "Writing config/user-tools-repos.json ..."
$userTools = [ordered]@{
    version = 4
    configurePresets = @(
        [ordered]@{
            name = 'user-tools-repos'; hidden = $true
            description = 'Generated by setup-ledtrees-dev.ps1'
            cacheVariables = [ordered]@{
                TOOL_HEX2DFU_PREFIX=$null; TOOL_SRECORD_PREFIX=$null
                CHIBIOS_SOURCE_FOLDER=$null; FREERTOS_SOURCE_FOLDER=$null
                CHIBIOS_CONTRIB_SOURCE=$null; CHIBIOS_HAL_SOURCE=$null
                STM32_CUBE_PACKAGE_SOURCE=$null; LWIP_SOURCE=$null; MBEDTLS_SOURCE=$null
                FATFS_SOURCE=$null; LITTLEFS_SOURCE=$null
                ESP32_IDF_PATH=$IdfFwd
                TI_SL_CC32xx_SDK_SOURCE=$null; TI_SL_CC13xx_26xx_SDK_SOURCE=$null
                TI_XDCTOOLS_SOURCE=$null; TI_SYSCONFIG_SOURCE=$null
                AZURERTOS_SOURCE_FOLDER=$null; NETXDUO_SOURCE_FOLDER=$null
            }
        }
    )
}
$userTools | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $RepoRoot 'config\user-tools-repos.json')
Ok "user-tools-repos.json (ESP32_IDF_PATH=$IdfFwd)"

$userPrefsPath = Join-Path $RepoRoot 'config\user-prefs.json'
if (-not (Test-Path $userPrefsPath)) {
    Copy-Item (Join-Path $RepoRoot 'config\user-prefs.TEMPLATE.json') $userPrefsPath
    Ok "user-prefs.json (from template)"
} else { Ok "user-prefs.json already present - kept" }

Info "Writing .vscode/settings.json ..."
$UH = $ToolsPath  # literal here (settings.json is generated per machine, no need for ${userHome})
$settings = [ordered]@{
    'idf.espIdfPath'      = '${env:IDF_PATH}'
    'idf.espIdfPathWin'   = $IdfPath
    'idf.pythonBinPathWin'= (Join-Path $pythonEnvPath 'Scripts\python.exe')
    'idf.toolsPathWin'    = $ToolsPath
    'idf.customExtraPaths'= $customExtraPaths
    'idf.customExtraVars' = [ordered]@{
        OPENOCD_SCRIPTS   = $openocdScripts
        IDF_CCACHE_ENABLE = '1'
        ESP_ROM_ELF_DIR   = $espRomElfDir
        IDF_TARGET        = 'esp32s3'
    }
    'idf.adapterTargetName' = 'esp32s3'
    'idf.openOcdConfigs'    = @('board/esp32s3-builtin.cfg')
    'idf.flashType'         = 'UART'
    'cmake.cmakePath'       = (($toolBins | Where-Object { $_ -match '\\cmake\\' } | Select-Object -First 1) + '\cmake.exe')
    'cmake.environment'     = [ordered]@{
        IDF_PATH            = $IdfFwd
        IDF_PYTHON_ENV_PATH = $pythonEnvPath
        ESP_IDF_VERSION     = '5.5'
        IDF_TOOLS_PATH      = $ToolsPath
        OPENOCD_SCRIPTS     = $openocdScripts
        ESP_ROM_ELF_DIR     = $espRomElfDir
        IDF_CCACHE_ENABLE   = '1'
        PATH                = ($customExtraPaths + ';${env:PATH}')
    }
    'files.associations'    = [ordered]@{ 'string.h'='c'; 'random'='cpp' }
}
$settings | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $RepoRoot '.vscode\settings.json')
Ok "settings.json generated"

# --------------------------------------------------------------------------
# 6. per-user environment variables (HKCU - no admin needed)
# --------------------------------------------------------------------------
if (-not $SkipEnv) {
    Info "Setting per-user ESP-IDF environment (HKCU)..."
    $k = 'HKCU:\Environment'
    Set-ItemProperty $k -Name IDF_PATH            -Value $IdfFwd
    Set-ItemProperty $k -Name IDF_PYTHON_ENV_PATH -Value $pythonEnvPath
    Set-ItemProperty $k -Name ESP_IDF_VERSION     -Value '5.5'
    Set-ItemProperty $k -Name IDF_TOOLS_PATH      -Value $ToolsPath
    # prepend tool bins to the user PATH (deduped)
    $curPath = (Get-Item $k).GetValue('Path','','DoNotExpandEnvironmentNames')
    $keep = $curPath -split ';' | Where-Object { $_ -and ($_.ToLower() -notlike "*$($ToolsPath.ToLower())*") -and ($_ -notlike "*\esp\v5.*") }
    $newPath = (($toolBins + $keep) -join ';')
    Set-ItemProperty $k -Name Path -Value $newPath -Type ExpandString
    Ok "HKCU env set (takes effect in newly launched apps; sign out/in for all)"
}

# --------------------------------------------------------------------------
# 7. nanoff
# --------------------------------------------------------------------------
if (-not $SkipNanoff) {
    if (Get-Command dotnet -ErrorAction SilentlyContinue) {
        Info "Installing/updating nanoff (dotnet global tool)..."
        if (Test-Path (Join-Path $env:USERPROFILE '.dotnet\tools\nanoff.exe')) { dotnet tool update -g nanoff } else { dotnet tool install -g nanoff }
        Ok "nanoff ready (run with DOTNET_ROLL_FORWARD=LatestMajor if only .NET 9/10 present)"
    } else { Warn "dotnet not found - skipping nanoff (install .NET SDK, then: dotnet tool install -g nanoff)" }
}

Write-Host ""
Info "DONE. Next steps:"
Write-Host @"
  1. Restart VS Code (or sign out/in) so the new environment is picked up.
  2. Build:  CMake Tools -> select preset (e.g. ESP32_LEDTREES_V2) -> Build.
  3. Run:    Run and Debug -> 'nanoCLR: Flash + Monitor' (F5), enter the COM port.
  4. JTAG debug ('nanoCLR: Flash + Debug') needs the WinUSB driver on the board's
     JTAG interface (one-time, per machine, requires admin):
        $env:USERPROFILE\Downloads\idf-env.exe driver install --espressif
     If the JTAG interface ends up on bare Microsoft winusb.inf (Code 31 /
     LIBUSB_ERROR_NOT_FOUND), rebind it to the signed libwdi driver:
        pnputil /add-driver C:\Windows\INF\<esp-jtag-oemNN>.inf /install
        pnputil /remove-device "<USB\VID_303A&PID_1001&MI_02...>"; pnputil /scan-devices
     (see SETUP.md for the full driver walkthrough).
"@ -ForegroundColor Gray

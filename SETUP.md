# Настройка dev-окружения LEDTREES на новом компьютере

Этот форк собирается под **ESP-IDF v5.5.4** (таргеты `ESP32_LEDTREES_*`, ESP32-S3).
Бо́льшая часть окружения машинно-специфична (абсолютные пути, имя пользователя,
драйвер), поэтому переносится не копированием файлов, а одним скриптом.

## Что уже в репозитории (переносится через git)

- `.vscode/launch.json` и `.vscode/tasks.json` — универсальные (через `${userHome}`),
  работают на любой машине после установки IDF 5.5.4:
  - **Run and Debug → `nanoCLR: Flash + Monitor`** — прошивка + монитор (спросит COM-порт).
  - **`nanoCLR: Flash + Debug` / `Attach + Debug`** — JTAG-отладка нативного кода.
  - Задачи `nanoCLR: Flash / Monitor / Flash + Monitor` (Terminal → Run Task).
- `install-scripts/setup-ledtrees-dev.ps1` — бутстрап-скрипт (ниже).

## Что генерируется на каждой машине (gitignored)

- ESP-IDF v5.5.4 в `%USERPROFILE%\esp\v5.5.4\esp-idf` + тулчейны в `%USERPROFILE%\.espressif`
- `config/user-tools-repos.json`, `config/user-prefs.json`
- `.vscode/settings.json` (пути IDF, `cmake.environment` — окружение 5.5.4 для CMake Tools)
- Переменные окружения пользователя (HKCU) + PATH
- `nanoff` (dotnet global tool)

## Шаги на новой машине

### 1. Предпосылки
- Git, PowerShell, [Visual C++ Build Tools], VS Code + расширения:
  `espressif.esp-idf-extension`, `ms-vscode.cmake-tools`, `ms-vscode.cpptools`.
- Python 3.11+ в PATH — без него `install.ps1` из ESP-IDF не сможет поставить
  тулчейны (алиас Microsoft Store не считается):
  `winget install --id Python.Python.3.11 --scope user --silent`
- Длинные пути: `New-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' LongPathsEnabled -Value 1 -PropertyType DWORD -Force` (админ, один раз).

### 2. Клонировать и запустить бутстрап
```powershell
git clone <repo-url> ; cd nf-interpreter ; git checkout dev
powershell -ExecutionPolicy Bypass -File install-scripts\setup-ledtrees-dev.ps1
```
Скрипт склонирует IDF 5.5.4 (несколько ГБ), поставит тулчейны + `kconfiglib` + `nanoff`,
сгенерирует конфиги под эту машину и пропишет переменные окружения. Повторный запуск
безопасен (идемпотентен). Флаги: `-SkipIdfInstall`, `-SkipNanoff`, `-SkipEnv`.

### 3. Перезапустить VS Code
Полностью закрыть и открыть (не «Reload Window») — чтобы подхватилось новое окружение.

### 4. Собрать и запустить
- **CMake Tools** → выбрать preset (напр. `ESP32_LEDTREES_V2`) → **Build**.
- **Run and Debug** → `nanoCLR: Flash + Monitor` → **F5** → ввести COM-порт.

### 5. JTAG-отладка — драйвер WinUSB (один раз, нужен админ)
ESP32-S3 отдаёт по USB два интерфейса: CDC (COM-порт, работает сразу) и JTAG
(`Interface 2`), которому нужен драйвер **WinUSB**.

Рекомендуемый способ (Zadig на Windows 11 может крашиться — `0xc000041d`):
```powershell
# скачать idf-env и поставить подписанный драйвер Espressif (от администратора):
Invoke-WebRequest https://github.com/espressif/idf-env/releases/latest/download/win64.idf-env.exe -OutFile $env:USERPROFILE\Downloads\idf-env.exe
$env:USERPROFILE\Downloads\idf-env.exe driver install --espressif --wait
```
Проверка: OpenOCD должен увидеть ядра:
```powershell
& "$env:USERPROFILE\.espressif\tools\openocd-esp32\<ver>\openocd-esp32\bin\openocd.exe" `
  -s "$env:USERPROFILE\.espressif\tools\openocd-esp32\<ver>\openocd-esp32\share\openocd\scripts" `
  -f board/esp32s3-builtin.cfg -c "init; targets; shutdown"
```

**Если JTAG остался на голом Microsoft `winusb.inf` (устройство в статусе Error, Code 31,
`LIBUSB_ERROR_NOT_FOUND`)** — переключить на подписанный libwdi-драйвер:
```powershell
# найти INF (Provider libwdi) в C:\Windows\INF\oemNN.inf, затем от администратора:
$inst = (Get-PnpDevice | ? InstanceId -like 'USB\VID_303A&PID_1001&MI_02*').InstanceId
pnputil /add-driver "C:\Windows\INF\<oemNN>.inf" /install
pnputil /remove-device "$inst"; pnputil /scan-devices
```
После этого `Status = OK`, драйвер = `oem28.inf (libwdi)`, и `nanoCLR: Flash + Debug`
работает по F5.

## Заметки
- `nanoff` собран под .NET 8; если стоит только .NET 9/10 — запускать с
  `DOTNET_ROLL_FORWARD=LatestMajor` (или поставить .NET 8 Desktop Runtime).
- Прошивка вручную без JTAG: `nanoff --update --target ESP32_S3 --serialport COMx --clrfile build\nanoCLR.bin`.

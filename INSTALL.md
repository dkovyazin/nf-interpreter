# Установка и настройка

Инструкция предназначена исключительно для Windows.

## Подготовка Windows

Установите:

- https://visualstudio.microsoft.com/en/visual-cpp-build-tools/

Увеличте максимальную длинну пути папок:

```PowerShell
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
```

Включите выполнение скриптов PowerShell:

```PowerShell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy Unrestricted
```

## Подготовка VS Code

Откройте VS Code и установите расширения:

- https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension
- https://marketplace.visualstudio.com/items?itemName=twxs.cmake
- https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools
- https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools
- https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools-themes
- https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools-extension-pack (или вместо последних 3-х)

После или во время установки расширений их растраивать не нужно.

## Установка ESP-IDF в VS Code

1. В VS Code наберите `Ctrl+Shift+P` и введите `configure esp-idf extension`. Откроется страница установки ESP-IDF.
2. Выберите `EXPRESS`.
3. Выберите версию ESP-IDF 5.2.5, так как [nf-interpreter](https://github.com/nanoframework/nf-interpreter) поддерживает только её. Нельзя выбирать 5.2.6, так как там бага с vfs.
4. Остальные параметры оставьте как есть и нажмите `Install`.
5. Дождитесь завершения скачивания и установки всех пакетов.

## Настройка ESP-IDF для Windows

К сожалению после установки ESP-IDF не выполняет настройки окружения Windows, поэтому их нужно выполнить самостоятельно.

Откройте редактор переменных окружения текущего пользователя Windows и в нём откройте редактирование переменной `PATH`.

1. Откройте терминал в VS Code
2. Включите выполнение скриптов PowerShell `Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy Unrestricted`
3. Перейдите в папку ESP-IDF 5.2.2 `cd C:\idf\esp\v5.2.5\esp-idf`
4. Добавьте в `PATH` путь к Python `C:\idf\.espressif\tools\idf-python\3.11.2`. Не добавляйте, если у вас уже установлен Python.
5. Выполните установку `./install.ps1`
6. Получите список путей для PATH `./export.ps1`
7. Добавьте в окружение пользователя все указанные переменные и пути в `PATH`
8. Добавьте переменную `IDF_PATH` со значением `C:/idf/esp/v5.2.5/esp-idf`. Слеш обязательно такой!
9. Добавьте переменную `IDF_TOOLS_PATH` со значением `C:\idf\.espressif`
10. Удалите в `PATH` путь к Python `C:\idf\.espressif\tools\idf-python\3.11.2`

## Настройка сборки стандартного Firmware

Добавить в файл `.vscode/settings.json`:

```JSON
{
  "idf.espIdfPath": "${env:IDF_PATH}",
  "idf.espIdfPathWin": "C:\\idf\\esp\\v5.2.5\\esp-idf",
  "idf.pythonInstallPath": "C:\\idf\\.espressif\\tools\\idf-python\\3.11.2\\python.exe",
  "idf.pythonBinPathWin": "C:\\idf\\.espressif\\python_env\\idf5.2_py3.11_env\\Scripts\\python.exe",
  "idf.gitPathWin": "C:\\idf\\.espressif\\tools\\idf-git\\2.39.2\\cmd\\git.exe",
  "idf.toolsPathWin": "C:\\idf\\.espressif",
  "idf.customExtraVars": {
    "OPENOCD_SCRIPTS": "C:\\idf\\.espressif\\tools\\openocd-esp32\\v0.12.0-esp32-20241016\\openocd-esp32\\share\\openocd\\scripts",
    "IDF_CCACHE_ENABLE": "1",
    "ESP_ROM_ELF_DIR": "C:\\idf\\.espressif\\tools\\esp-rom-elfs\\20230320",
    "IDF_TARGET": "esp32s3"
  },
  "idf.portWin": "COM3",
  "idf.adapterTargetName": "esp32s3",
  "idf.openOcdConfigs": [
    "board/esp32s3-builtin.cfg"
  ],
  "idf.flashType": "UART",
  "files.associations": {
    "string.h": "c",
    "random": "cpp"
  }
}
```

Откопируйте файлы `user-prefs.TEMPLATE.json` и `user-tools-repos.TEMPLATE.json` в папке `config` и удалите в названии `.TEMPLATE`.

В файле `user-tools-repos.json`:
1. Поменяйте название с `user-tools-repos-local` на `user-tools-repos`
2. В `cacheVariables` во всех значениях поставьте `null`
3. Для переменно `ESP32_IDF_PATH` установите значение типа `C:/idf/esp/v5.2.5/esp-idf`
4. Перезапустите VS Code
5. После перезапуска вам будет сразу предложено выбрать preset, выберите `ESP32_LEDTREES_V2_DEV`
6. Нажмите F7 и дождитель завершения конфигурирования проекта. Это не быстро!

Установите если потребуется пакет esp-idf-kconfig:

```PowerShell
cd "C:\idf\esp\v5.2.5\esp-idf"
python -m pip install -U esp-idf-kconfig
```

## Прошивка Firmware

```PowerShell
cd "build"
nanoff --update --target ESP32_S3 --serialport COM4 --clrfile "nanoCLR.bin"
```

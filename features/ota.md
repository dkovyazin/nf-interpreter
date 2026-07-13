# OTA: обновление прошивки и приложения единым bundle

Система OTA-обновлений для устройств LEDTREES (ESP32-S3, 16 MB flash, ESP-IDF 5.5.4,
таргеты `ESP32_LEDTREES_V1/V2` и их `_DEV`-варианты).

> **Решение (2026-07-12):** целевая архитектура — единый firmware bundle; проект
> `LedTrees.Device.Loader` удалён, приложение — единственный managed-образ.
> Раздельная схема с `Assembly.Load`/`OtaManager` выведена из эксплуатации —
> см. §14 «Отклонённые альтернативы».
>
> **Статус: фаза 1 реализована и проверена на железе** (сквозные OTA-циклы,
> лёгкий и полный, до состояния CONFIRMED; веб-админка и DHCP на AP работают).
> Подробности — §2 и §13.

## 1. Архитектура

Одна единица обновления — **firmware bundle** (`.ltfw`), три секции в одном артефакте:

| Секция | Что это | Куда ставится |
|---|---|---|
| nanoCLR | интерпретатор + нативные API | app-партиция `ota_0`/`ota_1` (A/B) |
| managed-образ | `App` + библиотеки (единая сборка, точка входа `Program.Main`) | партиция `deploy` (через `stage`) |
| web-ассеты | `wwwroot` админки | SD-карта, каталог `ota/wwwroot-{ver}` |

Всё собирается вместе, версия одна на всё ⇒ несовместимых комбинаций
«прошивка ↔ приложение» на устройстве не существует по построению. Приложение вызывается
напрямую (`Startup.Run()`), без `Assembly.Load` и reflection; managed-код исполняется
из flash (memory-mapped), не занимая RAM.

Два сценария доставки одного и того же артефакта (решение принимает устройство,
сравнив `clrSha256` секции с работающим образом):

- **Полное обновление** — nanoCLR изменился: пишутся все секции, точка фиксации —
  переключение ota-слота (`otadata`, атомарно), откат — штатный rollback ESP-IDF.
- **Лёгкое обновление** — nanoCLR не менялся: применяются только managed-образ и
  web-ассеты (~0.5 MB вместо ~1.9 MB), точка фиксации — флаг в NVS, откат —
  восстановление из `backup` по счётчику неудачных стартов.

```
              ┌────────────────────────────┐
              │  Источник .ltfw:           │
              │  upload-update (HTTP) /    │
              │  DeviceConsole (TCP) /     │
              │  update-сервер (фаза 2)    │
              └─────────────┬──────────────┘
                            │
              ┌─────────────┴──────────────┐
              │  BundleInstaller (managed) │
              │  кэш на SD; clrSha256 == ? │
              └──────┬──────────────┬──────┘
            ПОЛНОЕ   │              │   ЛЁГКОЕ
                     ▼              ▼
        nanoCLR → неактивный слот   (секция CLR пропущена)
        managed → stage             managed → stage
        wwwroot-{ver} → SD          wwwroot-{ver} → SD
        commit: otadata             commit: NVS-флаг (StageCommit)
        reboot                      reboot
                     └──────┬───────┘
                            ▼
        boot-hook (до старта CLR): deploy → backup, stage → deploy
        App: старт сервисов → Bundle.ConfirmIfPending (иначе автооткат)
```

## 2. Реализация (2026-07-12)

### nf-interpreter (ветка `dev`)

| Компонент | Где |
|---|---|
| Таблица разделов OTA | [partitions_nanoclr_16mb_ota.csv](targets/ESP32/_IDF/esp32s3/partitions_nanoclr_16mb_ota.csv) |
| Опция `NF_FEATURE_OTA` | [Kconfig.features](Kconfig.features), включена во всех LEDTREES defconfig |
| Нативное ядро (state machine в разделе `ota_state`, слоты, stage, commit, confirm) | [targetHAL_Ota.c](targets/ESP32/_common/targetHAL_Ota.c) / [targetHAL_Ota.h](targets/ESP32/_include/targetHAL_Ota.h) |
| Boot-hook `NF_Ota_ApplyPending()` | [app_main.c](targets/ESP32/_IDF/esp32s3/app_main.c), после `nvs_flash_init`, до задач CLR |
| Регион nanoCLR через `esp_ota_get_running_partition()` | [Device_BlockStorage.c](targets/ESP32/_common/Device_BlockStorage.c) |
| Interop `interoplib.Ota` (нативная часть, чексумма `0x52D58C6F`) | [InteropAssemblies/interoplib](InteropAssemblies/interoplib) |
| Упаковщик `.ltfw` для CI | [scripts/pack-ltfw.py](scripts/pack-ltfw.py) |
| Rollback бутлоадера | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` в sdkconfig lt_v1/v2 |
| Прошивочные задачи (0x20000 + сброс otadata) | .vscode/tasks.json, launch.json |

### ledtrees-esp32 (ветка `dev`)

| Компонент | Где |
|---|---|
| Точка входа + recovery | `LedTrees.Device.App/Program.cs` (проект Loader удалён; `OutputType Exe`) |
| Идентичность bundle (версия, `Id` для группы, WebPath, Confirm) | `LedTrees.Device/OTA/Bundle.cs` |
| Установка `.ltfw` (кэш на SD, полное/лёгкое, wwwroot) | `LedTrees.Device/OTA/BundleInstaller.cs` (+`Crc32.cs`) |
| Interop `Ota` (managed) | `interoplib/Ota.cs` |
| Приём `.ltfw`: HTTP | `WebService` — `POST /upload-update` |
| Приём `.ltfw`: TCP-терминал | `DeploymentTerminal` (команда 2), клиент — `LedTrees.DeviceConsole` (сам пакует bundle) |
| Раздача `.ltfw` группе | `DownloadService` (отдаёт кэш с SD по `Bundle.Id`) |
| Подтверждение после health-check | `Startup.Run` → `Bundle.ConfirmIfPending()` после старта сервисов |

Версия bundle — `AssemblyVersion` сборки `LedTrees.Device` (сейчас 2.1.0.0);
`Board.SoftwareVersion` читает её же, упаковщики берут её из DLL.

### Проверено на железе

Сквозные OTA-циклы через `POST /upload-update`: лёгкий (managed+web) и полный
(со сменой A/B-слота) — оба до состояния `CONFIRMED`; wwwroot распакован на SD,
админка работает (WebSocket :8080, `otaSlot: 4 = Confirmed` в init), DHCP-сервер AP
выдаёт адреса клиентам. Managed-образ фактически 27 сборок / **322 KB** — запас
в deploy-партиции (2944 KB) ~9×.

### Попутно найденные и исправленные баги

- **toolchain esp32s3/s2 без `-fno-builtin-*`**: xtensa GCC 14 разворачивал запись
  структуры в APB-регистр в побайтовые `s8i` — ломало SDMMC (карта не инициализировалась,
  `sdmmc_host_wait_for_event 0x107`). Фикс: [toolchain.xtensa-esp32s3-elf.cmake](CMake/toolchain.xtensa-esp32s3-elf.cmake)
  (+s2), требует чистого build-каталога.
- **DHCP-сервер на AP** терялся при миграции на IDF 5.5: nf снимает флаг DHCP_SERVER
  с netif, а `esp_wifi_set_config` перезапускает AP и убивает уже запущенный сервер.
  Фикс: явный `esp_netif_dhcps_start` **после** конфигурации AP, с ожиданием поднятия
  netif ([NF_ESP32_Wireless.cpp](targets/ESP32/_Network/NF_ESP32_Wireless.cpp)).
- **Kconfig не перегенерировал `.config` при смене пресета** (таймстампы) — фикс через
  stamp-файл в [NF_Kconfig.cmake](CMake/Modules/NF_Kconfig.cmake).
- SDMMC ограничен 20 MHz на S3/P4 (GPIO-матрица, esp-idf #8521); диагностика монтирования
  через `esp_rom_printf` в не-RTM сборках.
- Фронтенд админки: полифилл `crypto.randomUUID` (недоступен по HTTP) и миграция
  `DOM.tag` на options-API `@brandup/ui` (`{ class: ... }`).

### Факты платформы, на которых стоит дизайн

- **nanoBooter отсутствует** — загрузкой управляет штатный бутлоадер ESP-IDF ⇒ для
  nanoCLR используется A/B-механизм IDF (`otadata` + rollback).
- **Deploy-регион memory-mapped и исполняется на месте** — работающий код не может
  перезаписать сам себя; отсюда `stage` и boot-hook.
- Managed-образ deploy-региона = .pe-файлы подряд, каждый выровнен до 4 байт — так их
  читает CLR ([CLRStartup.cpp:243](src/CLR/Startup/CLRStartup.cpp#L243)).
- CRC32 для проверки stage — стандартный zlib-совместимый: `esp_rom_crc32_le` (нативно)
  ↔ `LedTrees.Device.OTA.Crc32` (managed) ↔ `zlib.crc32` (CI).

## 3. Таблица разделов (16 MB)

Файл [partitions_nanoclr_16mb_ota.csv](targets/ESP32/_IDF/esp32s3/partitions_nanoclr_16mb_ota.csv):

```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000
otadata,   data, ota,      0xf000,   0x2000     # активный слот (A/B), атомарный
phy_init,  data, phy,      0x11000,  0x1000
ota_state, data, 0x87,     0x12000,  0x2000     # состояние OTA-автомата (вне NVS)
ota_0,     app,  ota_0,    0x20000,  0x1A0000   # nanoCLR слот A (1664 KB)
ota_1,     app,  ota_1,    0x1C0000, 0x1A0000   # nanoCLR слот B (1664 KB)
deploy,    data, 0x84,     0x360000, 0x2E0000   # managed-образ, рабочая копия (2944 KB)
stage,     data, 0x85,     0x640000, 0x2E0000   # staging нового managed-образа
backup,    data, 0x86,     0x920000, 0x2E0000   # копия старого managed-образа для отката
config,    data, littlefs, 0xC00000, 0x400000   # littlefs 4 MB: конфигурация
```

Замечания:

- app-партиции выровнены по 0x10000 (требование IDF); nanoCLR без BLE занимает слот
  на ~79% (1.35 MB).
- `stage`/`backup` — data-партиции с кастомными subtype, бутлоадер их не трогает.
- managed-образ фактически 322 KB при 2944 KB партиции — при желании `deploy/stage/backup`
  можно ужать (например до 1 MB) и отдать место littlefs; пока не трогаем.
- **wwwroot и кэш `.ltfw` живут на SD-карте** (`{MmcPath}/ota/`), littlefs — только
  конфигурация. SD есть на всех устройствах (обязательна: `Board.Init` требует её).
- Смещения `nvs`/`config` изменились относительно старой таблицы ⇒ **переход — только
  по USB** (§10).

## 4. Артефакт: `firmware-{ver}.ltfw`

```
struct LtFwHeader {                     // 172 байта (v2), little-endian, в начале файла
    uint32_t magic;                     // 'LTFW'
    uint32_t formatVersion;             // 2
    char     version[32];               // единая версия bundle (NUL-padded utf-8)
    uint32_t clrOffset,  clrSize;       // nanoCLR.bin (app-образ IDF)
    uint8_t  clrSha256[32];
    uint32_t mngdOffset, mngdSize;      // managed-образ deploy-региона
    uint8_t  mngdSha256[32];
    uint32_t webOffset,  webSize;       // web-секция: count + [nameLen,name,dataLen,data]*
    uint8_t  webSha256[32];
    uint32_t clrCrc32;                  // zlib-CRC32 секций (v2): устройство проверяет их
    uint32_t mngdCrc32;                 // в InstallFromFile ДО каких-либо действий —
    uint32_t webCrc32;                  // e2e-целостность без SHA256-примитива на девайсе
};
```

Sha256 секций — для манифеста/сервера (подпись — фаза 3); на устройстве целостность
проверяется по CRC32 из заголовка (SHA256-примитива в managed-стеке нет).

- Смещения секций в заголовке ⇒ можно скачивать заголовок отдельно (Range) и решать
  «полное/лёгкое» до скачивания артефакта. Текущие транспорты (upload-update,
  терминал) передают файл целиком — устройство кэширует его на SD и решает локально.
- nanoCLR.bin — артефакт сборки таргета (`build/nanoCLR.bin`); managed-образ —
  конкатенация .pe из bin приложения (4-байтовое выравнивание); web-секция — файлы
  `wwwroot` (относительные пути, подкаталоги поддерживаются).
- Упаковщики: [scripts/pack-ltfw.py](scripts/pack-ltfw.py) (CI; также выдаёт JSON-фрагмент
  для манифеста) и `LedTrees.DeviceConsole` (локально, версия из `LedTrees.Device.dll`).

## 5. Поток обновления

### Полное (nanoCLR изменился)

```
BundleInstaller (managed)               Boot-hook (нативный, до задач CLR)
-------------------------               -----------------------------------
0. .ltfw → кэш на SD (идемпотентно)
1. CLR-секция → неактивный слот
   (Ota.FirmwareBegin/Write/End;
    слот НЕ переключён)
2. managed-секция → stage
   (Ota.StageBegin/Write/StageCommit:
    проверка CRC32; рекорд STAGED сразу
    привязан к новому слоту — пассивен,
    пока устройство не загрузится из него)
3. web-секция → SD ota/wwwroot-{ver}
4. Ota.CommitFull():
   esp_ota_set_boot_partition  ← ТОЧКА ФИКСАЦИИ (атомарно)
5. reboot
                                        6. запуск из нового слота (PENDING_VERIFY);
                                           слот == target_slot и state == STAGED:
                                           a. deploy → backup
                                           b. рекорд: state = COPYING
                                           c. erase deploy; stage → deploy; CRC
                                           d. рекорд: state = APPLIED, attempts = 1
                                        7. старт CLR → Program.Main → Startup.Run()
8. сервисы запущены (health-check)
   → Bundle.ConfirmIfPending():
     esp_ota_mark_app_valid…
     рекорд: state = CONFIRMED;
     чистка чужих wwwroot-*
```

До шага 4 все записи (кэш, слот, stage, wwwroot-{ver}) — пассивные данные: сбой на любом
этапе оставляет устройство на старой версии. Привязка STAGED-рекорда к слоту прямо в
`StageCommit` (шаг 2) закрывает окно «ребут между StageCommit и CommitFull»: без неё
boot-hook применил бы новый managed-образ лёгким путём против старого nanoCLR. После
шага 4 всё решает boot-hook, каждый его шаг идемпотентен (источник копирования не
затирается до успеха).

**Откат.** Новый слот грузится в `PENDING_VERIFY`; если `Confirm()` не вызван до
следующего ресета (крэш CLR, watchdog, не поднялся managed-стек) — бутлоадер возвращает
старый слот. Boot-hook на старом слоте видит `target_slot != running_slot` при
`state ∈ {COPYING, APPLIED}` и восстанавливает deploy из `backup` — старый стек целиком.
Это закрывает главный риск: «слот откатился, а managed-образ в deploy уже новый».

### Лёгкое (clrSha256 совпал с текущим слотом)

Шаги 1 и 4 пропускаются: точка фиксации — рекорд `STAGED` (внутри `StageCommit`),
без привязки к слоту (`target = NONE`). Boot-hook применяет stage → deploy так же. Поскольку ota-слот не
менялся, rollback IDF недоступен — откат делает сам boot-hook: при `state = APPLIED` он
инкрементирует `boot_attempts`; если приложение трижды не дошло до `Confirm()` —
`backup → deploy`, `state = ROLLED_BACK`.

Замечание: сравнение `clrSha256` идёт с фактическим образом в слоте
(`esp_partition_get_sha256`). USB-прошивка с `--flash_size detect` патчит заголовок
образа, поэтому первое OTA после USB-прошивки может пойти полным путём — это безвредно.

### Web-ассеты

`wwwroot` живёт на SD, версионированно: `ota/wwwroot-{ver}`. Managed-код скомпилирован
со своей версией bundle и обращается только к «своему» каталогу — указатель не нужен,
откат согласован автоматически (старый образ ищет старый каталог, он не тронут).
После `Confirm()` каталоги других версий удаляются.

## 6. Состояния (раздел `ota_state`)

```
IDLE → STAGED(target_slot?) → COPYING → APPLIED → CONFIRMED
                                            │
                                            └→ ROLLED_BACK (лёгкое: boot_attempts ≥ 3;
                                                полное: откат слота IDF + restore backup)
```

Состояние хранится в **собственном raw-разделе `ota_state`** (subtype 0x87,
два сектора по 4 KB) — не в NVS. Запись — единый CRC-защищённый рекорд
(`magic`, `sequence`, `state`, `target`, `attempts`, `stage_len`, `stage_crc`),
пишется пинг-понгом в «не текущий» сектор по образцу `otadata`: сбой питания
посреди записи оставляет предыдущий рекорд нетронутым, а каждый переход
автомата атомарен по построению (весь рекорд целиком, порядок ключей не важен).

Почему не NVS: восстановление после повреждения NVS (`nvs_flash_erase` +
retry в `app_main`) стирало бы и OTA-состояние — в худшем случае
(APPLIED + откат слота бутлоадером) устройство оставалось бы с парой
«старый CLR + новый managed» без шанса на автоматический restore из backup.
Отдельный раздел отвязывает судьбу автомата от жизненного цикла NVS.
Littlefs не подходит: недоступен boot-hook'у до инициализации ФС.

## 7. API

### Interop `interoplib.Ota` (нативный слой, реализован)

```csharp
public class Ota
{
    // nanoCLR (неактивный слот)
    static void   FirmwareBegin(int totalSize);
    static void   FirmwareWrite(byte[] data, int length);
    static void   FirmwareEnd();                      // валидация образа средствами IDF
    static byte[] GetRunningFirmwareSha256();         // решение «полное/лёгкое»

    // managed-образ (stage-партиция)
    static void   StageBegin(int totalSize);
    static void   StageWrite(byte[] data, int length);
    static void   StageCommit(uint crc32);            // zlib-CRC32; фиксация лёгкого пути

    static void   CommitFull();                       // фиксация полного пути (otadata)
    static void   Confirm();                          // mark_app_valid + CONFIRMED
    static OtaState State { get; }
    static bool   IsPendingConfirm { get; }
}
```

Обёртки бросают исключения; reboot — на вызывающей стороне (`Board.Reboot()`).

### Managed-слой (реализован)

- `Bundle` — идентичность: `Version` (= AssemblyVersion `LedTrees.Device`),
  детерминированный `Id` (16 байт, для группового протокола), `WebPath`,
  `BundleFilePath` (кэш на SD), `ConfirmIfPending()`.
- `BundleInstaller` — `DownloadToFile(stream, len, logger)` (кэш на SD, идемпотентно),
  `TryReadHeader()` (валидация кэша перед раздачей), `InstallFromFile(logger)`
  (полное/лёгкое → `Ota.*` → wwwroot → reboot).

Оркестрация доставки (манифест, HTTPS, ретраи) — будущий `OtaUpdater` (фаза 2).

## 8. Манифест и доставка (фаза 2)

`GET https://ota.ledtrees.example/{channel}/{hw}/manifest.json`:

```json
{
  "bundle": {
    "version": "2.4.0",
    "url": ".../firmware-2.4.0.ltfw",
    "size": 1900000,
    "sha256": "…",
    "clrSha256": "…",
    "mngdSha256": "…",
    "webSha256": "…"
  },
  "signature": "base64(ECDSA-P256 подпись канонизированного manifest)"
}
```

- Каналы `dev` / `beta` / `prod`; `hw`: `lt-v1` / `lt-v2`. Канал — в конфиге устройства.
- `clrSha256` в манифесте позволяет решить «полное/лёгкое» до скачивания артефакта.
- Опрос по таймеру с джиттером + push «проверь обновления» по каналу управления.
- Докачка: HTTP Range с продолжением записи по сохранённому смещению.
- `pack-ltfw.py --manifest-fragment` уже генерирует значения для манифеста.

Уже работающие транспорты (вне сервера): `POST /upload-update` (админка/скрипты) и
TCP-терминал `DeviceConsole` — оба принимают готовый `.ltfw`.

## 9. Групповое обновление: роли MAIN и SCREEN

Инсталляция — группа устройств: MAIN (оркестратор экрана) + N×SCREEN (плееры).
Роль — runtime-конфигурация (`ConfigurationManager.Role`), **bundle один для всех ролей**.

**SCREEN'ы обновляются автоматически вслед за MAIN** по схеме «регистрация → сравнение →
NeedUpdate → скачивание с MAIN». Механика реализована (перевод существующего протокола
на bundle), сквозная проверка на группе устройств — впереди:

```
Update-сервер ──HTTPS──▶ MAIN ──UDP: NeedUpdate──▶ SCREEN 1..N
                          │  ◀──TCP: .ltfw ────────┘
                          └─ кэш .ltfw на SD (Bundle.BundleFilePath)
```

1. **Только MAIN ходит на update-сервер.** SCREEN'ам сервер (и интернет вообще) не нужен —
   их источник истины MAIN.
2. MAIN обновляет **сначала себя**; раздача группе — только после собственного
   `Confirm()` (кэш `.ltfw` уже на SD).
3. При регистрации SCREEN сообщает `Bundle.Id` (формат кадра не менялся — 16 байт).
   Несовпадение → `NeedUpdate` → SCREEN скачивает `.ltfw` с MAIN по TCP
   (`DownloadService`, тип `Deploy`) и применяет поток §5; «полное/лёгкое» каждый
   SCREEN решает сам по своему `clrSha256`. `DownloadService` проверяет целостность
   кэша (`TryReadHeader`) перед раздачей.
4. **Сходимость, а не монотонность**: группа сводится к версии MAIN, в том числе вниз.
   Anti-downgrade (§11) действует только на паре «MAIN ↔ сервер».
5. Новый или сервисный SCREEN с любой версией догоняет группу при первом подключении.
6. **Доверие end-to-end** (фаза 2): вместе с bundle MAIN отдаёт кэшированный подписанный
   манифест; SCREEN проверяет подпись и sha256 сам.
7. **Окно смешанных версий** закрывается протоколом: кадры несут
   `DeviceProtocolVersion`; SCREEN с несовпадающей версией выпадает из показа до синка.
   **Кадр регистрации заморожен (V1)** — при смене протокола меняются только команды:
   иначе после отката MAIN новый SCREEN не сможет зарегистрироваться у старого MAIN
   (неизвестная версия кадра игнорируется) и не получит downgrade — сходимость (п.4)
   сломается.
8. **Планирование** (фаза 2): раскатка в простое; ограниченная параллельность;
   SCREEN после трёх неудач остаётся на старой версии и репортится в телеметрию.

## 10. Миграция существующих устройств

Проверенный порядок первичной установки (он же — миграция со старой таблицы), один раз
по USB:

1. `esptool erase_flash`;
2. `esptool write_flash 0x0 bootloader.bin 0x8000 partition-table.bin
   0xf000 ota_data_initial.bin 0x20000 nanoCLR.bin 0x360000 <managed-образ>`
   (всё из `build/`, managed-образ — конкатенация .pe; есть задача
   `nanoCLR: Flash (esptool)` в tasks.json — без deploy-региона);
3. `nanoff --filedeployment <filedeploy.json>` доставляет wwwroot на SD
   (`D:\ota\wwwroot-{ver}`) по тому же COM-порту через Wire Protocol storage
   operations — сеть не нужна;
4. первая загрузка: приложение поднимает AP, DHCP выдаёт адрес; все дальнейшие
   обновления — OTA (`POST /upload-update` с `.ltfw`).

Шаги 2–3 автоматизированы скриптом `ledtrees-esp32/scripts/build-and-flash.ps1`
(таска `deploy: полный (nanoCLR + managed + wwwroot) → COM` в tasks.json).
Требование прошивки для шага 3: RX/TX-буферы драйвера USB-Serial-JTAG не меньше
WP-кадра (`USB_JTAG_BUFFER_SIZE 2048` в `WireProtocol_HAL_Interface.c`) — при
256 байтах ISR молча выбрасывает байты, когда приёмный поток занят записью на SD,
и деплой файлов больше ~4 КБ зависает; плюс запас стека `ReceiverThread` 6 КБ
(путь FATFS+SDMMC не помещается в 3 КБ) и Append по `offset` в
`targetHAL_StorageOperation.cpp`.

Dev-итерации не меняются: деплой managed-кода из Visual Studio в deploy-регион по USB —
штатный workflow nanoFramework. JTAG-прошивка nanoCLR — по 0x20000 (launch.json сбрасывает
otadata, чтобы плата не грузила старый слот).

## 11. Безопасность (фаза 2/3)

1. **TLS + pinning**: корневой сертификат update-сервера в config-блоке
   (`NF_FEATURE_HAS_CONFIG_BLOCK=y` уже включён).
2. **Подпись манифеста** ECDSA P-256, публичный ключ вшит в прошивку; артефакт
   аутентифицируется по sha256 (общему и посекционным) из подписанного манифеста.
3. **Целостность на устройстве** (уже работает): CRC32 stage при записи и после
   копирования в deploy; для nanoCLR — встроенная проверка `esp_ota_end`.
4. **Anti-downgrade**: отказ от версий ниже текущей (кроме канала `dev`).
5. **(фаза 3)** Secure Boot V2 + подписанные app-образы IDF. Необратимый eFuse —
   включать только после обкатки пайплайна.

## 12. Матрица сбоев

| Сбой | Результат |
|---|---|
| Питание при скачивании .ltfw | кэш на SD перезаписывается при следующей попытке; точки фиксации не пройдены |
| Питание между STAGED и `set_boot_partition` | STAGED-данные пассивны — работает старая версия |
| Питание при копировании stage→deploy | state = COPYING, после ресета копирование повторяется |
| Новый CLR не стартует / паникует (полное) | бутлоадер откатывает слот; boot-hook восстанавливает deploy из backup |
| Managed-стек не дошёл до `Confirm()` (полное) | нет mark_valid → откат слота при следующем ресете + restore backup |
| Managed-стек не дошёл до `Confirm()` (лёгкое) | boot_attempts ≥ 3 → boot-hook: backup → deploy, ROLLED_BACK |
| Приложение упало на старте при pending-обновлении | `Program.Main` перезагружает устройство → соответствующий откат |
| Питание при записи wwwroot-{ver} | каталог не используется до старта новой версии; перезапись идемпотентна |
| Битый .ltfw (заголовок/CRC) | отбрасывается до точек фиксации (`TryReadHeader`/`StageCommit`) |
| NVS переполнен/повреждён | `app_main` делает erase+retry вместо паники; OTA-состояние в разделе `ota_state` не затрагивается |
| Питание при записи ota_state | пинг-понг секторов: действует предыдущий рекорд, переход повторится/отменится штатно |
| Деплой из VS при незавершённом OTA | стирание deploy-региона сбрасывает состояние в IDLE — boot-hook не тронет свежий деплой |
| SD переполнена/IO-ошибка при распаковке web | web извлекается ДО `StageBegin` — исключение до точек фиксации, устройство на старой версии |
| Обрыв upload нового бандла на MAIN | кэш пишется во временный файл с атомарной подменой — рабочий `bundle.ltfw` не разрушается, раздача группе продолжается |
| Upload на MAIN во время раздачи SCREEN'у | подмена кэша и раздача под общим `Bundle.CacheLock` — SCREEN не получит рваный файл |
| Краш `Board.Init` (SD умерла) на pending-буте | `Program.Main` ловит и ребутит → штатный откат (bootloader / boot-hook) |
| Ребут между `StageCommit` и `CommitFull` (полное) | рекорд привязан к слоту ещё в `StageCommit` — пассивен до загрузки из нового слота |
| Питание SCREEN при скачивании с MAIN | обычный сбой скачивания; `NeedUpdate` при следующей регистрации повторит |
| SCREEN трижды не смог обновиться | остаётся на старой версии, выпадает из показа, репорт в телеметрию |
| MAIN откатился, часть SCREEN'ов обновилась | группа сводится к версии MAIN: SCREEN'ы даунгрейдятся (§9 п.4) |

Во всех сценариях отката устройство остаётся на согласованной тройке
«nanoCLR + managed-образ + wwwroot» одной версии, а группа сходится к версии MAIN.

## 13. План внедрения

**Фаза 1 — механика OTA: ✅ сделана и проверена на железе (2026-07-12).**
Состав — §2. Коммиты: 7 в nf-interpreter (`4eb8129e..10ced896` + DHCP-fix),
3 в ledtrees-esp32 (`7c4b3b6`, `0a2f27c`, `b3ef895`).

**Фаза 2 — эксплуатация: код реализован 2026-07-13, не проверен на железе**

Сделано:
- `OtaUpdaterService` (только MAIN): опрос манифеста по HTTP(S), anti-downgrade
  (понижение — только при `allowDowngrade=1`), скачивание с докачкой (HTTP Range,
  temp-файл переживает обрыв), установка через `BundleInstaller`. Конфигурация —
  `update.cfg` (key=value: `url`, `interval`, `allowDowngrade`) в каталоге
  конфигурации; без файла сервис спит. Джиттер опроса ±10% + стартовая задержка.
- **E2E-целостность секций**: формат `.ltfw` v2 — заголовок 172 байта с zlib-CRC32
  каждой секции; `InstallFromFile` проверяет все секции ДО каких-либо действий
  (вместо sha256, которого нет в стеке). Упаковщики обновлены (pack-ltfw.py,
  `LtfwImage`); v1-бандлы отклоняются.
- **Ретрай застрявших SCREEN**: `ScreenService` раз в 10 минут сверяет `OtaHash`
  подключённых устройств и повторяет `NeedUpdate` отставшим — только когда
  собственный bundle подтверждён и кэш цел.
- Телеметрия отката: `otaSlot` в init-модели админки + лог
  «предыдущее обновление откачено» при каждом старте в состоянии `ROLLED_BACK`.

Осталось (инфраструктура/железо):
- Update-сервер: статика + манифесты в CI (`pack-ltfw.py --manifest-fragment`
  готов), каналы dev/beta/prod. Подпись манифеста — фаза 3 (нет ECDSA).
- Групповая раскатка в бою: планирование в простое, проверка на реальной
  группе MAIN+SCREEN.
- Миграционная прошивка остальных устройств (порядок — §10).
- Постепенная раскатка (процент устройств по хэшу serial) — требует сервера.

**Фаза 3 — усиление**
- Secure Boot V2, шифрование flash, дельта-обновления при необходимости.

## 14. Отклонённые альтернативы

**Раздельные единицы обновления: firmware (nanoCLR+Loader) + app bundle
(`Assembly.Load` из файловой системы)** — так работало до 2026-07-12. Отклонено:
.pe-сборки завязаны на точные версии managed-библиотек и чексуммы нативных сборок,
почти каждое обновление nanoCLR ломает установленное приложение. Управление этим
требует ABI-уровней, сцепленных транзакций «прошивка+приложение», pending-слотов и
пар `previous` в манифесте — класс сложности, который единый bundle устраняет по
построению. Дополнительные минусы раздельной схемы: `Assembly.Load` копирует сборки
в RAM (вместо исполнения из flash), хрупкий `BinaryFormatter`-формат `state.dat`,
reflection-контракт `Startup.Run/Stop` как ещё одна ось совместимости. Цена
объединения — размер обновления (решено лёгким сценарием) и reboot при каждом
обновлении (его не было и в старой схеме).

## 15. Открытые вопросы

- Перекройка партиций: managed-образ занимает 322 KB из 2944 KB — можно ужать
  `deploy/stage/backup` и отдать место littlefs/SD-независимому хранилищу. Не срочно.
- Нужен ли `backup` (2944 KB), или при откате достаточно перекачать старую версию с
  сервера? Backup спасает офлайн-устройства — пока оставляем.
- Канал push-уведомлений для «проверь обновления»: MQTT или только периодический опрос?
- Health-check перед `Confirm()` сейчас = «сервисы запущены»; стоит ли ждать
  подключения Wi-Fi STA / первого кадра показа — решить по опыту эксплуатации.
- Консоль на плате с внешним USB-UART (COM-порт) в normal-run не читается — для полевой
  диагностики полагаться на веб-канал (`AdminDebugger` → WebSocket) и телеметрию.

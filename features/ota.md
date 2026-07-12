# OTA: обновление прошивки и приложения единым bundle

Проект системы OTA-обновлений для устройств LEDTREES (ESP32-S3, 16 MB flash, ESP-IDF 5.5.4,
таргеты `ESP32_LEDTREES_V1/V2` и их `_DEV`-варианты).

> **Решение (2026-07-12):** целевая архитектура — единый firmware bundle.
> `LedTrees.Device.Loader` и `LedTrees.Device.App` объединяются: приложение статически
> линкуется с Loader'ом и живёт в deploy-регионе. Раздельная схема с динамической
> загрузкой приложения через `Assembly.Load` (текущий `OtaManager`) выводится из
> эксплуатации — см. §14 «Отклонённые альтернативы».

## 1. Архитектура

Одна единица обновления — **firmware bundle**, три секции в одном артефакте:

| Секция | Что это | Куда ставится |
|---|---|---|
| nanoCLR | интерпретатор + нативные API | app-партиция `ota_0`/`ota_1` (A/B) |
| managed-образ | `Loader` + `App` + библиотеки, статически слинкованы | партиция `deploy` (через `stage`) |
| web-ассеты | `wwwroot` | littlefs, каталог `wwwroot-{ver}` |

Всё собирается вместе в CI, версия одна на всё ⇒ несовместимых комбинаций
«прошивка ↔ приложение» на устройстве не существует по построению. Приложение вызывается
напрямую (`Startup.Run()`), без `Assembly.Load` и reflection; managed-код исполняется
из flash (memory-mapped), не занимая RAM.

Два сценария доставки одного и того же артефакта:

- **Полное обновление** — nanoCLR изменился: пишутся все секции, точка фиксации —
  переключение ota-слота (`otadata`, атомарно), откат — штатный rollback ESP-IDF.
- **Лёгкое обновление** — `clrSha256` совпадает с текущим слотом: по HTTP Range
  скачиваются только managed-образ и web-ассеты (~1 MB вместо ~2.5 MB), точка фиксации —
  флаг в NVS, откат — восстановление из `backup` по счётчику неудачных стартов.

```
              ┌────────────────────────────┐
              │  Update-сервер (HTTPS)     │
              │  manifest.json + .ltfw     │
              └─────────────┬──────────────┘
                            │
              ┌─────────────┴──────────────┐
              │  OtaUpdater (managed)      │
              │  clrSha256 == текущий?     │
              └──────┬──────────────┬──────┘
            ПОЛНОЕ   │              │   ЛЁГКОЕ
                     ▼              ▼
        nanoCLR → неактивный слот   (секция CLR пропущена)
        managed → stage             managed → stage
        wwwroot-{ver} → littlefs    wwwroot-{ver} → littlefs
        commit: otadata             commit: NVS-флаг
        reboot                      reboot
                     └──────┬───────┘
                            ▼
        boot-hook (до старта CLR): deploy → backup, stage → deploy
        Loader: health-check → Confirm (иначе автооткат)
```

## 2. Текущее состояние

### Что есть сейчас (ledtrees-esp32)

- `LedTrees.Device.Loader` в deploy-регионе; приложение `LedTrees.Device.App` он грузит
  динамически через `Assembly.Load(byte[])` из littlefs (A/B-каталоги `ota/app1|app2`,
  `state.dat`) — реализовано в
  [OtaManager.cs](../../../DevOps/ledtrees/ledtrees-esp32/nanoframework/main/LedTrees.Device/OTA/OtaManager.cs).
  В целевой архитектуре этот механизм упраздняется.
- Доставка только локальная: Wi-Fi AP `LedTrees_startup` + `DeploymentTerminal`
  (утилита `LedTrees.DeviceConsole`). Остаётся как recovery/сервисный путь.
- Прошивка обновляется только по USB (`nanoff` / esptool).
- **Группа уже самосинхронизируется по приложению**: SCREEN при регистрации на MAIN
  сообщает свой `OtaManager.Hash`; при несовпадении MAIN шлёт `NeedUpdateCommand`
  ([ScreenService.cs:129](../../../DevOps/ledtrees/ledtrees-esp32/nanoframework/main/LedTrees.Device.App/Screen/Services/ScreenService.cs#L129)),
  и SCREEN скачивает деплой с MAIN по TCP (`DownloadService`). Этот механизм
  сохраняется и расширяется на полный bundle (§9).

### Факты платформы, влияющие на дизайн

Текущая таблица разделов ([partitions_nanoclr_16mb.csv](targets/ESP32/_IDF/esp32s3/partitions_nanoclr_16mb.csv)):

```
nvs,      data, nvs,      0x9000,   0x6000
phy_init, data, phy,      0xf000,   0x1000
factory,  app,  factory,  0x10000,  0x1A0000    # nanoCLR
deploy,   data, 0x84,     0x1B0000, 0x2E0000    # managed-код
config,   data, littlefs, 0x490000, 0x300000
# свободно с 0x790000 — ~8.4 MB
```

- **nanoBooter отсутствует** (`CONFIG_NF_TARGET_HAS_NANOBOOTER` не установлен) — загрузкой
  управляет штатный бутлоадер ESP-IDF ⇒ для nanoCLR используем A/B-механизм IDF.
- **Deploy-регион memory-mapped и исполняется на месте**
  ([Device_BlockStorage.c:53](targets/ESP32/_common/Device_BlockStorage.c#L53)) —
  работающий код не может перезаписать сам себя; отсюда `stage` и boot-hook.
- Партиция nanoCLR ищется по subtype `factory` в `FixUpBlockRegionInfo()`
  ([Device_BlockStorage.c:141](targets/ESP32/_common/Device_BlockStorage.c#L141)) —
  при переходе на `ota_0/ota_1` это надо менять.
- CSV таблицы разделов выбирается по размеру flash в
  [binutils.ESP32.cmake:455](CMake/binutils.ESP32.cmake#L455).
- Для доставки всё есть: Wi-Fi, `System.Net` (HTTPS), `System.Security.Cryptography`,
  config-блок для сертификатов, `littlefs`.

## 3. Таблица разделов (16 MB)

Файл `targets/ESP32/_IDF/esp32s3/partitions_nanoclr_16mb_ota.csv`:

```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000
otadata,   data, ota,      0xf000,   0x2000     # активный слот (A/B), атомарный
phy_init,  data, phy,      0x11000,  0x1000
ota_0,     app,  ota_0,    0x20000,  0x1A0000   # nanoCLR слот A (1664 KB)
ota_1,     app,  ota_1,    0x1C0000, 0x1A0000   # nanoCLR слот B (1664 KB)
deploy,    data, 0x84,     0x360000, 0x2E0000   # managed-образ, рабочая копия (2944 KB)
stage,     data, 0x85,     0x640000, 0x2E0000   # staging нового managed-образа
backup,    data, 0x86,     0x920000, 0x2E0000   # копия старого managed-образа для отката
config,    data, littlefs, 0xC00000, 0x400000   # littlefs 4 MB: конфиг + wwwroot-{ver} ×2
```

Замечания:

- app-партиции выровнены по 0x10000 (требование IDF); размер слота = текущему `factory`.
- `stage`/`backup` — data-партиции с кастомными subtype, бутлоадер их не трогает.
- deploy 2944 KB теперь вмещает Loader **и** App с библиотеками — размер сохранён от
  текущей таблицы, где так уже живёт полный managed-стек (Loader тянет почти все
  зависимости App). После замера можно перекроить в пользу littlefs.
- littlefs 4 MB: конфигурация + две версии `wwwroot` (текущая и предыдущая для отката).
- Смещения `nvs`/`config` меняются ⇒ **переход на новую таблицу — только по USB** (§10).

## 4. Артефакт: `firmware-{ver}.ltfw`

```
struct LtFwHeader {                     // фиксированный размер, в начале файла
    uint32_t magic;                     // 'LTFW'
    uint32_t formatVersion;
    char     version[32];               // единая версия bundle
    uint32_t clrOffset,  clrSize;       // nanoCLR.bin (app-образ IDF)
    uint8_t  clrSha256[32];
    uint32_t mngdOffset, mngdSize;      // managed-образ deploy-региона
    uint8_t  mngdSha256[32];
    uint32_t webOffset,  webSize;       // web-секция: count + [nameLen,name,dataLen,data]*
    uint8_t  webSha256[32];
};
```

- Смещения секций в заголовке ⇒ устройство скачивает заголовок (один маленький Range-
  запрос), решает «полное или лёгкое», и дальше качает только нужные секции по Range.
- nanoCLR.bin — обычный артефакт CI-сборки таргета; managed-образ — выход
  `nanoff --deploy` по объединённому решению Loader+App; web-секция — в том же формате
  файловых записей, что текущий app-бандл (`nameLen, name, dataLen, data`).
- Упаковщик — `scripts/pack-ltfw.py` в CI, версия проставляется из тега релиза.

## 5. Поток обновления

### Полное (nanoCLR изменился)

```
OtaUpdater (managed)                    Boot-hook (нативный, до ClrStartup)
--------------------                    -----------------------------------
1. CLR-секция → неактивный слот
   (esp_ota_begin/write/end;
    слот НЕ переключён)
2. managed-секция → stage, sha256
3. web-секция → littlefs wwwroot-{ver}
4. NVS: state = STAGED,
        target_slot = <новый слот>
5. esp_ota_set_boot_partition  ← ТОЧКА ФИКСАЦИИ (атомарно)
6. reboot
                                        7. запуск из нового слота (PENDING_VERIFY);
                                           слот == target_slot и state == STAGED:
                                           a. deploy → backup
                                           b. NVS: state = COPYING
                                           c. erase deploy; stage → deploy; CRC
                                           d. NVS: state = APPLIED, boot_attempts = 0
                                        8. старт CLR → Loader → Startup.Run()
9. health-check пройден (Wi-Fi поднят,
   основной цикл работает)
   → Ota.Confirm():
     esp_ota_mark_app_valid…
     NVS: state = CONFIRMED
```

До шага 5 все записи (слот, stage, wwwroot-{ver}) — пассивные данные: сбой на любом
этапе оставляет устройство на старой версии, докачка по Range. После шага 5 всё решает
boot-hook, каждый его шаг идемпотентен (источник копирования не затирается до успеха).

**Откат.** В `sdkconfig.default_lt_v*.esp32s3` включается
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Новый слот грузится в `PENDING_VERIFY`; если
`Confirm()` не вызван до следующего ресета (крэш CLR, watchdog, не поднялся managed-стек,
таймаут health-check) — бутлоадер возвращает старый слот. Boot-hook на старом слоте видит
`target_slot != running_slot` при `state ∈ {COPYING, APPLIED}` и восстанавливает deploy
из `backup` — старый стек целиком. Это закрывает главный риск: «слот откатился, а
managed-образ в deploy уже новый».

### Лёгкое (clrSha256 совпал с текущим слотом)

Шаг 1 пропускается, точка фиксации — `NVS: state = STAGED` без `target_slot`
(шаг 5 не выполняется). Boot-hook применяет stage → deploy так же. Поскольку ota-слот
не менялся, rollback IDF недоступен — откат делает сам boot-hook: при `state = APPLIED`
он инкрементирует `boot_attempts`; если приложение трижды не дошло до `Confirm()` —
`backup → deploy`, `state = ROLLED_BACK`. Устройство работает на прежней версии и
сообщает об инциденте в телеметрию.

### Web-ассеты

`wwwroot` не влезает в deploy-регион, поэтому живёт в littlefs, версионированно:
каталог `wwwroot-{ver}`. Managed-код скомпилирован со своей версией bundle и обращается
только к «своему» каталогу — указатель не нужен, откат согласован автоматически (старый
образ ищет старый каталог, он не тронут). После `Confirm()` каталоги других версий
удаляются.

## 6. Состояния (NVS)

```
IDLE → STAGED(target_slot?) → COPYING → APPLIED → CONFIRMED
                                            │
                                            └→ ROLLED_BACK (лёгкое: boot_attempts ≥ 3;
                                                полное: откат слота IDF + restore backup)
```

NVS выбран вместо littlefs/config: доступен boot-hook'у до инициализации файловой
системы, атомарен на уровне записи ключа, переживает переформатирование littlefs.

## 7. Managed API (нативный interop `LedTrees.Ota`)

Только то, чего нельзя сделать из managed-кода (ota-слоты, stage, NVS-состояния);
вся оркестрация — в `OtaUpdater` (managed):

```csharp
public static class Ota
{
    // nanoCLR (неактивный слот)
    static void  FirmwareBegin(int totalSize);      // esp_ota_begin
    static void  FirmwareWrite(byte[] chunk, int len);
    static void  FirmwareEnd();                     // esp_ota_end (валидация образа)
    static byte[] RunningFirmwareSha256 { get; }    // для решения «полное/лёгкое»

    // managed-образ (stage-партиция)
    static void  StageBegin(int totalSize);         // erase stage
    static void  StageWrite(byte[] chunk, int len);
    static void  StageCommit(byte[] sha256);        // проверка + NVS STAGED

    static void  CommitFullAndReboot();             // set_boot_partition + reboot
    static void  CommitLightAndReboot();            // только NVS STAGED + reboot
    static void  Confirm();                         // mark_app_valid + NVS CONFIRMED
    static bool  IsPendingConfirm { get; }          // приложение должно вызвать Confirm
    static string BundleVersion { get; }
}
```

Скачивание, ретраи, парсинг манифеста, Range-докачка, индикация прогресса (на гирлянде!) —
на managed-стороне: этот слой итерируется без пересборки CLR.

## 8. Манифест и доставка

`GET https://ota.ledtrees.example/{channel}/{hw}/manifest.json`:

```json
{
  "bundle": {
    "version": "2.4.0",
    "url": ".../firmware-2.4.0.ltfw",
    "size": 2530000,
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
- Опрос по таймеру с джиттером + push «проверь обновления» по существующему каналу
  управления (MQTT/BLE-команда).
- Докачка: HTTP Range с продолжением записи по сохранённому смещению.

## 9. Групповое обновление: роли MAIN и SCREEN

Инсталляция — группа устройств: MAIN (оркестратор экрана) + N×SCREEN (плееры).
Роль — runtime-конфигурация (`ConfigurationManager.Role`), **bundle один для всех ролей**.

**SCREEN'ы обновляются автоматически вслед за MAIN** — по существующей схеме
«регистрация → сравнение → NeedUpdate → скачивание с MAIN», расширенной на полный bundle:

```
Update-сервер ──HTTPS──▶ MAIN ──UDP: NeedUpdate──▶ SCREEN 1..N
                          │  ◀──TCP: .ltfw ────────┘
                          └─ кэш .ltfw + манифест на SD (Storage.MmcPath)
```

1. **Только MAIN ходит на update-сервер.** SCREEN'ам сервер (и интернет вообще) не нужен —
   их источник истины MAIN.
2. MAIN скачивает `.ltfw` + манифест, кэширует на SD, обновляет **сначала себя** (§5).
   Раздача группе начинается только после собственного `Confirm()` — не раскатываем на
   группу то, что не пережило health-check на MAIN.
3. При регистрации SCREEN сообщает `BundleVersion` (вместо нынешнего Guid-хэша).
   Несовпадение с версией MAIN → `NeedUpdate` → SCREEN скачивает `.ltfw` с MAIN по TCP
   и применяет тот же поток §5; решение «полное/лёгкое» каждый SCREEN принимает сам по
   своему `clrSha256`.
4. **Сходимость, а не монотонность**: группа сводится к версии MAIN, в том числе вниз —
   если MAIN откатился, обновившиеся раньше SCREEN'ы даунгрейдятся к нему.
   Anti-downgrade (§11) действует только на паре «MAIN ↔ сервер».
5. Новый или сервисный SCREEN с любой заводской версией догоняет группу автоматически
   при первом подключении — как и сейчас.
6. **Доверие end-to-end**: вместе с bundle MAIN отдаёт кэшированный подписанный манифест;
   SCREEN проверяет подпись и sha256 сам — MAIN как транспорт не является доверенным
   звеном.
7. **Окно смешанных версий** (MAIN уже новый, SCREEN'ы ещё нет) закрывается протоколом:
   UDP/TCP-протокол группы несёт `DeviceProtocolVersion` (уже есть) и меняется
   консервативно; SCREEN с несовпадающей версией исключается из показа до синка —
   это происходит естественно, т.к. `NeedUpdate` срабатывает при регистрации.
8. **Планирование**: раскатка на группу — в простое, не во время показа; SCREEN'ы
   обновляются последовательно или с малой параллельностью (память MAIN ограничена).
   SCREEN, трижды не сумевший обновиться, остаётся на старой версии, выпадает из показа
   и репортится в телеметрию через MAIN.

## 10. Миграция существующих устройств

Переход на OTA-таблицу разделов — один раз по USB:

1. `esptool erase_flash` (смещения nvs/config меняются, littlefs пересоздаётся);
2. полный образ с новой таблицей: bootloader + partition table + ota_0 (nanoCLR) +
   deploy (managed-образ) + wwwroot;
3. повторная провизия (AP `LedTrees_startup` + `DeviceConsole` — уже есть).

Дальше всё — только OTA. Новые устройства прошиваются OTA-образом с завода.
Dev-итерации не меняются: деплой managed-кода из Visual Studio в deploy-регион по USB —
штатный workflow nanoFramework.

## 11. Безопасность

1. **TLS + pinning**: корневой сертификат update-сервера в config-блоке
   (`NF_FEATURE_HAS_CONFIG_BLOCK=y` уже включён).
2. **Подпись манифеста** ECDSA P-256, публичный ключ вшит в прошивку; артефакт
   аутентифицируется по sha256 (общему и посекционным) из подписанного манифеста —
   файлы можно раздавать с CDN.
3. **Целостность на устройстве**: sha256 каждой секции при записи; для nanoCLR
   дополнительно встроенная проверка `esp_ota_end`.
4. **Anti-downgrade**: отказ от версий ниже текущей (кроме канала `dev`).
5. **(опционально, фаза 3)** Secure Boot V2 + подписанные app-образы IDF. Необратимый
   eFuse — включать только после обкатки пайплайна.

## 12. Матрица сбоев

| Сбой | Результат |
|---|---|
| Питание при скачивании любой секции | точка фиксации не пройдена — работает старая версия; докачка по Range |
| Питание между STAGED и `set_boot_partition` | то же: STAGED-данные пассивны |
| Питание при копировании stage→deploy | state = COPYING, после ресета копирование повторяется |
| Новый CLR не стартует / паникует (полное) | watchdog ×3 → бутлоадер откатывает слот; boot-hook восстанавливает deploy из backup |
| Managed-стек не дошёл до `Confirm()` (полное) | нет mark_valid → откат слота при следующем ресете + restore backup |
| Managed-стек не дошёл до `Confirm()` (лёгкое) | boot_attempts ≥ 3 → boot-hook: backup → deploy, ROLLED_BACK |
| Приложение зависло, не крэш | таймаут health-check → reboot без Confirm → соответствующий откат |
| Питание при записи wwwroot-{ver} | каталог не используется до старта новой версии; докачка/перезапись идемпотентны |
| Битый манифест / подпись / hash mismatch | артефакт отброшен до записи во flash |
| Питание SCREEN при скачивании с MAIN | обычный сбой скачивания: точка фиксации не пройдена, `NeedUpdate` при следующей регистрации повторит |
| SCREEN трижды не смог обновиться | остаётся на старой версии, исключён из показа (несовпадение версий), репорт в телеметрию через MAIN |
| MAIN откатился, часть SCREEN'ов уже обновилась | группа сводится к версии MAIN: обновившиеся SCREEN'ы даунгрейдятся (§9 п.4) |

Во всех сценариях отката устройство остаётся на согласованной тройке
«nanoCLR + managed-образ + wwwroot» одной версии, а группа сходится к версии MAIN.

## 13. План внедрения

**Фаза 1 — механика OTA**
- nf-interpreter: таблица `_ota`, boot-hook (`targetHAL_OtaApply.c`, вызов до
  `CLRStartupThread` — [CLR_Startup_Thread.c:11](targets/ESP32/_nanoCLR/CLR_Startup_Thread.c#L11)),
  `FixUpBlockRegionInfo` через `esp_ota_get_running_partition()`, rollback IDF,
  interop `LedTrees.Ota`, `CONFIG_NF_FEATURE_OTA` в Kconfig/defconfig,
  выбор CSV и адреса прошивки в [binutils.ESP32.cmake](CMake/binutils.ESP32.cmake).

  > Статус (2026-07-12): нативная часть реализована — [partitions_nanoclr_16mb_ota.csv](targets/ESP32/_IDF/esp32s3/partitions_nanoclr_16mb_ota.csv),
  > опция `NF_FEATURE_OTA` (вкл. во всех LEDTREES defconfig), rollback в sdkconfig,
  > ядро [targetHAL_Ota.c](targets/ESP32/_common/targetHAL_Ota.c) (state machine в NVS,
  > `NF_Ota_Firmware*/Stage*/CommitFull/Confirm`), boot-hook `NF_Ota_ApplyPending()` в
  > [app_main.c](targets/ESP32/_IDF/esp32s3/app_main.c), `Device_BlockStorage.c` через
  > `esp_ota_get_running_partition()`. Сборка ESP32_LEDTREES_V2 проходит; nanoCLR
  > занимает слот ota_0 на 88%. Прошивка: nanoCLR теперь по 0x20000 + `ota_data_initial.bin`
  > по 0xf000 (tasks.json/launch.json обновлены).
  >
  > Interop готов: класс `interoplib.Ota` (managed, в `nanoframework/main/interoplib`
  > ledtrees-esp32) + нативная реализация поверх `NF_Ota_*` в
  > [InteropAssemblies/interoplib](InteropAssemblies/interoplib) (чексумма `0x52D58C6F`;
  > заодно нативный interoplib синхронизирован с веткой `release` — методы
  > brightness/play — с сохранением правок под IDF 5.5.4). CRC32 — стандартный
  > zlib-совместимый (`esp_rom_crc32_le` ↔ `System.IO.Hashing.Crc32` ↔ `zlib.crc32`).
  > Упаковщик бандла: [scripts/pack-ltfw.py](scripts/pack-ltfw.py).
  >
  > Managed-сторона (ledtrees-esp32) тоже готова: проект `LedTrees.Device.Loader`
  > **удалён полностью** — точка входа (`Program.Main`) и `DeploymentTerminal`
  > перенесены в `LedTrees.Device.App` (OutputType Exe); Assembly.Load-механизм
  > и `OtaManager` удалены. `Bundle`
  > (версия = AssemblyVersion LedTrees.Device → 2.1.0.0, детерминированный `Id`
  > для группового протокола, `wwwroot-{ver}` на SD) + `BundleInstaller`
  > (скачивание в кэш `ota/bundle.ltfw` на SD → полное/лёгкое по clrSha256 →
  > `Ota.*` → reboot; подтверждение `Bundle.ConfirmIfPending()` в `Startup.Run`
  > после старта сервисов). Групповой протокол переведён на bundle: регистрация
  > несёт `Bundle.Id`, `NeedUpdate` → SCREEN качает кэшированный .ltfw с MAIN
  > (`DownloadService`), формат кадров не менялся. Локальная доставка —
  > `DeploymentTerminal`/`upload-update`/`DeviceConsole` — принимает .ltfw
  > (консоль сама пакует bundle из bin Loader'а + nanoCLR.bin + wwwroot).
  > Managed-образ deploy-региона = .pe подряд с выравниванием до 4 байт
  > ([CLRStartup.cpp:243](src/CLR/Startup/CLRStartup.cpp#L243)).
  >
  > Первый деплой на живое устройство (2026-07-12): erase → bootloader +
  > OTA-таблица + `ota_data_initial.bin` + nanoCLR@0x20000, managed-образ
  > (27 сборок, 322 KB) записан esptool'ом прямо в deploy@0x360000 — бутлоадер
  > видит OTA-таблицу, CLR стартует из ota_0, Wire Protocol отвечает,
  > приложение монтирует SD и поднимает AP. Попутно найден и исправлен
  > критический баг toolchain (отсутствие `-fno-builtin-*` для esp32s3/s2 —
  > GCC 14 писал в APB-регистры побайтово, ломая SDMMC; см.
  > [toolchain.xtensa-esp32s3-elf.cmake](CMake/toolchain.xtensa-esp32s3-elf.cmake)).
  > BLE исключён из LEDTREES defconfig.
  >
  > **Сквозная проверка на железе (2026-07-12): три OTA-цикла через
  > `POST /upload-update` — лёгкий (managed+web) и полный (со сменой A/B-слота) —
  > завершились состоянием CONFIRMED; wwwroot распакован на SD, админка работает
  > (WebSocket :8080, init получен, `otaSlot: 4 = Confirmed`).**
  > Попутные фиксы: DHCP-старт на AP перенесён из release
  > ([NF_ESP32_Wireless.cpp](targets/ESP32/_Network/NF_ESP32_Wireless.cpp)),
  > полифилл `crypto.randomUUID` во фронтенде (insecure context).
  >
  > Не сделано: подтвердить DHCP-сервер AP на клиентах (пока обход — статический
  > IP), `OtaUpdater` (опрос манифеста с сервера по HTTPS — вместе с сервером в
  > фазе 2), подпись манифеста, миграционная прошивка остальных устройств.
- ledtrees-esp32: слияние Loader+App (статическая линковка, `Startup.Run()` напрямую),
  `OtaUpdater`, вывод `OtaManager`/`Assembly.Load`-механизма из эксплуатации,
  `DeploymentTerminal` остаётся как recovery.
- Групповая раскатка: `BundleVersion` вместо Guid-хэша в регистрации, `NeedUpdate` +
  `DownloadService` переводятся с app-деплоя на `.ltfw` (+ подписанный манифест),
  кэш артефакта на SD у MAIN, планирование раскатки в простое (§9).
- CI: упаковщик `.ltfw`, генерация и подпись манифеста.
- Промежуточный вариант «удалённая доставка через существующий OtaManager» возможен,
  если OTA нужен раньше готовности нативной части, но в целевую архитектуру не входит.

**Фаза 2 — эксплуатация**
- Update-сервер (статика + генератор манифестов в CI), каналы, постепенная раскатка
  (процент устройств по хэшу serial), телеметрия версий и откатов по парку.

**Фаза 3 — усиление**
- Secure Boot V2, шифрование flash, дельта-обновления при необходимости.

## 14. Отклонённые альтернативы

**Раздельные единицы обновления: firmware bundle (nanoCLR+Loader) + app bundle
(`Assembly.Load` из littlefs)** — так работает сейчас. Отклонено, потому что .pe-сборки
завязаны на точные версии managed-библиотек и чексуммы нативных сборок: почти каждое
обновление nanoCLR ломает установленное приложение. Управление этим требует ABI-уровней,
сцепленных транзакций «прошивка+приложение» с общей точкой фиксации и подтверждения,
pending-слотов и пар `previous` в манифесте — класс сложности, который единый bundle
устраняет по построению. Дополнительные минусы раздельной схемы: `Assembly.Load` копирует
все сборки в RAM (вместо исполнения из flash), хрупкий `BinaryFormatter`-формат
`state.dat`, reflection-контракт `Startup.Run/Stop` как ещё одна ось совместимости.
Цена объединения — размер обновления (решено лёгким сценарием, ~1 MB при неизменном
nanoCLR) и невозможность обновить приложение без reboot (его не было и раньше:
`DeployApplication` завершается `Board.Reboot()`).

## 15. Открытые вопросы

- Реальный размер объединённого managed-образа: влезает ли Loader+App+библиотеки в
  2944 KB с запасом на рост; по результату — перекройка `deploy/stage/backup` ↔ littlefs.
- Нужен ли `backup` (2944 KB), или при откате достаточно перекачать старую версию с
  сервера? Backup спасает офлайн-устройства — пока оставляем.
- Канал push-уведомлений: MQTT уже есть в проде или только периодический опрос?
- Формат web-секции при большом wwwroot: хватит ли littlefs 4 MB на две версии, либо
  ввести общий контентно-адресуемый кэш файлов (по sha) вместо каталогов-версий.

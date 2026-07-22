//-----------------------------------------------------------------------------
//
// LEDTREES: вывод кадров на светодиодную ленту по SPI.
//
// Реализация пишется руками (в отличие от interoplib.cpp/.h и *_mshl.cpp — те
// генерируются сборкой managed-проекта и лежат готовыми в
// ledtrees-esp32/nanoframework/main/interoplib/Stubs/interoplib; их надо копировать
// оттуда, а не править здесь: таблица method_lookup индексируется порядковым номером
// метода в ассембли, и ручная вставка сдвигает индексы — вызовы уходят в чужие слоты).
//
// Два потока вывода:
//   LedTask  (core 1) — гонит кадры на ленту с фиксированным fps из play-буфера;
//   FeedTask (core 0) — читает следующую пачку кадров с SD в свободный буфер.
// Буферов воспроизведения два, по BUFFER_FRAMES_COUNT кадров: пока один играет,
// во второй читается продолжение программы.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_LedPixelController.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us — точная короткая latch-пауза WS2812
#include "interoplib_config.h"
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <task.h>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>

// Захват семафора не прячем внутрь assert: под NDEBUG он остаётся вызовом только
// потому, что ESP-IDF подменяет assert своим заголовком с
// CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE. Выставь опцию в n — и захваты молча
// исчезнут из RTM-прошивки. При portMAX_DELAY take не возвращает ошибку иначе как
// на NULL-семафоре, поэтому результат и не проверяем.
#define CS_BEGIN(s) ((void)xSemaphoreTake(s, portMAX_DELAY))
#define CS_END(s) ((void)xSemaphoreGive(s))

using namespace interoplib::interoplib;

static TaskHandle_t LedTask;
static spi_device_handle_t spi = NULL;
static spi_host_device_t SPI_HOST = SPI2_HOST;
static DMA_ATTR uint8_t FRAME_BUFFER[BUFF_SIZE];
static uint16_t PREPARED_FRAMES = 0;
static DMA_ATTR uint8_t* PREPARE_BUFFERS;    // кадры, подготовленные managed до StartPlay
static DMA_ATTR uint8_t* PROGRAM1_BUFFERS;   // 1-й буфер воспроизведения
static DMA_ATTR uint8_t* PROGRAM2_BUFFERS;   // 2-й буфер воспроизведения

// Промежуточный буфер во ВНУТРЕННЕЙ RAM для чтения подкормки: буферы
// воспроизведения (сотни КБ) живут в PSRAM, а SDMMC на S3 не умеет DMA в
// PSRAM — fread туда падает в посекторный путь (~2 мс на сектор), пачка
// кадров держала карту ~секунду и вдвое замедляла параллельные загрузки.
// Чтение во внутренний буфер + memcpy в PSRAM возвращает мультисекторный DMA.
//
// Буфер на FEED_BOUNCE_FRAMES кадров: кадры в файле лежат подряд, и один fread
// на несколько кадров размазывает накладные VFS/FATFS по пачке (50 вызовов
// на пачку -> ~7). Не хватило внутренней DMA-памяти — ёмкость уполовинивается
// вплоть до одного кадра (фактическая — в FEED_BOUNCE_CAP); NULL — чтение
// прямо в PSRAM прежним медленным путём.
#define FEED_BOUNCE_FRAMES 8
static uint8_t* FEED_BOUNCE;
static uint16_t FEED_BOUNCE_CAP = 0; // ёмкость FEED_BOUNCE, в кадрах
static int LEDS_COUNT = 0;
static int FRAME_SIZE = 0;

// Пер-нодовый ремап (docs/node-calibration.md): на каждый физический
// пиксель-триплет — индекс дизайн-триплета в кадре, либо REMAP_PAD (гасить в
// чёрный). NULL — идентичность (кадр как есть), дефолт до появления калибровки.
// Подмена — под bodySemaphore (NativeSetRemap), чтение — в emit_frame в той же
// секции, поэтому старую таблицу можно освободить сразу после подмены.
#define REMAP_PAD 0xFFFFu
static uint16_t* remapTable = NULL;

// Вывод кадра на ленту: ремап + яркость → FRAME_BUFFER → SPI (определение ниже).
static void emit_frame(const uint8_t *source);

// Общее у LedTask (core 1) и FeedTask (core 0), читается/пишется без блокировок —
// отсюда volatile: иначе оптимизатору ничто не мешает закешировать их в регистре
// на весь цикл вывода.
static volatile short BUFFERED_FRAMES = 0;       // кадров в готовой пачке; -1 — пачки нет
static volatile int32_t BUFFERED_FROM = -1;      // кадр программы, с которого пачка прочитана
static volatile uint8_t CURRENT_PLAY_BUFFER = 1; // какой из PROGRAMx_BUFFERS играет: 1 или 2

static volatile bool running = false;

// Текущий кадр программы от задачи вывода (реальная позиция ленты, в замирании
// подкормки не растёт); -1 — воспроизведение не идёт. Координаты — кадры самой
// программы, якорь вступления (SOURCE_START_FRAME) уже учтён, поэтому позиции
// узлов сравнимы между собой напрямую. Читается managed-кодом
// (NativeGetPlayPosition) для сводки синхронности узлов на MAIN.
static volatile int32_t CURRENT_PROGRAM_FRAME = -1;

// На сколько кадров группа впереди нас (＜0 — мы убежали вперёд). Источники:
// замирание подкормки, хвост просрочки за границей пачки и ре-якорь SyncPlay
// (NativeSyncPlayPosition, поток CLR). Гасится задачей вывода: пропуском из
// свежей пачки на свопе и плавной подтяжкой ±1 кадр за виток. Часть
// чтений-изменений идёт вне bodySemaphore (хвост цикла вывода) — гонка со
// записью из SyncPlay может потерять единицы кадров, это осознанно: значение
// корректирующее, следующий ре-якорь (раз в ~5 с) перепишет его свежей мерой.
static volatile int32_t BEHIND_FRAMES = 0;

static volatile bool requestedForStop = false;
static SemaphoreHandle_t bodySemaphore = NULL;
static SemaphoreHandle_t joinSemaphore = NULL;

struct LedTaskParams {
    uint8_t fps;
    uint16_t countFrames;
    uint16_t transition;
};
static LedTaskParams params;

static volatile uint8_t brightness = 255;

class Transition {
    public:
        uint8_t current;
        uint8_t length;
        uint8_t from[BUFF_SIZE];
        uint8_t to[BUFF_SIZE];

        bool getNextFrame(uint8_t* frameData) {
            if (current < length) {
                // По FRAME_SIZE, не по BUFF_SIZE: frameData — это lastRawFrame,
                // выделенный под фактический pixelCount; при меньшей ленте
                // BUFF_SIZE писал бы за границу кучи (from/to заполняются тоже
                // только на FRAME_SIZE).
                for (int i = 0; i < FRAME_SIZE; ++i) {
                    uint8_t a = (current +1)*0xFF / length;
                    frameData[i] = ((from[i]*(0xFF-a) + to[i]*a))/0xFF;
                }
                ++current;
                return true;
            } else {
                return false;
            }
        }
};

static uint8_t* lastRawFrame;
static Transition transitionFrame;

// Подкормка кадров: LedTask на каждом витке буфера уведомляет FeedTask, та читает
// следующую пачку с SD прямо в свободный play-буфер. Приоритет ниже LedTask —
// чтение никогда не вытесняет вывод кадра, а живёт на другом ядре, чтобы
// SD-латентность (120-240 мс на пачку) не задевала тайминг ленты.
#define FEED_CLOSE 0xFFFFFFFFu
#define FEED_TASK_PRIORITY (CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT - 1)

// Пока плеер замер в ожидании пачки — переспрашиваем раз в ~1 с (при 30 fps), иначе
// не приехавшая пачка (не открылся файл, оборвалось чтение) заморозила бы ленту
// навсегда. Не каждый кадр: пока задача читает, повтор лишь заставит её перечитать
// ту же пачку впустую.
#define STALL_RETRY_FRAMES 30

// Запрос пачки = поколение источника + индекс кадра в одном слове.
//
// Слот уведомления у задачи один, и запрос, поставленный LedTask старой программы,
// может быть подхвачен уже ПОСЛЕ смены источника. Сверки поколения внутри FeedFrames
// для этого мало: она снимает его на входе и увидит уже новое. Поколение, приехавшее
// в самом запросе, позволяет опознать такой запрос-призрак и выбросить.
#define FEED_REQ(gen, frame) ((((uint32_t)(gen) & 0x7FFFu) << 16) | ((uint32_t)(frame) & 0xFFFFu))
#define FEED_REQ_EPOCH(v) (((v) >> 16) & 0x7FFFu)
#define FEED_REQ_FRAME(v) ((v) & 0xFFFFu)

static TaskHandle_t FeedTask = NULL;
static SemaphoreHandle_t sourceSemaphore = NULL;  // защищает поля SOURCE_* при смене
static SemaphoreHandle_t feedMutex = NULL;        // удерживается на время чтения пачки
static char SOURCE_PATH[160] = {0};               // VFS-путь ("/D/..."), "" — источника нет
static uint32_t SOURCE_HEADER = 0;                // размер заголовка файла кэша
static uint16_t SOURCE_FRAME_SIZE = 0;
static uint16_t SOURCE_COUNT_FRAMES = 0;
static uint16_t SOURCE_START_FRAME = 0;           // якорь вступления из BeginPlay (кадр группы)
static volatile bool SOURCE_DIRTY = false;        // путь сменился — переоткрыть

// Растёт на каждый SetSource. Смена программы обязана отменить пачку, читающуюся под
// неё: свободный буфер уже отдан новой программе, и дописывать в него кадры старой
// нельзя — иначе первые ~50 кадров идут вперемешку.
static volatile uint32_t SOURCE_GENERATION = 0;

// Ошибки чтения подкормки с SD за время работы (не открылся файл, сорвался
// fseek/fread). Никогда не сбрасывается: спорадика плохого контакта должна
// оставаться видимой (managed включает её в отчёт регистрации), даже если
// прямо сейчас чтение идёт нормально. Читается NativeGetFeedReadErrors.
static volatile int32_t FEED_READ_ERRORS = 0;

// managed-путь ("D:\programs-cache\103.4.dat") в VFS-путь ("/D/programs-cache/103.4.dat"):
// SD монтируется как "/D" (targets/ESP32/_common/Target_System_IO_FileSystem.c — там
// буква диска ровно так же переписывается в точку монтирования).
static void ToVfsPath(const char* src, char* dst, size_t dstSize)
{
    size_t i = 0;
    if (src[0] != 0 && src[1] == ':') {
        dst[i++] = '/';
        dst[i++] = src[0];
        src += 2;
    }

    while (*src != 0 && i < dstSize - 1) {
        dst[i++] = (*src == '\\') ? '/' : *src;
        src++;
    }

    dst[i] = 0;
}

void LedPixelController::NativeInit( signed int mosiPin, signed int misoPin, signed int clkPin, signed int csPin, signed int pixelCount, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr  )
{
    if (spi != NULL || pixelCount > MAX_PIXELS) {
        hr = S_FALSE;
        return;
    }

    LEDS_COUNT = pixelCount;
    FRAME_SIZE = pixelCount * STRIPS_CNT * BYTES_PER_PIXEL;

    PREPARE_BUFFERS = new uint8_t[BUFFER_FRAMES_COUNT * FRAME_SIZE];
    PROGRAM1_BUFFERS = new uint8_t[BUFFER_FRAMES_COUNT * FRAME_SIZE];
    PROGRAM2_BUFFERS = new uint8_t[BUFFER_FRAMES_COUNT * FRAME_SIZE];

    memset(PREPARE_BUFFERS, 0, BUFFER_FRAMES_COUNT * FRAME_SIZE);
    memset(PROGRAM1_BUFFERS, 0, BUFFER_FRAMES_COUNT * FRAME_SIZE);
    memset(PROGRAM2_BUFFERS, 0, BUFFER_FRAMES_COUNT * FRAME_SIZE);

    // Обнулить обязательно: с этого кадра фейдится первый же переход
    // (StartPlay/SetFull), и мусор кучи дал бы случайные цвета на разгорании.
    lastRawFrame = new uint8_t[FRAME_SIZE];
    memset(lastRawFrame, 0, FRAME_SIZE);

    // NULL допустим (нет внутренней памяти) — подкормка тогда читает прямо в
    // PSRAM-буфер прежним медленным путём. +4 — под выравнивающий сдвиг pad.
    for (FEED_BOUNCE_CAP = FEED_BOUNCE_FRAMES; FEED_BOUNCE_CAP >= 1; FEED_BOUNCE_CAP /= 2) {
        FEED_BOUNCE = (uint8_t*)heap_caps_malloc(
            (size_t)FEED_BOUNCE_CAP * FRAME_SIZE + 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (FEED_BOUNCE != NULL)
            break;
    }
    if (FEED_BOUNCE == NULL)
        FEED_BOUNCE_CAP = 0;

    esp_err_t ret;

    spi_bus_config_t bus_cfg {
        mosi_io_num: 		mosiPin,
        miso_io_num: 		misoPin,
        sclk_io_num: 		clkPin,
        quadwp_io_num:  	-1,
        quadhd_io_num:  	-1,
        data4_io_num:       -1,
        data5_io_num:       -1,
        data6_io_num:       -1,
        data7_io_num:       -1,
        data_io_default_level: 0,
        max_transfer_sz:	FRAME_SIZE,
        flags:              0,
        isr_cpu_id:         ESP_INTR_CPU_AFFINITY_1,
        intr_flags:         0
    };

    ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO); // инициализируем и включаем DMA для spi
    ESP_ERROR_CHECK(ret);

    spi_device_interface_config_t dev_cfg {
        0,                   // Command bits
        0,                   // Address bits
        0,                   // Dummy bits
        0,                   // SPi Mode
        SPI_CLK_SRC_DEFAULT, // Clock source
        0,                   // Duty cycle 50/50
        0,                   // cs_ena_pretrans
        0,                   // cs_ena_posttrans
        SPI_LEDS_FREQ_HZ,         // Clock speed in Hz
        0,                   // Input_delay_ns
        SPI_SAMPLING_POINT_PHASE_0, // Sampling point (new in IDF 5.5)
        csPin,               // Chip select, we will use manual chip select
        0,                   // SPI_DEVICE flags
        7,                   // Queue size
        0,                   // Callback before
        0,                   // Callback after transaction complete
    };

    ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &spi);
    ESP_ERROR_CHECK(ret);

    vSemaphoreCreateBinary(bodySemaphore);
    vSemaphoreCreateBinary(joinSemaphore);
    sourceSemaphore = xSemaphoreCreateMutex();
    feedMutex = xSemaphoreCreateMutex();

    // задача подкормки: core 0 — LedTask живёт на core 1 и не должен делить ядро
    // с блокирующим SD-чтением; стек 4096 — под FATFS/VFS
    xTaskCreatePinnedToCore(
        FeedTask_Handler,
        "LedFeedTask",
        4096,
        NULL,
        FEED_TASK_PRIORITY,
        &FeedTask,
        0);

    LedPixelController::NativeSetFull(red, green, blue, 0, hr);
    if (hr != S_OK)
        return;

    hr = S_OK;
}

// Читает BUFFER_FRAMES_COUNT кадров начиная с fromFrame в НЕиграющий буфер.
//
// Любой отказ (не открылся файл, оборвалось чтение, сменился источник) — это просто
// выход без публикации пачки: BUFFERED_FRAMES остаётся -1, LedTask замирает на
// последнем кадре и через STALL_RETRY_FRAMES переспросит. Файл при отказе закрываем,
// чтобы повтор открыл его заново.
static void FeedFrames(FILE** file, uint32_t fromFrame)
{
    char path[sizeof(SOURCE_PATH)];
    uint32_t header, generation;
    uint16_t frameSize, countFrames;

    // Снимок под семафором: SetSource пишет эти поля из потока CLR.
    // Файл к этому моменту уже закрыт обработчиком, если источник менялся.
    CS_BEGIN(sourceSemaphore);
    generation = SOURCE_GENERATION;
    hal_strncpy_s(path, sizeof(path), SOURCE_PATH, sizeof(path) - 1); // strncpy запрещён nanoPAL
    header = SOURCE_HEADER;
    frameSize = SOURCE_FRAME_SIZE;
    countFrames = SOURCE_COUNT_FRAMES;
    CS_END(sourceSemaphore);

    if (path[0] == 0 || countFrames == 0 || frameSize == 0)
        return;

    if (*file == NULL) {
        *file = fopen(path, "rb");
        if (*file == NULL) {
            ESP_LOGE("interoplib", "feed: не открыть %s", path);
            FEED_READ_ERRORS = FEED_READ_ERRORS + 1;
            return;
        }
    }

    // fromFrame уже в кадрах программы: якорь вступления учтён задачей вывода
    // в стартовых координатах буфера, второй сдвиг здесь удвоил бы фазу.
    uint32_t frame = fromFrame % countFrames;
    long expectedPos = -1;

    // Пишем в буфер, который сейчас не играет.
    const uint8_t playBufferAtStart = CURRENT_PLAY_BUFFER;
    uint8_t* target = (playBufferAtStart == 1) ? PROGRAM2_BUFFERS : PROGRAM1_BUFFERS;

    uint16_t i = 0;
    while (i < BUFFER_FRAMES_COUNT) {
        // Источник сменили — свободный буфер уже отдан новой программе.
        if (SOURCE_GENERATION != generation)
            return;

        // Плеер свопнулся под нами — target стал играющим, писать в него нельзя.
        if (CURRENT_PLAY_BUFFER != playBufferAtStart)
            return;

        if (frame >= countFrames)
            frame = 0; // виток программы

        // Сколько кадров забрать одним fread: до конца пачки, но не через виток
        // (на витке разрыв последовательности в файле) и не больше ёмкости
        // bounce. Без bounce — по кадру: слоты в PSRAM идут с шагом FRAME_SIZE,
        // и слитное чтение при frameSize < FRAME_SIZE легло бы мимо слотов.
        uint16_t chunk = BUFFER_FRAMES_COUNT - i;
        if ((uint32_t)chunk > countFrames - frame)
            chunk = (uint16_t)(countFrames - frame);
        if (FEED_BOUNCE == NULL)
            chunk = 1;
        else if (chunk > FEED_BOUNCE_CAP)
            chunk = FEED_BOUNCE_CAP;

        long offset = (long)header + (long)frame * frameSize;
        // seek только на разрыве последовательности (старт пачки и виток) —
        // подряд идущие кадры читаются без лишних обращений к FATFS
        if (offset != expectedPos && fseek(*file, offset, SEEK_SET) != 0) {
            ESP_LOGE("interoplib", "feed: fseek %ld", offset);
            FEED_READ_ERRORS = FEED_READ_ERRORS + 1;
            fclose(*file);
            *file = NULL;
            return;
        }

        uint8_t* dst = target + i * FRAME_SIZE;
        // Через внутренний буфер, если он есть: чтение сразу в PSRAM-буфер
        // роняет SDMMC в посекторный режим (см. FEED_BOUNCE). Сдвиг offset&3
        // выравнивает указатель, которым FatFs после дозаполнения частичного
        // сектора читает целые секторы напрямую в наш буфер: offset кадра в
        // файле не кратен сектору, и без сдвига этот указатель оказывается
        // невыровненным — sdmmc снова падает в посекторный путь.
        uint8_t* readDst = (FEED_BOUNCE != NULL) ? (FEED_BOUNCE + (offset & 3)) : dst;
        size_t want = (size_t)chunk * frameSize;
        if (fread(readDst, 1, want, *file) != want) {
            ESP_LOGE("interoplib", "feed: кадр %u не дочитан", (unsigned)frame);
            FEED_READ_ERRORS = FEED_READ_ERRORS + 1;
            fclose(*file);
            *file = NULL;
            return;
        }

        // Раскладываем прочитанное по слотам буфера (шаг FRAME_SIZE). Хвост
        // слота гасим, если кадр в файле короче буфера устройства — иначе в
        // нём останутся байты предыдущей программы.
        for (uint16_t f = 0; f < chunk; f++) {
            uint8_t* slot = dst + (size_t)f * FRAME_SIZE;
            if (readDst != dst)
                memcpy(slot, readDst + (size_t)f * frameSize, frameSize);
            if (frameSize < FRAME_SIZE)
                memset(slot + frameSize, 0, FRAME_SIZE - frameSize);
        }

        expectedPos = offset + (long)want;
        frame += chunk;
        i += chunk;
    }

    // Публикуем пачку вместе с кадром, с которого она прочитана: одного «готово» мало,
    // потому что принести мы могли и пачку, которую плеер уже не ждёт (в слот
    // уведомления лёг повторный запрос). Сверив BUFFERED_FROM, он такую отбросит.
    if (SOURCE_GENERATION == generation && CURRENT_PLAY_BUFFER == playBufferAtStart) {
        BUFFERED_FROM = (int32_t)fromFrame;
        BUFFERED_FRAMES = BUFFER_FRAMES_COUNT;
    }
}

void FeedTask_Handler( void * pvParameters )
{
    (void)pvParameters;
    FILE* file = NULL;

    while (1) {
        uint32_t request = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &request, portMAX_DELAY) != pdTRUE)
            continue;

        // Источник менялся — отпускаем файл: пока он открыт, managed не сможет ни
        // удалить, ни переписать кэш (IO в nanoFramework эксклюзивен). Смотрим на
        // КАЖДОМ пробуждении, а не только по FEED_CLOSE: тот занимает единственный
        // слот уведомления и может быть затёрт запросом пачки от живого ещё LedTask.
        bool dirty;
        CS_BEGIN(sourceSemaphore);
        dirty = SOURCE_DIRTY;
        SOURCE_DIRTY = false;
        CS_END(sourceSemaphore);

        if (dirty && file != NULL) {
            fclose(file);
            file = NULL;
        }

        if (request == FEED_CLOSE)
            continue;

        // Запрос-призрак: программу сменили, а уведомление осталось в слоте.
        if (FEED_REQ_EPOCH(request) != (SOURCE_GENERATION & 0x7FFFu))
            continue;

        // feedMutex удерживается на всё чтение: LedTask_Start ждёт его перед стартом
        // нового таска, чтобы недочитанная пачка не легла в буфер новой программы.
        CS_BEGIN(feedMutex);
        FeedFrames(&file, FEED_REQ_FRAME(request));
        CS_END(feedMutex);
    }
}

void LedPixelController::NativeSetSource( CLR_RT_TypedArray_UINT8 path, unsigned int header, uint16_t frameSize, uint16_t countFrames, uint16_t startFrame, HRESULT &hr )
{
    if (FeedTask == NULL) {
        hr = S_FALSE;
        return;
    }

    char managedPath[sizeof(SOURCE_PATH)] = {0};
    int length = (int)path.GetSize();
    if (length >= (int)sizeof(managedPath)) {
        hr = CLR_E_INVALID_PARAMETER;
        return;
    }
    if (length > 0)
        memcpy(managedPath, (void*)path.GetBuffer(), length);

    // Кадр обязан помещаться в буфер устройства: FeedFrames кладёт кадры с шагом
    // FRAME_SIZE, и больший frameSize уехал бы за конец PROGRAMx_BUFFERS. Проверку
    // managed (IsValidFrameSize) тут не зачесть: она сверяет с константой, а не с
    // фактическим FRAME_SIZE = pixelCount*12.
    if (length > 0 && (frameSize == 0 || (int)frameSize > FRAME_SIZE)) {
        hr = CLR_E_INVALID_PARAMETER;
        return;
    }

    CS_BEGIN(sourceSemaphore);
    if (length == 0)
        SOURCE_PATH[0] = 0;
    else
        ToVfsPath(managedPath, SOURCE_PATH, sizeof(SOURCE_PATH));
    SOURCE_HEADER = header;
    SOURCE_FRAME_SIZE = frameSize;
    SOURCE_COUNT_FRAMES = countFrames;
    SOURCE_START_FRAME = startFrame;
    SOURCE_DIRTY = true;
    SOURCE_GENERATION++;   // читающаяся прямо сейчас пачка увидит это и бросит чтение
    CS_END(sourceSemaphore);

    // Будим задачу на ЛЮБУЮ смену источника, не только на снятие: иначе старый файл
    // висел бы открытым до следующей пачки, а у программы короче BUFFER_FRAMES_COUNT
    // пачек не бывает вовсе — и managed не смог бы перезаписать кэш.
    xTaskNotify(FeedTask, FEED_CLOSE, eSetValueWithOverwrite);

    hr = S_OK;
}

void LedPixelController::NativeSetBrightness( uint8_t value, HRESULT &hr )
{
    brightness = value;

    hr = S_OK;
}

signed int LedPixelController::NativeGetPlayPosition( HRESULT &hr )
{
    hr = S_OK;
    return CURRENT_PROGRAM_FRAME;
}

signed int LedPixelController::NativeGetFeedReadErrors( HRESULT &hr )
{
    hr = S_OK;
    return FEED_READ_ERRORS;
}

// Загрузить таблицу пер-нодового ремапа (docs/node-calibration.md). Ровно
// slots = FRAME_SIZE/3 записей ushort little-endian: индекс дизайн-триплета либо
// REMAP_PAD. Пустой массив — снять ремап (идентичность). Битую длину/индекс
// отвергаем, действующую таблицу не трогаем.
void LedPixelController::NativeSetRemap( CLR_RT_TypedArray_UINT8 param0, HRESULT &hr )
{
    hr = S_OK;

    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    const int slots = FRAME_SIZE / BYTES_PER_PIXEL; // физ. пиксель-триплеты (pixelCount*4)
    const int length = (int)param0.GetSize();

    // пустой массив — снять ремап
    if (length == 0) {
        CS_BEGIN(bodySemaphore);
        uint16_t* old = remapTable;
        remapTable = NULL;
        CS_END(bodySemaphore);
        if (old) heap_caps_free(old);
        return;
    }

    // ждём ровно slots записей по 2 байта; иначе таблица не под этот кадр — отказ
    if (length != slots * 2) {
        hr = S_FALSE;
        return;
    }

    uint16_t* table = (uint16_t*)heap_caps_malloc((size_t)slots * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (table == NULL) {
        hr = S_FALSE;
        return;
    }

    const uint8_t* src = (const uint8_t*)param0.GetBuffer();
    for (int i = 0; i < slots; i++) {
        uint16_t v = (uint16_t)(src[i * 2] | (src[i * 2 + 1] << 8)); // LE
        // индекс обязан быть в пределах кадра либо PAD — иначе таблица битая
        if (v != REMAP_PAD && v >= (uint16_t)slots) {
            heap_caps_free(table);
            hr = S_FALSE;
            return;
        }
        table[i] = v;
    }

    // атомарная подмена под bodySemaphore (вывод кадра идёт в той же секции):
    // старую освобождаем только когда её уже никто не читает
    CS_BEGIN(bodySemaphore);
    uint16_t* old = remapTable;
    remapTable = table;
    CS_END(bodySemaphore);
    if (old) heap_caps_free(old);
}

// Мёртвая зона подгонки фазы: сама сводка на MAIN точна до ±1–2 кадров
// (регистрация раз в секунду, UDP-джиттер, квантование тиков) — гоняться за
// расхождением такого масштаба значит дёргать фазу впустую.
#define SYNC_DEADBAND_FRAMES 2

// Ре-якорь SyncPlay: сверить свою позицию с кадром, который группа играет прямо
// сейчас, и при расхождении назначить коррекцию (BEHIND_FRAMES) — задача вывода
// рассосёт её плавной подтяжкой. Возвращает назначенную коррекцию в кадрах;
// 0 — в фазе либо очень близкая коррекция уже идёт.
signed int LedPixelController::NativeSyncPlayPosition( uint16_t groupFrame, HRESULT &hr )
{
    hr = S_OK;

    int32_t applied = 0;

    // Без bodySemaphore: задача вывода держит его почти весь кадр (SPI + латч
    // внутри секции), а в догоняющем режиме отпускает на микросекунды — ожидание
    // здесь морозило бы все managed-потоки на секунды (та же болезнь, из-за
    // которой LedTask_Stop переведён на флаг). Оба читаемых поля — volatile
    // 32-битные, чтение атомарно; params.countFrames пишется только в
    // LedTask_Start, а interop-вызовы не перекрываются. Гонка записи
    // BEHIND_FRAMES с хвостом цикла вывода осознанна и описана у поля.

    int32_t position = CURRENT_PROGRAM_FRAME;
    uint16_t countFrames = params.countFrames;

    if (running && position >= 0 && countFrames > 0) {
        // кольцевая разница (−count/2 .. +count/2]: >0 — группа впереди, догоняем
        int32_t diff = (int32_t)(groupFrame % countFrames) - position;
        int32_t half = countFrames / 2;
        if (diff > half)
            diff -= countFrames;
        else if (diff < -half)
            diff += countFrames;

        // Уже назначенную коррекцию не перетираем, пока свежая мера отличается от
        // неё в пределах мёртвой зоны: замирание копит BEHIND_FRAMES само, и якорь,
        // намеривший то же самое, не должен сбрасывать счёт из-за джиттера.
        int32_t pending = BEHIND_FRAMES;
        int32_t delta = diff - pending;
        if (delta > SYNC_DEADBAND_FRAMES || delta < -SYNC_DEADBAND_FRAMES) {
            BEHIND_FRAMES = diff;
            applied = diff;
        }
    }

    return applied;
}

void LedPixelController::NativePrepareForPlay( uint16_t frame, CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    if (frame >= BUFFER_FRAMES_COUNT) {
        hr = S_FALSE;
        return;
    }

    int offset = frame * FRAME_SIZE;
    uint8_t* frameData = (uint8_t*)data.GetBuffer();
    memcpy(PREPARE_BUFFERS + offset, frameData, FRAME_SIZE);

    if (frame == 0)
        PREPARED_FRAMES = 1; // сбрасываем счётчик подготовленных фреймов
    else
        PREPARED_FRAMES++;

    hr = S_OK;
}

void LedPixelController::NativeStartPlay( uint16_t countFrames, uint8_t fps, uint16_t transition, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    if (countFrames == 0 ||  fps < 1) {
        hr = S_FALSE;
        return;
    }

    // Короткой программе (вся в буфере, подкормки нет) нужен хотя бы один
    // подготовленный кадр: закольцовка в задаче берёт модуль по PREPARED_FRAMES,
    // и ноль там — деление на ноль и panic. Отказываем до остановки текущего
    // воспроизведения — нечем заменить, нечего и останавливать.
    if (countFrames <= BUFFER_FRAMES_COUNT && PREPARED_FRAMES == 0) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    LedTask_Start(countFrames, fps, transition);

    hr = S_OK;
}

void LedPixelController::NativeWriteToPlayBuffer( uint16_t frame, CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    uint8_t* buffer;
    if (CURRENT_PLAY_BUFFER == 1)
        buffer = PROGRAM2_BUFFERS;
    else
        buffer = PROGRAM1_BUFFERS;

    int offset = frame * FRAME_SIZE;
    uint8_t* frameData = (uint8_t*)data.GetBuffer();
    memcpy(buffer + offset, frameData, FRAME_SIZE);

    if (frame == 0)
        BUFFERED_FRAMES = 1; // сбрасываем счётчик буферизованных кадров воспроизведения
    else
        BUFFERED_FRAMES++;

    hr = S_OK;
}

void LedPixelController::NativeWrite( CLR_RT_TypedArray_UINT8 data, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    CS_BEGIN(bodySemaphore);

    // Шлём ровно столько, сколько дал managed (но не больше кадра): копирование
    // FRAME_SIZE безусловно читало за концом более коротких массивов.
    int length = (int)data.GetSize();
    if (length > FRAME_SIZE)
        length = FRAME_SIZE;

    memcpy(FRAME_BUFFER, (void*)data.GetBuffer(), length);

    spi_send_data(FRAME_BUFFER, length);

    CS_END(bodySemaphore);

    hr = S_OK;
}

// Заливка цветом с плавным переходом от текущего состояния ленты (transition мс;
// 0 — мгновенно). Блокирует вызвавший поток CLR на время перехода.
//
// Обновляет lastRawFrame, поэтому следующий StartPlay фейдится из реального
// состояния ленты: после гашения — разгорание из чёрного, а не скачок от кадра
// давно остановленной программы.
void LedPixelController::NativeSetFull( uint8_t red, uint8_t green, uint8_t blue, uint16_t transition, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    CS_BEGIN(bodySemaphore);

    const uint8_t target[3] = { red, green, blue };

    // Циклы идут по FRAME_SIZE, не по BUFF_SIZE: lastRawFrame выделен под фактический
    // pixelCount, и при меньшей ленте BUFF_SIZE вышел бы за границу кучи.
    int steps = transition / (1000 / PROGRAM_TRANSITION_FPS);
    for (int s = 1; s <= steps; ++s) {
        uint8_t a = s * 0xFF / steps;
        for (int i = 0; i < FRAME_SIZE; ++i) {
            uint8_t raw = (lastRawFrame[i] * (0xFF - a) + target[i % 3] * a) / 0xFF;
            FRAME_BUFFER[i] = raw * brightness / 0xFF;
        }
        spi_send_data(FRAME_BUFFER, FRAME_SIZE);
        vTaskDelay(pdMS_TO_TICKS(1000 / PROGRAM_TRANSITION_FPS));
    }

    // финальный кадр точным цветом + фиксация состояния ленты
    for (int i = 0; i < FRAME_SIZE; ++i) {
        lastRawFrame[i] = target[i % 3];
        FRAME_BUFFER[i] = target[i % 3] * brightness / 0xFF;
    }

    spi_send_data(FRAME_BUFFER, FRAME_SIZE);

    CS_END(bodySemaphore);

    hr = S_OK;
}

void LedPixelController::NativeSetPixel( uint8_t line, uint16_t cell, uint8_t red, uint8_t green, uint8_t blue, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    // cell — группа пикселя (12 байт), line — лента 0..3. Мимо этих границ индекс
    // уезжает за FRAME_SIZE, а lastRawFrame ровно такого размера в куче.
    if (line > 3 || (int)cell >= LEDS_COUNT) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    CS_BEGIN(bodySemaphore);

    int i = (cell * 3 * 4) + (line * 3);
    FRAME_BUFFER[i + 0] = red;
    FRAME_BUFFER[i + 1] = green;
    FRAME_BUFFER[i + 2] = blue;

    // Состояние ленты для переходов — из него фейдятся StartPlay и SetFull.
    lastRawFrame[i + 0] = red;
    lastRawFrame[i + 1] = green;
    lastRawFrame[i + 2] = blue;

    spi_send_data(FRAME_BUFFER, FRAME_SIZE);

    CS_END(bodySemaphore);

    hr = S_OK;
}

void LedTask_Start(uint16_t countFrames, uint8_t fps, uint16_t transition)
{
    // Ждём, пока подкормка отпустит буферы: пачка предыдущей программы может
    // дочитываться прямо сейчас, а новый таск сразу зальёт свободный буфер из
    // PREPARE_BUFFERS. Поколение источника уже сменил SetSource, так что пачка бросит
    // чтение на ближайшем кадре — ожидание в единицы мс.
    if (feedMutex != NULL) {
        CS_BEGIN(feedMutex);
        CS_END(feedMutex);
    }

    CS_BEGIN(bodySemaphore);

    if (!running) {
        params.fps = fps;
        params.countFrames = countFrames;
        params.transition = transition;

        // Флаги ставим ЗДЕСЬ, до создания задачи, а не внутри неё. Раньше
        // running=true выставляла сама задача после старта — между созданием и
        // первым её витком гейт «if (!running)» был дырявым: второй StartPlay,
        // пришедший в это окно, создавал ВТОРУЮ задачу-зомби. Две задачи вечно
        // пинг-понгуют bodySemaphore на одном ядре, а requestedForStop гасит
        // только одну из них. Заодно чистим флаг остановки: он мог остаться
        // взведённым, если прошлый Stop сработал по уже вышедшей задаче.
        running = true;
        requestedForStop = false;
        BEHIND_FRAMES = 0; // коррекции прошлой программы новой не принадлежат

        // Протокол join: пока задача жива, токена в joinSemaphore нет — его отдаёт
        // только выходящая задача. Дежурный токен съедаем ДО создания, иначе give
        // на выходе пришёлся бы на полный семафор, потерялся, и Join завис бы навсегда.
        (void)xSemaphoreTake(joinSemaphore, 0);

        // core 1: вывод кадров не должен делить ядро с подкормкой и её SD-чтением
        if (xTaskCreatePinnedToCore(
            LedTask_Handler,
            "LedTask",
            4096,
            (void*) &params,
            CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT,
            &LedTask,
            1) != pdPASS) {
            // Задачи нет — некому ни выйти, ни отдать токен: откатываем оба,
            // иначе следующий Stop/Join повиснет на portMAX_DELAY вместе со
            // всеми managed-потоками.
            running = false;
            (void)xSemaphoreGive(joinSemaphore);
        }
    }

    CS_END(bodySemaphore);
}

// Сигнал остановки БЕЗ захвата bodySemaphore. Захват здесь голодал секундами:
// задача вывода держит семафор почти весь кадр (SPI-транзакция + vTaskDelay
// внутри секции), а после любого отставания vTaskDelayUntil гонит витки
// вплотную (догоняющий режим) — семафор перехватывался на своём же ядре
// быстрее, чем просыпался ожидающий CLR на другом. Флаг volatile, задача
// читает его на каждом витке — семафор для сигнала не нужен. joinSemaphore
// здесь тоже не трогаем: его токен отдаёт только выходящая задача (дежурный
// съеден в LedTask_Start), а забор здесь гонялся бы с этим give — задача,
// увидевшая флаг между двумя строками, отдала бы токен в полный семафор,
// give потерялся бы, и Join завис навсегда. Конкурентных вызовов со стороны
// managed не бывает: nanoCLR исполняет все managed-потоки на одной нативной
// задаче, и interop-вызовы не перекрываются.
void LedTask_Stop()
{
    if (running) {
        requestedForStop = true;
    }
}

void LedTask_Join()
{
    (void)xSemaphoreTake(joinSemaphore, portMAX_DELAY);
    (void)xSemaphoreGive(joinSemaphore);
}

void LedTask_Handler( void * pvParameters )
{
    LedTaskParams* taskParams = (LedTaskParams*) pvParameters;

    // Кадр вступления (якорь BeginPlay) и признак заякоренного входа длинной
    // программы. PREPARE-буфер длинной готовился managed'ом ДО прихода якоря, с
    // кадра 0 — играть его значит ~2 с показывать чужой кусок программы. Вместо
    // этого сразу заказываем пачку с якорного кадра и замираем до её прихода
    // (обычно 150–250 мс чтения SD): замирание копит BEHIND_FRAMES, навёрстывание
    // на свопе съедает их — лента вступает точно в фазу группы. Обычный групповой
    // старт (якорь 0) играет PREPARE сразу, как раньше.
    const uint16_t entryStartFrame = taskParams->countFrames > 0
        ? (uint16_t)(SOURCE_START_FRAME % taskParams->countFrames)
        : (uint16_t)0;
    const bool anchoredLongEntry = taskParams->countFrames > BUFFER_FRAMES_COUNT
        && entryStartFrame != 0
        && FeedTask != NULL;

    // Фейд заякоренного входа откладывается до первой пачки: его цель — первый
    // реальный кадр — до неё неизвестна (см. блок отложенного фейда в цикле).
    bool pendingEntryTransition = false;

    uint16_t transitionTime = taskParams->transition;
    if (1000 / taskParams->fps < transitionTime) {
        transitionFrame.length = transitionTime / (1000 / PROGRAM_TRANSITION_FPS);
        transitionFrame.length += 1; // for first frame of next program
        transitionFrame.current = 0;

        if (anchoredLongEntry) {
            pendingEntryTransition = true;
        } else {
            // Цель фейда — кадр, который реально заиграет первым: у короткой
            // программы (вся в буфере) это кадр вступления, не кадр 0; у длинной
            // без якоря — начало PREPARE-буфера.
            size_t firstFrameOffset = 0;
            if (taskParams->countFrames > 0 && taskParams->countFrames <= BUFFER_FRAMES_COUNT)
                firstFrameOffset = (size_t)entryStartFrame * FRAME_SIZE;

            memcpy(transitionFrame.to, PREPARE_BUFFERS + firstFrameOffset, FRAME_SIZE);
            memcpy(transitionFrame.from, lastRawFrame, FRAME_SIZE);
        }
    } else
        transitionFrame.length = 0;

    CS_BEGIN(bodySemaphore);
    // running=true уже выставил LedTask_Start (до создания задачи — иначе гейт
    // «if (!running)» дыряв и второй StartPlay плодил задачу-зомби)
    //
    // Темп кадров. Период 1000/fps редко кратен тику: 24 fps при тике 10 мс — это
    // 41.67 мс, а усечённый до целых тиков период (40 мс) гнал бы ленту на +4%
    // быстрее номинала. Между собой узлы от этого не расходятся (усечение у всех
    // одно), но якорь кадра MAIN считает по номинальному fps — и промахивался бы
    // тем сильнее, чем дольше играет программа к моменту вступления узла. Целые
    // тики отдаём xTaskDelayUntil, дробный остаток добирает аккумулятор Брезенхэма:
    // часть периодов на тик длиннее, средний темп — точно fps.
    // fps выше частоты тиков не воспроизвести — зажимаем, иначе периоды в 0 тиков
    // (для xTaskDelayUntil это assert).
    const TickType_t ticksPerSecond = configTICK_RATE_HZ;
    const uint16_t pacingFps = taskParams->fps < ticksPerSecond ? taskParams->fps : (uint16_t)ticksPerSecond;
    const TickType_t programBaseTicks = ticksPerSecond / pacingFps;
    const uint16_t programTicksRemainder = (uint16_t)(ticksPerSecond % pacingFps);
    const TickType_t transitionTimeIncrement = 1000.0 / PROGRAM_TRANSITION_FPS / portTICK_PERIOD_MS;
    CS_END(bodySemaphore);

    uint16_t programTicksAcc = 0; // доли тика: числитель, знаменатель pacingFps

	TickType_t lastWakeTime = xTaskGetTickCount();

    // выбираем буфер воспроизведения, куда скопируем буфер подготовки
    uint8_t* buffer;
    if (CURRENT_PLAY_BUFFER == 1) {
        buffer = PROGRAM2_BUFFERS;
        CURRENT_PLAY_BUFFER = 2;
    }
    else {
        buffer = PROGRAM1_BUFFERS;
        CURRENT_PLAY_BUFFER = 1;
    }

    // копируем буфер подготовки в буфер воспроизведения
    memcpy(buffer, PREPARE_BUFFERS, BUFFER_FRAMES_COUNT * FRAME_SIZE);
    uint16_t bufferFramesCount = PREPARED_FRAMES;

    // Поколение источника, под которое этот таск запущен: SetSource всегда идёт
    // перед StartPlay. Уходит в каждый запрос подкормки, чтобы задача могла
    // опознать запрос от уже смещённой программы.
    const uint32_t myGeneration = SOURCE_GENERATION;

    // Кадр программы, лежащий в начале текущего буфера, и кадр, с которого заказана
    // следующая пачка. Сквозной счётчик выведенных кадров тут не годится: в замирании
    // лента стоит, а он бы убегал — и пачка заказывалась бы не с того места.
    //
    // Счёт — в кадрах ПРОГРАММЫ, от якоря вступления (BeginPlay мог включить нас в
    // уже играющую группу): длинной программе якорь задаёт старт координат пачек,
    // короткой (вся в буфере, подкормки нет) — стартовый индекс в кольце буфера.
    // Так CURRENT_PROGRAM_FRAME честен для обоих путей, а подкормка получает запросы
    // сразу в кадрах программы. Оговорка: первый буфер длинной программы managed
    // готовил до прихода якоря, с кадра 0, — до первого свопа лента играет начало
    // программы, хотя позиция уже рапортуется от якоря. SOURCE_START_FRAME здесь
    // читается без семафора: SetSource строго предшествует StartPlay.
    uint32_t bufferStartFrame = 0;
    uint32_t nextBufferFrame = 0;

    uint16_t bufferFrameIndex = 0;
    if (taskParams->countFrames <= BUFFER_FRAMES_COUNT)
        bufferFrameIndex = entryStartFrame;
    else
        bufferStartFrame = entryStartFrame;

    uint16_t stallTicks = 0;

    // Пачка для текущего буфера уже заказана. Раньше заказ висел на условии
    // «index == 0», и оно срабатывало один раз на буфер — advance был всегда ≥1.
    // Придержка кадра (slew −1) теперь может держать индекс на нуле много витков,
    // и без флага каждый из них заново сбрасывал бы BUFFERED_FRAMES (выбрасывая
    // уже приехавшую пачку) и гонял FeedTask перечитывать её с SD.
    bool feedRequested = false;

    // Заякоренный вход: PREPARE-буфер объявляем доигранным и сразу заказываем
    // пачку с якоря — до её прихода цикл штатно замирает (см. комментарий выше).
    if (anchoredLongEntry) {
        BUFFERED_FRAMES = -1;
        bufferFrameIndex = bufferFramesCount;
        nextBufferFrame = bufferStartFrame;
        feedRequested = true;
        xTaskNotify(FeedTask, FEED_REQ(myGeneration, nextBufferFrame), eSetValueWithOverwrite);
    }
    while (1) {
        CS_BEGIN(bodySemaphore);

        bool stalled = false;
        if (bufferFrameIndex >= bufferFramesCount) {
            if (taskParams->countFrames <= BUFFER_FRAMES_COUNT) {
                // Программа целиком уместилась в буфер — подкормки для неё нет и не
                // будет, виток это просто её закольцовка. Модуль, а не сброс в 0:
                // пропуск кадров при просрочке периода мог перешагнуть границу
                // кольца, и обнуление съедало бы фазу перескока.
                bufferFrameIndex = (uint16_t)(bufferFrameIndex % bufferFramesCount);
            }
            else if (BUFFERED_FRAMES != -1 && BUFFERED_FROM == (int32_t)nextBufferFrame) {
                // Свопаем только на ту пачку, которую ждём: прийти могла и лишняя —
                // повторный запрос из слота заставляет задачу перечитать пачку.
                //
                // Хвост просрочки за границей пачки (index > count) — в навёрстывание:
                // обнуление индекса иначе съедало бы фазу перескока, как раньше.
                BEHIND_FRAMES = BEHIND_FRAMES + (int32_t)(bufferFrameIndex - bufferFramesCount);
                bufferFrameIndex = 0;
                stallTicks = 0;

                if (CURRENT_PLAY_BUFFER == 1) {
                    buffer = PROGRAM2_BUFFERS;
                    CURRENT_PLAY_BUFFER = 2;
                }
                else {
                    buffer = PROGRAM1_BUFFERS;
                    CURRENT_PLAY_BUFFER = 1;
                }

                bufferFramesCount = BUFFERED_FRAMES;
                bufferStartFrame = nextBufferFrame;
                feedRequested = false; // новому буферу — свой заказ продолжения
            }
            else {
                // Пачка не приехала — замираем на последнем кадре, как буферизация
                // видео на медленном канале. Играть буфер заново нельзя: тогда запрос
                // уходил бы на каждом витке, копился в слоте, и задача читала бы пачку
                // дважды — вторым чтением уже в играющий буфер.
                stalled = true;
            }
        }

        // Начало буфера: первый кадр таска или первый кадр после свопа. Ровно
        // один раз на буфер (feedRequested) — индекс может гостить на нуле и
        // дольше витка, см. придержку кадра.
        if (!stalled && bufferFrameIndex == 0 && !feedRequested) {
            feedRequested = true;
            BUFFERED_FRAMES = -1; // -1 = следующая пачка ещё не приехала

            if (taskParams->countFrames > BUFFER_FRAMES_COUNT && FeedTask != NULL) {
                nextBufferFrame = bufferStartFrame + bufferFramesCount;
                if (nextBufferFrame >= taskParams->countFrames)
                    nextBufferFrame -= taskParams->countFrames;

                xTaskNotify(FeedTask, FEED_REQ(myGeneration, nextBufferFrame), eSetValueWithOverwrite);
            }
        }
        else if (stalled && ++stallTicks >= STALL_RETRY_FRAMES) {
            // Замерли надолго — пачка не едет вовсе. Переспрашиваем; дубликат
            // безопасен: задача бросит чтение, если плеер свопнется под ней, а лишнюю
            // пачку отсеет сверка BUFFERED_FROM.
            stallTicks = 0;
            if (taskParams->countFrames > BUFFER_FRAMES_COUNT && FeedTask != NULL)
                xTaskNotify(FeedTask, FEED_REQ(myGeneration, nextBufferFrame), eSetValueWithOverwrite);
        }

        // Навёрстывание после замирания: кадры, пролежавшие в ожидании пачки, группа
        // уже отыграла — пропускаем их из свежей пачки и возвращаемся в фазу, а не
        // тащим отставание до конца программы. Дальше конца пачки не прыгаем (её
        // продолжение только что заказано выше); если замирание было дольше пачки,
        // излишек переносится и догорает на следующих свопах.
        if (!stalled && bufferFrameIndex == 0 && BEHIND_FRAMES > 0) {
            uint16_t skip = (uint32_t)BEHIND_FRAMES < bufferFramesCount
                ? (uint16_t)BEHIND_FRAMES
                : (uint16_t)(bufferFramesCount - 1);
            bufferFrameIndex = skip;
            BEHIND_FRAMES = BEHIND_FRAMES - skip;
        }

        if (buffer == NULL)
            break;

        // Отложенный фейд заякоренного входа: цель — первый реальный кадр
        // приехавшей пачки (уже с учётом навёрстывания выше); from — то, что
        // лента показывала в ожидании (последний кадр прежней программы/чернота).
        if (pendingEntryTransition && !stalled) {
            pendingEntryTransition = false;
            memcpy(transitionFrame.to, buffer + bufferFrameIndex * FRAME_SIZE, FRAME_SIZE);
            memcpy(transitionFrame.from, lastRawFrame, FRAME_SIZE);
        }

        // передаём кадр на ленту
        int offset = bufferFrameIndex * FRAME_SIZE;

        // позиция для сводки синхронности: в замирании не обновляется —
        // лента реально стоит, и наблюдатель должен видеть именно это
        if (!stalled)
            CURRENT_PROGRAM_FRAME = (int32_t)((bufferStartFrame + bufferFrameIndex) % taskParams->countFrames);

        bool programPace = true;
        if (stalled) {
            // lastRawFrame не трогаем — на ленту снова уходит последний кадр. Именно
            // шлём, а не молчим: в замирании должен работать SetBrightness.
        }
        else if (transitionFrame.getNextFrame(lastRawFrame)) {
            programPace = false; // переход живёт своим темпом (PROGRAM_TRANSITION_FPS)
        }
        else {
            memcpy(lastRawFrame, buffer + offset, FRAME_SIZE);
        }

        TickType_t xTimeIncrement;
        if (programPace) {
            xTimeIncrement = programBaseTicks;
            programTicksAcc += programTicksRemainder;
            if (programTicksAcc >= pacingFps) {
                programTicksAcc -= pacingFps;
                xTimeIncrement++;
            }
        }
        else
            xTimeIncrement = transitionTimeIncrement;

        // Плавная подтяжка фазы к группе (ре-якорь SyncPlay и остатки замираний):
        // отстаём — шагаем по два кадра за период, убежали вперёд — придерживаем
        // кадр. ±1 кадр за виток глазу не виден, отставание в секунду
        // рассасывается за секунду. Не в замирании (позиция и так стоит) и не в
        // переходе (там чужой темп, а фаза программы ещё не видна на ленте).
        int slewStep = 0;
        if (!stalled && programPace) {
            if (BEHIND_FRAMES > 0) {
                slewStep = 1;
                BEHIND_FRAMES = BEHIND_FRAMES - 1;
            }
            else if (BEHIND_FRAMES < 0) {
                slewStep = -1;
                BEHIND_FRAMES = BEHIND_FRAMES + 1;
            }
        }

        // Вывод через emit_frame: ремап (дизайн→факт калибровки) + яркость.
        // lastRawFrame — дизайн-кадр; без калибровки ремап = идентичность и это
        // ровно прежний проход по FRAME_SIZE с яркостью.
        emit_frame(lastRawFrame);

        bool exit = requestedForStop;
        requestedForStop = false;

        CS_END(bodySemaphore);

        if (exit) break;

        // Просрочка периода: график подтягиваем ЦЕЛЫМ числом периодов и ровно
        // столько же кадров пропускаем — сетка времени (и темп fps) сохраняется
        // точной, узлы группы не расползаются. Две прежние крайности не годятся:
        // догоняющий бурст оригинального vTaskDelayUntil держал темп, но гнал
        // витки вплотную, почти не отпуская bodySemaphore (Stop голодал
        // секундами); полный сброс lastWakeTime лечил голодание, но терял
        // остаток периода на каждой просрочке — узлы с разной нагрузкой играли
        // с разным фактическим fps и дрейфовали на миллисекунды в секунду.
        TickType_t skippedPeriods = 0;
        if (xTaskDelayUntil(&lastWakeTime, xTimeIncrement) == pdFALSE) {
            const TickType_t now = xTaskGetTickCount();
            if (programPace) {
                // Каждый пропущенный период проводим через тот же Bresenham-
                // аккумулятор, что и обычный виток: заряжать все пропуски одним
                // текущим xTimeIncrement значит терять дробную часть периода на
                // каждом (до ~10% кадра) — темп уезжал бы от сетки группы.
                // Инкремент коммитим только для реально пропущенного периода,
                // недопропущенный шаг остаётся следующему витку.
                while (1) {
                    TickType_t step = programBaseTicks;
                    uint16_t acc = programTicksAcc + programTicksRemainder;
                    if (acc >= pacingFps) {
                        acc -= pacingFps;
                        step++;
                    }
                    if ((TickType_t)(now - lastWakeTime) < step)
                        break;
                    lastWakeTime += step;
                    programTicksAcc = acc;
                    skippedPeriods++;
                }
            }
            else {
                // Переход живёт своим целочисленным темпом — дробить нечего.
                const TickType_t step = xTimeIncrement > 0 ? xTimeIncrement : 1;
                skippedPeriods = (now - lastWakeTime) / step;
                lastWakeTime += skippedPeriods * step;
            }
        }

        // в замирании индекс не двигаем: буфер доигран, ждём пачку — но время
        // группы идёт, и потерянные кадры копим для навёрстывания после свопа
        if (!stalled)
            bufferFrameIndex = (uint16_t)(bufferFrameIndex + 1 + skippedPeriods + slewStep);
        else
            BEHIND_FRAMES = BEHIND_FRAMES + (int32_t)(1 + skippedPeriods);
    }

    CS_BEGIN(bodySemaphore);
    running = false;
    CS_END(bodySemaphore);

    CURRENT_PROGRAM_FRAME = -1;

    (void)xSemaphoreGive(joinSemaphore);
    vTaskDelete(NULL);
}

void spi_send_data(const uint8_t *data, int len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;

    // Interrupt-транзакция, не polling: кадр 4800 байт на 2.5 МГц идёт ~15 мс, и
    // polling_transmit прожигал бы их busy-wait'ом (~46% ядра 1 при 30 fps),
    // отбирая ядро у непривязанных задач (lwIP tcpip плавает между ядрами).
    // Здесь задача спит до прерывания о завершении.
    spi_device_transmit(spi, &t);

    // Кадр укладывается в одну транзакцию: max_transfer_sz шины задан в FRAME_SIZE
    // (NativeInit), а больше кадра сюда и не приходит.

    // НЕ убирать: latch-пауза WS2812 — лента фиксирует кадр по тишине на линии
    // данных, без паузы кадры сливаются в один поток. Ленте нужно ~280 мкс; даём
    // 300 мкс busy-wait'ом, а НЕ vTaskDelay(1). Прежний тик (до 10 мс) покрывал
    // паузу с огромным запасом, но съедал ~9,7 мс из бюджета кадра: вместе с
    // передачей (~15 мс) на полной ленте это упирало реальный потолок в ~40 fps.
    // Короткий busy-wait оставляет упором только передачу (~15 мс) → ~60 fps на
    // 400 px, с запасом при меньшем числе пикселей. Голодания задач нет: сама
    // передача идёт через прерывание (задача спит ~15 мс) — yield на кадр есть и
    // без тика; 300 мкс под bodySemaphore пренебрежимы рядом с 15 мс транзакции.
    esp_rom_delay_us(300);
}

// Единственная точка вывода кадра на ленту: пер-нодовый ремап (дизайн→факт) +
// яркость → FRAME_BUFFER → SPI. source — дизайн-кадр (lastRawFrame при
// воспроизведении). remapTable == NULL — идентичность, т.е. прежний проход по
// FRAME_SIZE с яркостью байт-в-байт. Вызывать под bodySemaphore (как главный
// цикл), чтобы подмена таблицы в NativeSetRemap не пересекалась с чтением.
static void emit_frame(const uint8_t *source)
{
    uint16_t* remap = remapTable;
    if (remap == NULL) {
        for (int i = 0; i < FRAME_SIZE; i++)
            FRAME_BUFFER[i] = source[i] * brightness / 0xFF;
    }
    else {
        const int slots = FRAME_SIZE / BYTES_PER_PIXEL;
        for (int slot = 0; slot < slots; slot++) {
            uint16_t s = remap[slot];
            int d = slot * 3;
            if (s == REMAP_PAD) {
                FRAME_BUFFER[d] = FRAME_BUFFER[d + 1] = FRAME_BUFFER[d + 2] = 0;
            }
            else {
                int b = s * 3;
                FRAME_BUFFER[d]     = source[b]     * brightness / 0xFF;
                FRAME_BUFFER[d + 1] = source[b + 1] * brightness / 0xFF;
                FRAME_BUFFER[d + 2] = source[b + 2] * brightness / 0xFF;
            }
        }
    }

    spi_send_data(FRAME_BUFFER, FRAME_SIZE);
}

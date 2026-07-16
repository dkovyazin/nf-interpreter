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

// Общее у LedTask (core 1) и FeedTask (core 0), читается/пишется без блокировок —
// отсюда volatile: иначе оптимизатору ничто не мешает закешировать их в регистре
// на весь цикл вывода.
static volatile short BUFFERED_FRAMES = 0;       // кадров в готовой пачке; -1 — пачки нет
static volatile int32_t BUFFERED_FROM = -1;      // кадр программы, с которого пачка прочитана
static volatile uint8_t CURRENT_PLAY_BUFFER = 1; // какой из PROGRAMx_BUFFERS играет: 1 или 2

static volatile bool running = false;
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
                for (int i = 0; i < BUFF_SIZE; ++i) {
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
static uint16_t SOURCE_START_FRAME = 0;           // сдвиг фазы устройства в сборке
static volatile bool SOURCE_DIRTY = false;        // путь сменился — переоткрыть

// Растёт на каждый SetSource. Смена программы обязана отменить пачку, читающуюся под
// неё: свободный буфер уже отдан новой программе, и дописывать в него кадры старой
// нельзя — иначе первые ~50 кадров идут вперемешку.
static volatile uint32_t SOURCE_GENERATION = 0;

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
    if (spi != NULL || pixelCount > 400) {
        hr = S_FALSE;
        return;
    }

    LEDS_COUNT = pixelCount;
    FRAME_SIZE = pixelCount * 4 * 3;

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
    uint16_t frameSize, countFrames, startFrame;

    // Снимок под семафором: SetSource пишет эти поля из потока CLR.
    // Файл к этому моменту уже закрыт обработчиком, если источник менялся.
    CS_BEGIN(sourceSemaphore);
    generation = SOURCE_GENERATION;
    hal_strncpy_s(path, sizeof(path), SOURCE_PATH, sizeof(path) - 1); // strncpy запрещён nanoPAL
    header = SOURCE_HEADER;
    frameSize = SOURCE_FRAME_SIZE;
    countFrames = SOURCE_COUNT_FRAMES;
    startFrame = SOURCE_START_FRAME;
    CS_END(sourceSemaphore);

    if (path[0] == 0 || countFrames == 0 || frameSize == 0)
        return;

    if (*file == NULL) {
        *file = fopen(path, "rb");
        if (*file == NULL) {
            ESP_LOGE("interoplib", "feed: не открыть %s", path);
            return;
        }
    }

    // Сдвиг на startFrame — фаза устройства в сборке: программа закольцована, поэтому
    // подкормка со сдвигом тождественна старту с этого кадра.
    uint32_t frame = (fromFrame + startFrame) % countFrames;
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

        // core 1: вывод кадров не должен делить ядро с подкормкой и её SD-чтением
        xTaskCreatePinnedToCore(
            LedTask_Handler,
            "LedTask",
            4096,
            (void*) &params,
            CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT,
            &LedTask,
            1);
    }

    CS_END(bodySemaphore);
}

void LedTask_Stop()
{
    CS_BEGIN(bodySemaphore);

    if (running) {
        requestedForStop = true;
        (void)xSemaphoreTake(joinSemaphore, portMAX_DELAY);
    }

    CS_END(bodySemaphore);
}

void LedTask_Join()
{
    (void)xSemaphoreTake(joinSemaphore, portMAX_DELAY);
    (void)xSemaphoreGive(joinSemaphore);
}

void LedTask_Handler( void * pvParameters )
{
    LedTaskParams* taskParams = (LedTaskParams*) pvParameters;

    uint16_t transitionTime = taskParams->transition;
    if (1000 / taskParams->fps < transitionTime) {
        transitionFrame.length = transitionTime / (1000 / PROGRAM_TRANSITION_FPS);
        transitionFrame.length += 1; // for first frame of next program
        transitionFrame.current = 0;

        memcpy(transitionFrame.to, PREPARE_BUFFERS, FRAME_SIZE);
        memcpy(transitionFrame.from, lastRawFrame, FRAME_SIZE);
    } else
        transitionFrame.length = 0;

    CS_BEGIN(bodySemaphore);
    running = true;
    const TickType_t programTimeIncrement = 1000.0 / taskParams->fps / portTICK_PERIOD_MS;
    const TickType_t transitionTimeIncrement = 1000.0 / PROGRAM_TRANSITION_FPS / portTICK_PERIOD_MS;
    CS_END(bodySemaphore);

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
    uint32_t bufferStartFrame = 0;
    uint32_t nextBufferFrame = 0;

    uint16_t bufferFrameIndex = 0;
    uint16_t stallTicks = 0;
    while (1) {
        CS_BEGIN(bodySemaphore);

        bool stalled = false;
        if (bufferFrameIndex >= bufferFramesCount) {
            if (taskParams->countFrames <= BUFFER_FRAMES_COUNT) {
                // Программа целиком уместилась в буфер — подкормки для неё нет и не
                // будет, виток это просто её закольцовка.
                bufferFrameIndex = 0;
            }
            else if (BUFFERED_FRAMES != -1 && BUFFERED_FROM == (int32_t)nextBufferFrame) {
                // Свопаем только на ту пачку, которую ждём: прийти могла и лишняя —
                // повторный запрос из слота заставляет задачу перечитать пачку.
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
            }
            else {
                // Пачка не приехала — замираем на последнем кадре, как буферизация
                // видео на медленном канале. Играть буфер заново нельзя: тогда запрос
                // уходил бы на каждом витке, копился в слоте, и задача читала бы пачку
                // дважды — вторым чтением уже в играющий буфер.
                stalled = true;
            }
        }

        // Начало буфера: первый кадр таска или первый кадр после свопа.
        if (!stalled && bufferFrameIndex == 0) {
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

        if (buffer == NULL)
            break;

        // передаём кадр на ленту
        int offset = bufferFrameIndex * FRAME_SIZE;

        TickType_t xTimeIncrement = transitionTimeIncrement;
        if (stalled) {
            // lastRawFrame не трогаем — на ленту снова уходит последний кадр. Именно
            // шлём, а не молчим: в замирании должен работать SetBrightness.
            xTimeIncrement = programTimeIncrement;
        }
        else if (!transitionFrame.getNextFrame(lastRawFrame)) {
            memcpy(lastRawFrame, buffer + offset, FRAME_SIZE);
            xTimeIncrement = programTimeIncrement;
        }

        for (int i = 0; i < BUFF_SIZE; i++)
            FRAME_BUFFER[i] = lastRawFrame[i] * brightness / 0xFF;

        spi_send_data(FRAME_BUFFER, FRAME_SIZE);

        bool exit = requestedForStop;
        requestedForStop = false;

        CS_END(bodySemaphore);

        if (exit) break;

		vTaskDelayUntil(&lastWakeTime, xTimeIncrement);

        // в замирании индекс не двигаем: буфер доигран, ждём пачку
        if (!stalled)
            bufferFrameIndex++;
    }

    CS_BEGIN(bodySemaphore);
    running = false;
    CS_END(bodySemaphore);

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

    vTaskDelay(1);
}

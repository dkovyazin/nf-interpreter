//-----------------------------------------------------------------------------
//
//                   ** WARNING! **
//    This file was generated automatically by a tool.
//    Re-running the tool will overwrite this file.
//    You should copy this file to a custom location
//    before adding any customization in the copy to
//    prevent loss of your changes when the tool is
//    re-run.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_LedPixelController.h"
#include "esp_log.h"
#include "interoplib_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <task.h>
#include <stdio.h>
#include <string.h>

// LEDTREES: захват/отдача — обычные вызовы, а не assert.
//
// Раньше здесь было assert(xSemaphoreTake(...) == pdTRUE). Работало это только
// потому, что ESP-IDF подменяет assert своим заголовком и при
// CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE=y (дефолт) под NDEBUG разворачивает его в
// ((void)(expr)) — то есть выражение всё же вычисляется. Стоит выставить эту опцию в
// n — и все захваты семафоров молча исчезнут из RTM-прошивки: плеер начнёт рвать
// кадры и подвисать, а причину будет не найти. Побочный эффект внутри assert не
// прячем; при portMAX_DELAY take не может вернуть ошибку иначе как на NULL-семафоре.
#define CS_BEGIN(s) ((void)xSemaphoreTake(s, portMAX_DELAY))
#define CS_END(s) ((void)xSemaphoreGive(s))

using namespace interoplib::interoplib;

static TaskHandle_t LedTask;
static spi_device_handle_t spi = NULL;
static spi_host_device_t SPI_HOST = SPI2_HOST;
static DMA_ATTR uint8_t FRAME_BUFFER[BUFF_SIZE];
static uint16_t PREPARED_FRAMES = 0;
// volatile: BUFFERED_FRAMES и CURRENT_PLAY_BUFFER делят LedTask (core 1) и задача
// подкормки (core 0) без блокировок — иначе оптимизатор вправе закешировать их в
// регистре на весь цикл (вся единица трансляции у него перед глазами).
static volatile short BUFFERED_FRAMES = 0;
static DMA_ATTR uint8_t* PREPARE_BUFFERS;    // буфер для подготовки кадров перед воспроизведением
static DMA_ATTR uint8_t* PROGRAM1_BUFFERS;   // 1-й буфер воспроизведения
static DMA_ATTR uint8_t* PROGRAM2_BUFFERS;   // 2-й буфер воспроизведения
static volatile uint8_t CURRENT_PLAY_BUFFER = 1;
static int LEDS_COUNT = 0;
static int FRAME_SIZE = 0;

static volatile bool running = false;
static volatile bool requestedForStop = false;
static SemaphoreHandle_t bodySemaphore = NULL;
static SemaphoreHandle_t joinSemaphore = NULL;
static SemaphoreHandle_t writeSemaphore = NULL;

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
        uint8_t lenght;
        uint8_t from[BUFF_SIZE];
        uint8_t to[BUFF_SIZE];

        bool getNextFrame(uint8_t* frameData) {
            if (current < lenght) {
                for (int i = 0; i < BUFF_SIZE; ++i) {
                    uint8_t a = (current +1)*0xFF / lenght;
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

// LEDTREES: нативная подкормка кадров из файла кэша.
//
// Раньше LedTask на каждый выпитый буфер (50 кадров, ~2 с) постил managed-событие,
// а C# читал 240 КБ с SD и заливал их 50 interop-вызовами. Это держало SD-латентность
// (120-240 мс) на CLR-потоке диспетчера событий 24/7, а на MAIN конкурировало с
// раздачей программ по TCP: не успел за виток — BUFFERED_FRAMES остаётся -1 и лента
// повторяет буфер (рывок). Здесь то же самое делает отдельная FreeRTOS-задача,
// читая сразу в play-буфер: 0 interop-переходов, 1 копия вместо трёх, CLR не при делах.
//
// Приоритет ниже LedTask — подкормка никогда не вытесняет вывод кадра.
#define FEED_CLOSE 0xFFFFFFFFu
#define FEED_TASK_PRIORITY (CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT - 1)

// Запрос пачки несёт НОМЕР ПОКОЛЕНИЯ источника вместе с индексом кадра. Слот
// уведомления один, и запрос, поставленный старым LedTask, может быть подхвачен
// задачей уже ПОСЛЕ смены программы — сверки поколения внутри FeedFrames тут мало:
// та снимает его на входе и увидит уже новое значение. Поколение в самом запросе
// позволяет опознать и выбросить такой запрос-призрак.
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

// Поколение источника: растёт на каждый SetSource/ClearSource. Подкормка сверяет его
// на каждом кадре и бросает пачку, если источник сменился под ней. Без этого смена
// программы даёт наложение: старый LedTask успел заказать пачку, задача читает кадры
// СТАРОЙ программы в свободный буфер, а новый LedTask тем временем кладёт туда же
// первые 50 кадров новой — и недочитавший feeder затирает их старыми. В managed-версии
// эту роль играла проверка task.IsDisposed на каждом кадре.
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

    // LEDTREES: обязательно обнулить — иначе первый переход StartPlay/SetFull
    // фейдится «из мусора» кучи (случайные цвета на первом разгорании)
    lastRawFrame = new uint8_t[FRAME_SIZE];
    memset(lastRawFrame, 0, FRAME_SIZE);

    esp_err_t ret;

    if (spi != NULL) {
        ret = spi_bus_remove_device(spi);
        ESP_ERROR_CHECK(ret);

        ret = spi_bus_free(SPI_HOST);
        ESP_ERROR_CHECK(ret);

        spi = NULL;
    }

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

    writeSemaphore = xSemaphoreCreateMutex();
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
// Ошибку не «чинит»: BUFFERED_FRAMES остаётся -1, LedTask повторит текущий буфер
// (лента крутит последние 50 кадров вместо чёрного экрана), а следующий виток
// попробует снова — файл переоткроется.
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

    // Сдвиг на startFrame — фаза устройства в сборке. Программа закольцована,
    // поэтому сдвиг подкормки тождественен старту с этого кадра (та же логика,
    // что была в managed-обработчике).
    uint32_t frame = (fromFrame + startFrame) % countFrames;
    long expectedPos = -1;

    // Неиграющий буфер. Пока мы здесь, LedTask не может его переназначить: свой
    // буфер он выбирает либо на старте (LedTask_Start ждёт feedMutex), либо на витке,
    // увидев BUFFERED_FRAMES != -1 — а его мы выставляем только на выходе.
    uint8_t* target = (CURRENT_PLAY_BUFFER == 1) ? PROGRAM2_BUFFERS : PROGRAM1_BUFFERS;

    for (uint16_t i = 0; i < BUFFER_FRAMES_COUNT; i++) {
        // источник сменили под нами — буфер уже принадлежит новой программе,
        // дописывать в него кадры старой нельзя (BUFFERED_FRAMES не трогаем:
        // пусть LedTask доиграет текущий буфер, новую пачку закажет следующий)
        if (SOURCE_GENERATION != generation)
            return;

        if (frame >= countFrames)
            frame = 0; // виток программы

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
        if (fread(dst, 1, frameSize, *file) != frameSize) {
            ESP_LOGE("interoplib", "feed: кадр %u не дочитан", (unsigned)frame);
            fclose(*file);
            *file = NULL;
            return;
        }

        // кадр в файле короче буфера устройства — хвост гасим, иначе в нём
        // останутся байты предыдущей программы
        if (frameSize < FRAME_SIZE)
            memset(dst + frameSize, 0, FRAME_SIZE - frameSize);

        expectedPos = offset + frameSize;
        frame++;
    }

    // Публикуем пачку только если источник за время чтения не менялся.
    if (SOURCE_GENERATION == generation)
        BUFFERED_FRAMES = BUFFER_FRAMES_COUNT;
}

void FeedTask_Handler( void * pvParameters )
{
    (void)pvParameters;
    FILE* file = NULL;

    while (1) {
        uint32_t request = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &request, portMAX_DELAY) != pdTRUE)
            continue;

        // Источник менялся — отпускаем файл до всего остального: пока он открыт,
        // managed не сможет ни удалить, ни переписать кэш (IO в nanoFramework
        // эксклюзивен). Проверяем на КАЖДОМ пробуждении, а не только по FEED_CLOSE:
        // тот занимает единственный слот уведомления и может быть затёрт запросом
        // пачки от ещё живого старого LedTask.
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

        // Запрос-призрак от старого LedTask: программу уже сменили, а уведомление
        // осталось в слоте. Поколение внутри FeedFrames такое не ловит — оно снимается
        // на входе и уже новое.
        if (FEED_REQ_EPOCH(request) != (SOURCE_GENERATION & 0x7FFFu))
            continue;

        // под мьютексом: LedTask_Start ждёт его перед стартом нового таска, чтобы
        // недочитанная пачка старой программы не легла в буфер новой
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
    // FRAME_SIZE, и при frameSize > FRAME_SIZE последний уедет за конец
    // PROGRAM1/2_BUFFERS. Managed это проверяет (IsValidFrameSize), но сверяет с
    // константой DEVICE_FRAME_SIZE, а не с фактическим FRAME_SIZE = pixelCount*12,
    // — на кучу это пускать нельзя даже при битом заголовке кэша.
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
    SOURCE_GENERATION++;   // пачка, читающаяся прямо сейчас, увидит это и бросит чтение
    CS_END(sourceSemaphore);

    // Будим задачу на ЛЮБУЮ смену источника, не только на снятие: по SOURCE_DIRTY она
    // закроет старый файл. Иначе он висел бы открытым до следующей пачки, а у короткой
    // программы (countFrames <= BUFFER_FRAMES_COUNT) пачек не бывает вовсе — и managed
    // не смог бы перезаписать кэш.
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
        BUFFERED_FRAMES = 1; // сбрасываем счётчик фуфферизованных фреймов воспроизведения
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

    if (spi == NULL) {
        hr = CLR_E_BUSY;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    xSemaphoreTake(bodySemaphore, portMAX_DELAY);

    // send exactly the managed array length (capped at frame size):
    // copying FRAME_SIZE unconditionally read past the end of shorter arrays
    int length = (int)data.GetSize();
    if (length > FRAME_SIZE)
        length = FRAME_SIZE;

    memcpy(FRAME_BUFFER, (void*)data.GetBuffer(), length);

    spi_send_data(FRAME_BUFFER, length);

    xSemaphoreGive(bodySemaphore);

    hr = S_OK;
}

// LEDTREES: заливка цветом с плавным переходом от текущего состояния ленты
// (transition мс; 0 — мгновенно). Обновляет lastRawFrame — следующий StartPlay
// фейдится из реального состояния (после гашения — плавное разгорание из чёрного,
// а не скачок от кадра давно остановленной программы). Яркость применяется как
// в цикле воспроизведения. Вызов блокирует CLR-поток на время перехода.
void LedPixelController::NativeSetFull( uint8_t red, uint8_t green, uint8_t blue, uint16_t transition, HRESULT &hr )
{
    if (spi == NULL) {
        hr = S_FALSE;
        return;
    }

    LedTask_Stop();
    LedTask_Join();

    xSemaphoreTake(bodySemaphore, portMAX_DELAY);

    const uint8_t target[3] = { red, green, blue };

    // именно FRAME_SIZE, не BUFF_SIZE: lastRawFrame выделен под фактический
    // pixelCount, при меньшем количестве пикселей BUFF_SIZE вышел бы за границу
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

    xSemaphoreGive(bodySemaphore);

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

    xSemaphoreTake(bodySemaphore, portMAX_DELAY);

    int i = (cell * 3 * 4) + (line * 3);
    FRAME_BUFFER[i + 0] = red;
    FRAME_BUFFER[i + 1] = green;
    FRAME_BUFFER[i + 2] = blue;

    // LEDTREES: состояние ленты для переходов (StartPlay/SetFull фейдятся отсюда)
    lastRawFrame[i + 0] = red;
    lastRawFrame[i + 1] = green;
    lastRawFrame[i + 2] = blue;

    spi_send_data(FRAME_BUFFER, FRAME_SIZE);

    xSemaphoreGive(bodySemaphore);

    hr = S_OK;
}

void LedTask_Start(uint16_t countFrames, uint8_t fps, uint16_t transition)
{
    // Дожидаемся, пока подкормка отпустит буферы: пачка, заказанная предыдущей
    // программой, может дочитываться прямо сейчас, а новый таск сразу скопирует
    // в свободный буфер PREPARE_BUFFERS. Поколение источника уже сменилось
    // (SetSource), поэтому пачка бросит чтение на ближайшем кадре — ждём единицы мс.
    if (feedMutex != NULL) {
        CS_BEGIN(feedMutex);
        CS_END(feedMutex);
    }

    CS_BEGIN(bodySemaphore);

    if (!running) {
        params.fps = fps;
        params.countFrames = countFrames;
        params.transition = transition;

        xTaskCreatePinnedToCore(
            LedTask_Handler,                                  /* Функция задачи. */
            "LedTask",                                    /* Ее имя. */
            4096,                                       /* Размер стека функции */
            (void*) &params,                                       /* Параметры */
            CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT,     /* Приоритет */
            &LedTask,                                     /* Дескриптор задачи для отслеживания */
            1);

    }

    CS_END(bodySemaphore);
}

void LedTask_Stop()
{
    CS_BEGIN(bodySemaphore);

    if (running) {
        requestedForStop = true;
        (void)xSemaphoreTake(joinSemaphore, portMAX_DELAY);  // не assert — см. CS_BEGIN
    }

    CS_END(bodySemaphore);
}

void LedTask_Join()
{
    (void)xSemaphoreTake(joinSemaphore, portMAX_DELAY);  // не assert — см. CS_BEGIN
    (void)xSemaphoreGive(joinSemaphore);
}

void LedTask_Handler( void * pvParameters )
{
    LedTaskParams* taskParams = (LedTaskParams*) pvParameters;

    uint16_t transitionTime = taskParams->transition;
    if (1000 / taskParams->fps < transitionTime) {
        transitionFrame.lenght = transitionTime / (1000 / PROGRAM_TRANSITION_FPS);
        transitionFrame.lenght += 1; // for first frame of next program
        transitionFrame.current = 0;

        memcpy(transitionFrame.to, PREPARE_BUFFERS, FRAME_SIZE);
        memcpy(transitionFrame.from, lastRawFrame, FRAME_SIZE);
    } else
        transitionFrame.lenght = 0;

    CS_BEGIN(bodySemaphore);
    running = true;
    const TickType_t programTimeIncrement = 1000.0 / taskParams->fps / portTICK_PERIOD_MS;
    const TickType_t transitionTimeIncrement = 1000.0 / PROGRAM_TRANSITION_FPS / portTICK_PERIOD_MS;
    CS_END(bodySemaphore);

	TickType_t lastWakeTime = xTaskGetTickCount();
    //TickType_t frameDelay = pdMS_TO_TICKS(1000 / taskParams->fps);

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

    uint32_t frameIndex = 0;
    uint16_t bufferFrameIndex = 0;
    while (1) {
        CS_BEGIN(bodySemaphore);

        if (bufferFrameIndex == 0) {
            BUFFERED_FRAMES = -1; // сбрасываем счётчик буфера воспроизведения

            if (taskParams->countFrames > BUFFER_FRAMES_COUNT) {
                // просим подготовить следующую порцию кадров, начиная с этого индекса
                // (читает задача подкормки прямо в неиграющий буфер — см. FeedFrames)
                if (FeedTask != NULL)
                    xTaskNotify(FeedTask, FEED_REQ(myGeneration, frameIndex + bufferFramesCount), eSetValueWithOverwrite);
            }
        }
        else if(bufferFrameIndex == bufferFramesCount) {
            // вычитали весь буфер воспроизведения

            // vTaskDelay(pdMS_TO_TICKS(2000));

            bufferFrameIndex = 0; // сбрасываем счётчик фреймов из буффера

            if (BUFFERED_FRAMES == -1) {
                // нет новых буфферизованных кадров, тогда просто начинаем заново читать тот же буфер что и был до этого
            }
            else {
                // есть новые буфферизованные кадры, значита меняем буффер воспроизведения на него
                if (CURRENT_PLAY_BUFFER == 1) {
                    buffer = PROGRAM2_BUFFERS;
                    CURRENT_PLAY_BUFFER = 2;
                }
                else {
                    buffer = PROGRAM1_BUFFERS;
                    CURRENT_PLAY_BUFFER = 1;
                }

                bufferFramesCount = BUFFERED_FRAMES;

                int prepareFramesFrom = frameIndex + bufferFramesCount;
                if(prepareFramesFrom >= taskParams->countFrames)
                    prepareFramesFrom = prepareFramesFrom - taskParams->countFrames;

                // просим подготовить следующую порцию кадров
                if (FeedTask != NULL)
                    xTaskNotify(FeedTask, FEED_REQ(myGeneration, prepareFramesFrom), eSetValueWithOverwrite);
            }
        }

        if (buffer == NULL)
            break;

        // передаём кадр на ленту
        int offset = bufferFrameIndex * FRAME_SIZE;
        //memcpy(FRAME_BUFFER, buffer + offset, FRAME_SIZE);

        TickType_t xTimeIncrement = transitionTimeIncrement;
        if (!transitionFrame.getNextFrame(lastRawFrame)) {
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

        bufferFrameIndex++;

        if (frameIndex == taskParams->countFrames - 1)
            frameIndex = 0;
        else
            frameIndex++;
    }

    CS_BEGIN(bodySemaphore);
    running = false;
    CS_END(bodySemaphore);

    (void)xSemaphoreGive(joinSemaphore);  // не assert — см. CS_BEGIN

    vTaskDelete(NULL);
}

void spi_send_data(const uint8_t *data, int len)
{
	uint32_t offset = 0;
	do {
        int tx_len = len;//((len - offset) < SPI_MAX_DMA_LEN) ? (len - offset) : SPI_MAX_DMA_LEN;

		spi_transaction_t t;
		memset(&t, 0, sizeof(t));
		t.length = tx_len * 8;
		t.tx_buffer = data + offset;

		spi_device_polling_transmit(spi, &t);
		offset += tx_len;
		break;
	} while (offset < len);

    vTaskDelay(1);
}

// LEDTREES: параметры вывода на ленту. Пины SPI и число пикселей приходят из
// managed-кода в NativeInit — здесь только то, что зашито в прошивку.
#define SPI_LEDS_FREQ_HZ        2500000

// Геометрия кадра. Обязана совпадать с managed interoplib.FrameGeometry
// (Pixels/Channels/BytesPerPixel) — общего источника через границу interop нет.
// MAX_PIXELS — потолок ленты (валидируется в NativeInit); фактический pixelCount
// приходит из managed и может быть меньше.
#define MAX_PIXELS              400
#define STRIPS_CNT              4
#define BYTES_PER_PIXEL         3
#define BUFF_SIZE               (STRIPS_CNT * MAX_PIXELS * BYTES_PER_PIXEL)

#define PROGRAM_TRANSITION_FPS  30

// Потолок темпа ВЫВОДА кадров на ленту (Гц). Продвижение по кадрам программы
// (contentFps = fps с учётом скорости) развязано от темпа вывода: до потолка
// outputFps==contentFps и лента идёт кадр-в-кадр как раньше, а выше — вывод
// держится на потолке, а по кадрам программы шагаем дробным аккумулятором
// (пропуск кадров). Так разгон скоростью не заказывает недостижимый железом темп.
// Потолок задан бюджетом SPI-кадра: 400 px = 4800 байт на 2.5 МГц ≈ 15 мс (+ latch)
// укладывается в период 60 fps (16,67 мс); выше лента физически не выводит.
#define LED_OUTPUT_FPS_MAX      60

// Кадров в буфере воспроизведения. ДОЛЖНО быть >= managed-кэпа предзагрузки
// (ProgramCache.MaxPrepareFrames): NativePrepareForPlay отвергает frame >= этого
// числа молча (S_FALSE).
#define BUFFER_FRAMES_COUNT     50

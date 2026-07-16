// LEDTREES: параметры вывода на ленту. Пины SPI и число пикселей приходят из
// managed-кода в NativeInit — здесь только то, что зашито в прошивку.
#define SPI_LEDS_FREQ_HZ        2500000
#define STRIPS_CNT              4
#define BUFF_SIZE               (STRIPS_CNT * 400 * 3)

#define PROGRAM_TRANSITION_FPS  30
#define BUFFER_FRAMES_COUNT     50

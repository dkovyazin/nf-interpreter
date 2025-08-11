#include <driver/gpio.h>

#define MOSI_PIN        41
#define MISO_PIN        39
#define CLK_PIN         40
#define CS_LEDS_PIN     21 // 37 for ver 2

#define SPI_LEDS_FREQ_HZ    2500000
#define STRIPS_CNT 		    4
#define BUFF_SIZE     STRIPS_CNT * 1000 * 3

#define PROGRAM_TRANSITION_FPS 30

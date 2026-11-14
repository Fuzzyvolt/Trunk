#include <stdio.h>
#include <unistd.h>
#include <gpiod.h>

#define LED_LINE 47   /* MIO47 = LED D3 */
#define BTN_LINE 51   /* MIO51 = button PB1 */

/* Stop leddaemon first (/etc/init.d/S99leddaemon stop): only one program can own a line. */
int main(void) {
    struct gpiod_chip *chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) { perror("open chip"); return 1; }

    struct gpiod_line *led = gpiod_chip_get_line(chip, LED_LINE);
    struct gpiod_line *btn = gpiod_chip_get_line(chip, BTN_LINE);
    if (!led || !btn) { perror("get line"); return 1; }

    if (gpiod_line_request_output(led, "blink", 0) < 0) {
        perror("request LED"); return 1;
    }
    if (gpiod_line_request_input(btn, "blink") < 0) {
        perror("request button"); return 1;
    }

    printf("Blinking LED D3. Press PB1 to stop.\n");
    int on = 0;
    while (gpiod_line_get_value(btn) == 0) {
        on = !on;
        gpiod_line_set_value(led, on);
        usleep(250000);            /* 250 ms */
    }

    gpiod_line_set_value(led, 0);  /* LED off */
    gpiod_line_release(led);
    gpiod_line_release(btn);
    gpiod_chip_close(chip);
    printf("Button pressed, bye!\n");
    return 0;
}

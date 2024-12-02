![PicoFlasher logo](https://raw.githubusercontent.com/X360Tools/PicoFlasher/master/picoflasher.png)

# PicoFlasher

Open source XBOX 360 NAND flasher firmware for Raspberry Pi Pico

## Wiring:

### Nand Flash
| Pico | Xbox |
| ------------- | ------------- |
| GP0  | SPI_MISO |
| GP1  | SPI_SS_N |
| GP2  | SPI_CLK |
| GP3  | SPI_MOSI |
| GP4  | SMC_DBG_EN |
| GP5  | SMC_RST_XDK_N |
| GND  | GND |

![Connection Diagram](https://raw.githubusercontent.com/15432/PicoFlasher/refs/heads/master/IMG_20241128_201825_526.jpg)

### ISD12xx Audible Feedback IC
|  | Pico | Trinity | Corona |
| ------------- | ------------- | ------------- | ------------- |
SPI_RDY | GP15 | FT2V4 | J2C2-A10
SPI_MISO | GP26 | FT2R7 | J2C2-B11
SPI_SS_N | GP27 | FT2R6 | J2C2-A11
SPI_CLK | GP28 | FT2T4 | J2C2-A8
SPI_MOSI | GP29 | FT2T5 | J2C2-B8

# Hanover Flipdot Display - Driver, Daemon and Client implemented in C

This project includes a complete implementation of the driver, daemon and a client for displaying stuff
on the 'Hanover Display' series flipdot display.

![RSS feed example](docs/HanoverAnimation.gif)

## Architecture

The basic architecture is as follows:

* Driver
  * opens and configures the RS485 tty dev with settings required by the display slave
  * handles encoding of supplied dot-array into a proprietary protocol used by the display
  * frees resources allocated for communication with the display
* Daemon
  * utlilizes the driver function for write operations to the bus
  * opens up a message queue to receive string to be rendered on the flipdot display
  * performs string optimization for best appearence on the display according to its size
  * decouples the display protocol logic in a way that it's kept in the driver, allowing for
  the client to be as dumb as possible
* Client
  * example client to demonstrate the function of the daemon + driver
  * messages are fetched from a defined RSS feed via curl, XML is parsed using libxml2's xmlTextReader
  * opens the message queue created by the daemon and writes each parsed '<title>' element onto the display
  * completely unaware of the size of the display

## Display Protocol

Hanover Display flipdot displays use a proprietary protocol on the RS485 bus. As with any RS485, the devices
must have an address on which to listen and reply from. In this case, the address on which the display listens
is set by a rotating switch on the main PCB (the back of the display needs to be opened). Address used in the
frames transmitted on the RS485 is ```rotating_switch_setting + 1```. Considering the lowest setting on the
switch is '0', the lowest possible address is 0x01. These's a connector coming from the board which allows the
displays to be daisy chained (A and B differential lines of the RS485 connected together for each chained device
respectively).

The RS485 bus needs to be configured as:
* Baud rate of 4800bps
* 8 databits
* 1 stopbit
* No parity
* Raw mode

The protocol follows an interesting encoding scheme in which each 'data byte' is taken as a hex value
(hex representation of high and low nibble). Then, each 'hex digit' is interpreted as an ASCII letter,
the value of each is used together. When a 'hex digit' is a 'letter' ('A','B','C','D','E','F'), the
value taken is of *uppercase* ASCII letter.
For example:

| Data byte | ASCII letters | Encoded byte |
|-----------|:-------------:|--------------|
|   0xFF    |    'F' 'F'    |  0x46 0x46   |
|   0x05    |    '0' '5'    |  0x30 0x35   |
|   0xB8    |    'B' '8'    |  0x42 0x38   |

The data is encoded on a column-by-column basis. A single column on the display consists of ```dot_rows / 8``` rows.
So, each 'display column' is encoded in ```dot_rows / 8``` each representing the 8 vertical dots from top to bottom.
Each frame being sent needs to set all the dots, there's no possibility to, for example, only set some in the middle
of the display without defining what states all the other ones should be. The device (probably) doesn't hold any
internal state and simply 'draws' the new bitmap sweeping from left to right column-by-column.

A complete frame consists has the following format:

| START |  ADDR  |  RES  |      DATA      | END |  CHKSUM  |
|-------|--------|-------|----------------|-----|----------|
| 1byte |  2byte | 2byte |  variable size |1byte|   2byte  |

* START  - frame opening byte - 0x02
* ADDR   - address as described above
* RES    - resolution of the screen, represented by the number of bytes contained in the ```DATA``` section
* DATA   - data bytes contaning where each bit '1' => ON, '0' => OFF
* END    - frame closing byte - 0x03
* CHKSUM - checksum of the frame's content calculated as
  * ```(((sum(all bytes in the frame) - START) & 0xFF) ^ 0xFF) + 1``` 

The display only reads frames, it never transmits anything back (no ACK frames), so the driver only takes
care of writing the data on the bus and 'hopes for the best' that the display will handle it.

## Fonts

I've used a C library called 'font8x8' which includes bitmaps of ASCII characters to be rendered in 8x8 pixels.
Each ASCII character is encoded as 8 bytes row-by-row from top-to-bottom, each byte representing one row, added
into a 2D array; each index of which corresponds to the ASCII value of a letter.

## Rotating the screen

I was thinking about adding the possibility to rotate the output to display by 180*, considering the display
can be mounted on either side, to the driver code. But, as it turns out, there's no need, the display's
firmware already supports this. You need to turn the display off, pop up the back cover and locate the jumper
array circles on the photo below.

![Screen roation jumper switch](docs/HanoverPCBJumper.png)

Each jumper position of marked by a letter. Locate the 'E' position and move the jumper to it. Close the lid
of the display and power it back on. The initial 'bootup' sequence will be the same as in 'normal' display
orientation, but all the following frames received will be rendered rotated by 180*.

## Example setup

I've used a Raspberry Pi4 Model B for my setup. The PrimeCell PL011 (/dev/ttyAMA0) is connected via
GPIO14 (TXD) and GPIO15 (RXD) to a RS485&CAN hat. You can also use one of the cheap USB<->RS485 adapters
and plug it into the RaspberryPi's or PC's USB port, as long as it shows up as a regular tty device, it
should be perfectly compatible. Raspberry Pi is connected over WiFi to the Internet in order to fetch
RSS feeds via client application to be written to the display.

The display needs to be supplied with a 24V power supply, I've used one with 2.5A current rating.

## Compiling and running the code

The following quick'n'dirty commands can be used to compile both the daemon and the client:
```gcc -o hanoverd driver/hanover_flipdot.c hanover.c -I . -I driver/ -I font8x8 -lrt``` 
```gcc -o hanover_client hanover_client.c -I . -I /usr/include/libxml2 -lrt -lcurl -lxml2``` 

This will result in two binaries which can be run as follows:
* ```./hanoverd &``` will run daemon in the background
* ```./hanover_client <max_news>``` will fetch the latest news from 'The Guardian's RSS feed and print them
  onto the display. The 'max_news' parameter defines the maximum number of news titles to fetch via RSS

Diagnostic data is logged into the syslog and you can read is by issuing:
```cat /var/log/syslog | grep hanover``` 

## Limitations and TODO's

The driver is completely versatile and written in a way that allows putting anything to the display.
However, the daemon will currently only accept ASCII strings and those will be printed only in a single
font.

Some future improvements and features I'm working on:
* more fonts, either existing or newly created
* drawing of bitmaps
* displaying weather information and icons
* replacing emojis in text with an actual emoji bitmap
* tranisition animations more complex than a simple sweep

## Copyright and license info

All code, not including the font submodule, is copyrighted © by the author (Luka Culic Viskota).
Code is released under the MIT license.

When including the source code into other projects, the headers including the author's name and license
conditions shall be included and left as-is.

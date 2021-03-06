# Building the TX module for the radio


## Required materials

For building the module, you'll need:

- 1x ESP32+LoRa board (screen is highly recommended, but not required).
- 1x [IPEX to SMA pigtail][pigtails], depending on the ESP32 modules you purchased (some modules come with one). Note that for optimal performance, you should make sure that feed line to the antenna has the approppriate size (Doc for this is TODO).
- 1x 5V Linear regulator (don't use a switching one!) with at least 1A output current.
For example, [L7805CV].
- 1x General purpose diode, to allow connecting the TX module via USB while the module is also being powered on by the radio. You can use [1N4148].
- 1x 5 pin dupont connector with 5x female pins. [Link][dupont]. Note that the listing has multiple sizes, make sure you select 5 pins.
- 1x Breadboard for soldering the components. Best size will depend on your skill level. Safest bet is to order a [kit with several sizes][breadboards], but you might need to cut one anyway. Use a sharp knife or a dremel-like too, whatever you feel more comfortable with.
- 1x 3D printed module case (Doc is TODO).

## Required tools

- Soldering iron.
- Dremel-like tool (optional, for cutting the breadboard).
- 3D Printer.

## Build

Wire the the components according to the following schematic.

[IMAGE WIP]

Which means:

- Bottom pin of the bottom bay is S.Port. Wire it pin 13 in the board.
- Next pin from the bottom in the module bay is ground. Connect it ground on the board.
- Third pin from the bottom is the battery voltage. Wire this to the V input of the 5V regulator.
- Connect the GND leg for the 5V regulator to any of the other GND points.
- Connect the output of the 5V regulator to the diode anode (the side without the mark).
- Connect the diode cathode (the side with the mark) to the 5V pin in the board.


TODO: Add link to the general configuration/usage once we write that document.

[pigtails]: https://www.aliexpress.com/item/5-pcs-lot-1-13-Cable-SMA-Jack-Bulkhead-to-Ufl-IPX-IPEX-Connector-Extension-Pigtail/2000223260.html
[L7805CV]: https://www.aliexpress.com/item/10pcs-L7805CV-L7805-7805-Voltage-Regulator-5V-1-5A-TO-220/32691111713.html
[1N4148]: https://www.aliexpress.com/item/Free-Shipping-100-PCS-1N4148-DO-35-IN4148-Silicon-Switching-Diode/2025724181.html
[dupont]: https://www.aliexpress.com/item/Free-Shipping-100set-2-54mm-1P-1Pin-Dupont-Connector-Dupont-Plastic-Shell-Plug-Dupont-Jumper-Wire/32262038907.html
[breadboards]: https://www.aliexpress.com/item/20pcs-5x7-4x6-3x7-2x8-cm-double-Side-Copper-prototype-pcb-Universal-Board-for-Arduino-Free/765383366.html


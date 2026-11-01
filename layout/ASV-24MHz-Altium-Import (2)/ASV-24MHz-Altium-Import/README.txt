Abracon ASV-24.000MHZ-EJ-T symbol and PCB footprint

The included EAGLE XML library contains both the schematic symbol and footprint. Import the .lbr in Altium Designer using File > Import Wizard > EAGLE Projects/Libraries, then save the imported component/footprint into Altium SchLib/PcbLib libraries.

Pin mapping (datasheet top view):
1 EN — tri-state enable/disable; high or open = oscillator enabled, low = high impedance
2 GND — ground/case
3 OUT — 24.000 MHz HCMOS clock output
4 VDD — 3.3 V supply

Footprint: nominal body 7.0 x 5.08 x 1.8 mm. Four SMD lands are 1.8 x 2.0 mm, arranged at X = +/-2.54 mm and Y = +/-2.00 mm. The datasheet recommended land pattern specifies these sizes and pitches. Pin 1 marker is shown at the lower-left in top view.

Source: ASV-24.000MHZ-EJ-T.pdf, Abracon ASV Series datasheet, outline dimension and pin assignment page. Check the imported pad numbering/orientation in Altium against the datasheet before placing on a production PCB.

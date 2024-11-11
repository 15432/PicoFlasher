import serial, struct, sys, os, msvcrt
import serial.tools.list_ports

def get_post(com):
    com.write(b"\x80" + b"\x00" * 4)
    size = com.read(1)[0]
    data = com.read(size)
    res = b""
    for c in data:
        res += struct.pack("B", c)
    return res

def start_smc(com):
    com.write(b"\xC0" + b"\x00" * 4)

import time

if __name__ == "__main__":
    msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
    port = serial.tools.list_ports.comports()[0]
    com = serial.Serial(port.device)
    start_smc(com)
    last = 0xFF
    while True:
        p = get_post(com)
        if (p):
            for c in p:
                if c != 0xFF and last == 0xFF:
                    print()
                if c != 0xFF:
                    print("%02X " % c, end="")
                last = c
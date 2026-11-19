#!/usr/bin/env python3
"""Textes de SMS for fun, servis par api->asset_read (flash, pas de RAM).
Usage : gen_assets.py smsfriends_assets.bin
L'ordre et les tailles doivent suivre les AS_* de smsfriends_app.c."""
import sys

HELP = [  # police 3x5 : 32 caractères max
    "PTT:ENVOI/LONG:RAPPEL F:EFFACE",
    "0:ESP *:ABC/abc/123/LONG:FREQ",
    "18-630 ET 840-1300 MHZ MEM:",
    "MENU:OK F:EFFACE *:POINT",
    "EX: 1 4 5 * 5 = 145.5 MHZ",
    "FREQUENCE MHZ",
]
STATUS = ["", "TX...", "ATT 0/3", "BLOQUE", "BANDE?", "ERREUR", "RECU", "ECHEC"]
CASE = ["ABC", "abc", "123"]
MARK = [" ...", "", " OK", " NON RECU"]  # par état H_SENT, H_RX, H_OK, H_FAIL
# Séquences BK4829 (registre, valeur) de l'app Beam ; 0xFF = pause en ms
DLY = 0xFF
RESET = [(0x3F, 0x0000), (0x59, 0x0068), (DLY, 30), (0x30, 0x0000)]
SEQS = [  # (nom, paires, position attendue) : voir SEQ_* dans smsfriends_app.c
    ("SETUP", [(0x70, 0x00C3), (0x72, 0x3065), (0x58, 0x00C1), (0x5C, 0x5665),
               (0x5D, 0x4700), (0x5E, 0x3204)] + RESET, 394),
    ("RX", [(0x02, 0x0000), (0x3F, 0x0000), (0x37, 0x9F1F), (0x30, 0x0000),
            (0x30, 0xBFF1), (0x3F, 0x3000), (0x59, 0x4068), (0x59, 0x3068)], 434),
    ("TX_LOAD", [(DLY, 30), (0x3F, 0x8000), (0x59, 0x8068), (0x59, 0x0068)], 466),
    ("TX_GO", [(DLY, 20), (0x59, 0x2868)], 482),
    ("TX_END", [(0x02, 0x0000), (DLY, 30)] + RESET + [(0x36, 0x0088)], 490),
]
T9 = [" 0", ".,'?!-1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9"]

def table(items, size):
    out = b""
    for s in items:
        b = s.encode("ascii")
        assert len(b) < size, s
        out += b + b"\0" * (size - len(b))
    return out

data = table(HELP, 33)
assert len(data) == 198
data += table(STATUS, 8)
assert len(data) == 262
data += table(CASE, 4)
assert len(data) == 274
data += table(T9, 8)
assert len(data) == 354
data += table(MARK, 10)
assert len(data) == 394
import struct
for name, pairs, pos in SEQS:
    assert len(data) == pos, (name, len(data))
    for r, v in pairs:
        data += struct.pack("<HH", r, v)
open(sys.argv[1], "wb").write(data)

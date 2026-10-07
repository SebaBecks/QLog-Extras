# OmniRig

[OmniRig](https://www.dxatlas.com/omnirig/) (Afreet, VE3NEA) talks to the rig and lets several programs use it at the same time, for example QLog, WSJT-X and JTDX. QLog uses it for frequency and mode. In this build, the [Rig Panel](rig-panel.md) and the [Panadapter](panadapter.md) work through it as well. Windows only.

Tested on a Yaesu FTdx101MP and an Icom IC-705.

## Setting up OmniRig

Install OmniRig 1 from dxatlas.com. Omni-Rig V2 works too; choose **Omnirig v2** in QLog instead of **Omnirig**.

In OmniRig, tab **RIG 1**:

| Field | Value |
|---|---|
| Rig type | your rig's file (e.g. FT-991, TS-590, IC-7300) |
| Port | the rig's CAT port |
| Baud rate, Stop bits | as in the rig's menu |
| Data bits / Parity | 8 / None |
| Poll int. | 300 ms |
| Timeout | 4000 ms |
| RTS, DTR | Low first, see below |

**On-line** must appear at the bottom of the window.

> **RTS and DTR:** on some rigs RTS or DTR keys the transmitter or CW, e.g. an Icom with *USB SEND*, or a Yaesu with PC KEYING on RTS/DTR. Set both to Low first. If the rig does not answer — some Yaesus need RTS for *CAT RTS* — set RTS to High and check that the rig does **not** start transmitting by itself.

Only OmniRig may have the CAT port open. Close other programs that talk to the rig directly; programs that go through OmniRig may stay open.

## Rig profile in QLog

Settings → Equipment → Rigs, new profile:

| Field | Value |
|---|---|
| Interface | Omnirig (or Omnirig v2) |
| Rig | Rig 1 |
| Rig features | Freq, Mode, VFO, PTT, Split |
| Default PWR | your rig's full power, e.g. 100. It sets the power scale and tells the 100 W FTdx101D from the 200 W MP. |

## What works

- Frequency, mode, VFO, and split with the transmit frequency.
- VFO B in the Rig Panel.
- The Panadapter: click and wheel tune the rig, including in data modes (DATA-U / DATA-L).
- **CW and CWR:** choosing CW in QLog, or clicking a CW spot, now sets the rig's *normal* CW. Before, Yaesu and Kenwood rigs got the reverse (CW-L / CW-R). The mapping is read from the rig file OmniRig has loaded, so no setting is needed.

Rig Panel meters and buttons, by rig:

| Rig | S meter | PWR / SWR / ALC | PRE ATT AGC NB NR |
|---|---|---|---|
| Yaesu FTDX…, FT-450, FT-710, FT-891, FT-950, FT-991, FT-2000 | yes | yes | yes |
| Icom IC-… | yes | yes | yes |
| Kenwood TS-590 | yes | yes | yes |
| Kenwood TS-480, TS-2000 | yes | – | – |
| others | – | – | – |

- Older Yaesus (FT-817, FT-857, FT-897, FT-1000 …) use another protocol, so only frequency and mode work on them.
- Kenwood has not been tested on a real rig yet.
- The meter scaling comes from Hamlib. For Yaesu the FTdx101 is the reference, so on another Yaesu the S meter may be a little off; correct it with **S cal** and **S9+** in the Rig Panel.

**Not available through OmniRig:** CW keying over CAT, RIT/XIT, power into the log, filter width, temperature, supply voltage and current, the tuner, the A/B button, and the rig's own scope in the Panadapter.

**Icoms:** OmniRig's rig file changes a few rig settings when it connects (CI-V echo, CI-V transceive and, on newer Icoms, *CW Normal Side*). That is OmniRig's rig file, not QLog.

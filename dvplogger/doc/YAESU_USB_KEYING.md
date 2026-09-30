# Yaesu USB DTR/RTS keying in DVPlogger

This note documents the current DVPlogger mapping and the behavior verified on
Yaesu rigs using the CP2105 Standard/keying virtual COM port (FTX-1 and
FTDX10 family).

## DVPlogger port mapping

For USB keying rigs:

- `CW:3` / `FSK:3` = DTR
- `CW:4` / `FSK:4` = RTS
- `PTT:3` = DTR
- `PTT:4` = RTS

The CP2105 direct-control test verified that these names are not swapped in the
DVPlogger USB driver: asserting DTR operates the rig key input, while asserting
RTS operates PTT when the corresponding rig menu assignments are selected.

## Recommended CW setup

For the currently verified FTX-1 setup:

- DVPlogger: `CW:3` (DTR)
- Rig: DTR assigned to CW keying
- Rig: `RPTT=OFF`
- Rig: use the rig's BK-IN / semi-break-in timing

In this configuration DVPlogger sends only the CW key waveform on DTR.  The rig
itself controls the TX/RX envelope according to its BK-IN setting.

If the rig is changed to `RPTT=RTS`, DTR keying alone is not sufficient: a PTT
envelope must also be asserted on RTS.  DVPlogger does not currently add a PTT
envelope around ordinary CW keying.  Do not assume that `PTT:4` automatically
causes RTS to be asserted for CW.

A future explicit "PTT-controlled CW" mode, if added, should use a sequence such
as RTS ON -> lead delay -> DTR CW keying -> tail delay -> RTS OFF.  It should be
kept distinct from rig-controlled BK-IN/semi-break-in operation.

## Recommended RTTY FSK setup

For USB FSK with the current FTX-1 setup:

- DVPlogger: `FSK:3` (DTR)
- DVPlogger: `PTT:4` (RTS)
- Rig: DTR assigned to RTTY/FSK keying
- Rig: RTS assigned to RPTT/PTT

Expected behavior during an RTTY message is:

1. RTS is asserted for PTT.
2. After the configured RTTY lead time, DTR carries the Baudot FSK waveform.
3. At the end of the message DTR returns to the idle/mark state.
4. RTS is released after the transmit sequence completes.

`RP` / RTTY polarity inversion changes the asserted state used for mark/space;
it does not change which physical control line is selected by `FSK:3/4`.

## FTX-1 CAT mode reporting

The FTX-1 CAT `IF` response reports the operating mode in P6.  DVPlogger treats
Yaesu mode `6` (RTTY-L) and `9` (RTTY-U) as `RTTY`, i.e. contest mode type `DG`.
This agrees with the FTX-1 CAT reference manual.

The diagnostic commands `rttytx`, `rttytest`, and `rttytest1` must test the
currently selected/focused radio, not SO2R's internal current TX radio.  Before
this was corrected, focusing RIG2 (FTX-1) while SO2R `tx_` still referred to a
different rig could incorrectly produce:

    RTTYTEST1: TX radio must be in RTTY/DG mode

although the selected FTX-1 was actually in RTTY mode.

## Diagnostic commands

CP2105 line identification:

    cp2105diag 1
    cp2105state 1 0    # DTR=0 RTS=0
    cp2105state 1 1    # DTR=1 RTS=0
    cp2105state 1 2    # DTR=0 RTS=1
    cp2105state 1 3    # DTR=1 RTS=1

Return both lines to the released state after a direct-control test:

    cp2105state 1 0

Runtime keying trace:

    usbkeytrace 1
    usbkeytrace status
    usbkeytrace 0

Expected trace with `CW:3`:

    USBTRACE CW ... line=DTR value=1 ...
    USBTRACE CW ... line=DTR value=0 ...

Expected RTTY trace with `FSK:3,PTT:4` should show RTS PTT control plus DTR FSK
transitions.  If `rttytest1` is rejected, its error now prints the selected rig
number, `opmode`, and `modetype` to make a mode-tracking problem visible.

## Verified vs. not yet verified

Verified so far:

- CP2105 DTR and RTS are not swapped by DVPlogger direct control.
- On the tested FTX-1 configuration, DTR assertion produces CW key action.
- On the tested FTX-1 configuration, RTS assertion produces PTT/TX action.
- Ordinary DVPlogger CW with `CW:3` toggles DTR and does not automatically
  assert RTS PTT.

Still to verify with runtime traces:

- Complete FTX-1 RTTY message: RTS PTT envelope plus DTR Baudot FSK waveform.
- The equivalent complete RTTY path on FTDX10.
- Whether a separate PTT-controlled CW mode is desirable in addition to the
  recommended rig-controlled BK-IN configuration.

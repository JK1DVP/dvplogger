# DVPlogger OTRSP/TCP Antenna Control Protocol

**Status:** DVPlogger implementation specification\
**Transport:** TCP\
**Default TCP port:** `12001`\
**Line terminator:** Carriage Return (`CR`, `0x0D`)

## 1. Purpose

This document describes the OTRSP/TCP protocol used between DVPlogger
and an external antenna-switch controller, together with the optional
DVPlogger extensions for antenna metadata and radio operating
information.

The design has two goals:

1.  Preserve interoperability with the standard OTRSP antenna-control
    mechanism currently used by DVPlogger.
2.  Allow an extension-aware antenna server to exchange antenna names
    and receive radio frequency/mode information for GUI display,
    automatic antenna tuners, filters, and other station peripherals.

The standard antenna-selection commands remain independent of the
extensions.

------------------------------------------------------------------------

## 2. Architecture and ownership of information

DVPlogger acts as the client and the antenna controller/server accepts
the TCP connection.

  -----------------------------------------------------------------------
  Information                         Authoritative side
  ----------------------------------- -----------------------------------
  Radio operating frequency           DVPlogger

  Radio operating mode                DVPlogger

  Band-to-antenna preference          DVPlogger

  Requested antenna allocation        DVPlogger

  Physical antenna number and name    Antenna server

  Actual switching hardware           Antenna server

  Hot-switch protection               DVPlogger currently performs TX/RX
                                      sequencing; hardware-side
                                      protection is also recommended
  -----------------------------------------------------------------------

DVPlogger determines the antenna allocation from its per-band preference
table. Before changing the allocation it waits until all enabled radios
are in RX and then waits an additional settling interval (currently 150
ms) before sending the OTRSP commands.

------------------------------------------------------------------------

## 3. Transport

The current implementation uses OTRSP commands over a TCP stream.

Default server endpoint:

``` text
TCP port 12001
```

Each protocol command is an ASCII/UTF-8 text line terminated by `CR`:

``` text
<command><CR>
```

Example:

``` text
AUX1 2\r
```

The extension-aware reference server may reply to accepted commands
with:

``` text
OK <command>\r
```

DVPlogger does not depend on these `OK` replies for normal antenna
switching.

------------------------------------------------------------------------

# Part I --- Standard OTRSP commands used by DVPlogger

## 4. AUX antenna-selection commands

DVPlogger uses the OTRSP AUX mechanism to communicate the requested
antenna number.

### 4.1 AUX1

``` text
AUX1 <antenna-number>
```

In the current DVPlogger implementation:

``` text
AUX1 = Radio 0 antenna selection
```

Example:

``` text
AUX1 2
```

requests antenna 2 for Radio 0.

### 4.2 AUX2

``` text
AUX2 <antenna-number>
```

In the current DVPlogger implementation:

``` text
AUX2 = Radio 1 antenna selection
```

Example:

``` text
AUX2 7
```

requests antenna 7 for Radio 1.

### 4.3 Antenna number 0

DVPlogger may send:

``` text
AUX1 0
```

or:

``` text
AUX2 0
```

when no usable antenna is allocated to that radio.

The physical interpretation of antenna 0 is implementation-dependent; an
antenna server should normally treat it as no antenna / disabled unless
its hardware requires another safe-state mapping.

### 4.4 Example allocation

``` text
AUX1 2
AUX2 1
```

means:

``` text
Radio 0 -> Antenna 2
Radio 1 -> Antenna 1
```

The standard AUX commands identify antenna numbers only. They do not
convey antenna names, radio frequency, operating band, operating mode,
or the reason an antenna was selected.

------------------------------------------------------------------------

## 5. DVPlogger antenna-switching sequence

DVPlogger calculates the desired antenna for each controlled radio from
its band and antenna-preference table.

When an allocation change is required:

``` text
Calculate requested allocation
        |
        v
Is any enabled radio transmitting?
        |
   yes  |----> wait
        |
       no
        |
        v
Wait 150 ms after all radios are RX
        |
        v
Send AUX1 / AUX2
        |
        v
Mark allocation active
```

This prevents DVPlogger from intentionally switching the antenna matrix
while a radio is transmitting.

The antenna server should nevertheless implement its own hardware
interlocks where appropriate.

------------------------------------------------------------------------

# Part II --- DVPlogger OTRSP Extensions

## 6. Design principles

The extensions add metadata without changing the standard `AUX1` /
`AUX2` switching commands.

Extension commands currently defined are:

``` text
?ANTLIST
ANTNAME <n> <name>
ANTLIST END

RADIO0 FREQ <frequency-hz>
RADIO0 MODE <mode>

RADIO1 FREQ <frequency-hz>
RADIO1 MODE <mode>
```

The extension has two distinct information flows:

``` text
Antenna server -> DVPlogger
    antenna names

DVPlogger -> Antenna server
    radio frequency
    radio mode
    antenna selection (standard AUX)
```

------------------------------------------------------------------------

## 7. Extension capability negotiation

DVPlogger does not immediately send `RADIOx` extension commands to every
OTRSP device.

After establishing the TCP connection, DVPlogger sends:

``` text
?ANTLIST
```

An extension-aware server responds with zero or more `ANTNAME` records
followed by:

``` text
ANTLIST END
```

Example:

``` text
DVPlogger -> ?ANTLIST

Server -> ANTNAME 1 40m Dipole
Server -> ANTNAME 2 Tribander
Server -> ANTNAME 3 50MHz 2el
Server -> ANTNAME 4 144/430/1200 GP
Server -> ANTNAME 5 2.4GHz antenna
Server -> ANTNAME 6 5.6GHz antenna
Server -> ANTNAME 7 Second triband DP
Server -> ANTNAME 8 Antenna 8
Server -> ANTNAME 9 Antenna 9
Server -> ANTLIST END
```

Receipt of:

``` text
ANTLIST END
```

indicates to DVPlogger that the peer supports the extension.

Only after this response does DVPlogger begin sending `RADIOx FREQ` and
`RADIOx MODE` metadata.

A standard OTRSP device that does not implement this extension can
ignore `?ANTLIST`. DVPlogger will continue using the standard AUX
antenna-selection mechanism and will not send `RADIOx` extension
traffic.

------------------------------------------------------------------------

## 8. ANTNAME

### 8.1 Syntax

``` text
ANTNAME <antenna-number> <antenna-name>
```

Example:

``` text
ANTNAME 2 Tribander
```

The antenna number identifies the same physical antenna used by:

``` text
AUX1 2
AUX2 2
```

### 8.2 Ownership

The antenna server is the authoritative source for antenna names because
it represents the physical antenna-switch installation.

DVPlogger may contain local/default names as a fallback, but names
received from an extension-aware server are used to update DVPlogger's
runtime antenna-name table.

### 8.3 Names containing spaces

Everything after the antenna number is treated as the antenna name.

Therefore:

``` text
ANTNAME 7 Second triband DP
```

has:

``` text
antenna-number = 7
antenna-name   = Second triband DP
```

------------------------------------------------------------------------

## 9. ANTLIST END

### 9.1 Syntax

``` text
ANTLIST END
```

This terminates the response to `?ANTLIST`.

It also acts as the capability indication for the current
implementation.

After receiving it, DVPlogger may send the `RADIOx` metadata commands
described below.

------------------------------------------------------------------------

# Part III --- Radio metadata extension

## 10. RADIOx FREQ

### 10.1 Syntax

``` text
RADIO<radio-number> FREQ <frequency-hz>
```

Current radio numbers are:

``` text
RADIO0
RADIO1
```

Examples:

``` text
RADIO0 FREQ 14025000
RADIO1 FREQ 7025000
```

The frequency is an integer in **Hz**.

Thus:

``` text
14025000 = 14.025000 MHz
7025000  = 7.025000 MHz
```

The protocol deliberately uses absolute frequency in Hz rather than only
a band identifier. This allows the information to be used by devices
such as:

-   automatic antenna tuners,
-   automatic filters,
-   amplifier/filter controllers,
-   antenna-selection displays,
-   station automation systems.

The receiving server can derive its own band classification from the
frequency.

### 10.2 Update behavior

DVPlogger sends a frequency record after extension negotiation and
subsequently when the frequency changes.

It is not intended to continuously retransmit an unchanged frequency on
every CAT polling cycle.

------------------------------------------------------------------------

## 11. RADIOx MODE

### 11.1 Syntax

``` text
RADIO<radio-number> MODE <mode>
```

Examples:

``` text
RADIO0 MODE CW
RADIO1 MODE USB
```

The mode string is the DVPlogger operating-mode representation.

Typical values may include:

``` text
LSB
USB
CW
CW-R
RTTY
RTTY-R
AM
FM
WFM
DV
```

A receiver should tolerate mode strings it does not recognize.

### 11.2 Update behavior

As with frequency, DVPlogger sends the mode after extension negotiation
and subsequently when the mode changes.

------------------------------------------------------------------------

# Part IV --- Combined operation

## 12. Connection example

A typical extension-aware connection is:

``` text
TCP connection established

DVPlogger -> ?ANTLIST

Server -> ANTNAME 1 40m Dipole
Server -> ANTNAME 2 Tribander
Server -> ANTNAME 3 50MHz 2el
Server -> ANTLIST END

DVPlogger -> RADIO0 FREQ 14025000
DVPlogger -> RADIO0 MODE CW
DVPlogger -> RADIO1 FREQ 7025000
DVPlogger -> RADIO1 MODE CW

DVPlogger -> AUX1 2
DVPlogger -> AUX2 1
```

The server can now interpret the station state as:

``` text
Radio 0
  14.025000 MHz
  CW
  |
  +--> Antenna 2 "Tribander"

Radio 1
   7.025000 MHz
  CW
  |
  +--> Antenna 1 "40m Dipole"
```

------------------------------------------------------------------------

## 13. Frequency change without antenna change

Suppose Radio 0 moves from:

``` text
14.025000 MHz CW
```

to:

``` text
14.035000 MHz CW
```

but remains on the same antenna.

DVPlogger only needs to send:

``` text
RADIO0 FREQ 14035000
```

No new AUX command is required unless the antenna allocation also
changes.

This is important for an automatic antenna tuner because the tuner may
need the exact frequency even though the antenna switch position remains
unchanged.

------------------------------------------------------------------------

## 14. Band change causing antenna change

Suppose Radio 0 changes from 7 MHz to 14 MHz and DVPlogger determines
that the appropriate antenna changes from antenna 1 to antenna 2.

The metadata may become:

``` text
RADIO0 FREQ 14025000
RADIO0 MODE CW
```

After DVPlogger's RX/hot-switch protection sequence, the antenna
selection becomes:

``` text
AUX1 2
```

The server can therefore distinguish:

-   the radio's operating state, and
-   the actual requested antenna-switch position.

------------------------------------------------------------------------

## 15. Mode change

For a mode change at an unchanged frequency:

``` text
RADIO0 MODE USB
```

is sufficient.

The antenna allocation does not necessarily change.

This metadata may nevertheless be useful to other station devices.

------------------------------------------------------------------------

# Part V --- Server behavior

## 16. Minimum standard-compatible server

A server that only implements the antenna-switching function needs to
understand:

``` text
AUX1 <n>
AUX2 <n>
```

It may ignore:

``` text
?ANTLIST
```

and all unknown commands.

Such a server remains usable by DVPlogger.

------------------------------------------------------------------------

## 17. Extension-aware server

An extension-aware server should:

1.  Accept `?ANTLIST`.
2.  Return its antenna definitions as `ANTNAME` records.
3.  Terminate the list with `ANTLIST END`.
4.  Accept `RADIO0/1 FREQ`.
5.  Accept `RADIO0/1 MODE`.
6.  Continue accepting standard `AUX1/AUX2`.
7.  Maintain the association between each radio and its current antenna.

A GUI can therefore display, for example:

``` text
Radio 0
14.025000 MHz  CW
        |
        +----> ANT 2
               Tribander

Radio 1
 7.025000 MHz  CW
        |
        +----> ANT 1
               40m Dipole
```

------------------------------------------------------------------------

## 18. Antenna-name persistence

Because the antenna server is the authoritative source for physical
antenna definitions, an extension-aware server should normally persist
antenna names locally.

For example:

``` json
{
  "antenna_names": {
    "1": "40m Dipole",
    "2": "Tribander",
    "3": "50MHz 2el"
  }
}
```

The storage format is an implementation detail and is not part of the
wire protocol.

------------------------------------------------------------------------

# Part VI --- Compatibility and implementation notes

## 19. Backward compatibility

The extension is designed so that standard antenna switching continues
to use:

``` text
AUX1
AUX2
```

DVPlogger probes for the extension using:

``` text
?ANTLIST
```

If the server does not complete the extension handshake with:

``` text
ANTLIST END
```

DVPlogger does not enable `RADIOx FREQ/MODE` transmission.

Therefore the extended implementation can continue to operate with a
conventional OTRSP antenna controller.

------------------------------------------------------------------------

## 20. Unknown commands

For interoperability, implementations should ignore unsupported commands
rather than close the TCP connection.

This is especially important for standard OTRSP devices receiving:

``` text
?ANTLIST
```

and for future extension-aware devices receiving newer DVPlogger
metadata commands.

------------------------------------------------------------------------

## 21. Reconnection

After a TCP reconnection, extension capability should be considered
unknown again.

DVPlogger repeats:

``` text
?ANTLIST
```

and waits for:

``` text
ANTLIST END
```

before resuming `RADIOx` metadata transmission.

After successful negotiation, current frequency and mode information
should be resent so the server can reconstruct the complete current
state without depending on information from the previous TCP session.

The antenna allocation is also resent by the existing DVPlogger
antenna-control logic when synchronization is required.

------------------------------------------------------------------------

# Part VII --- Command reference

## 22. Standard OTRSP commands currently used

  Direction              Command      Purpose
  ---------------------- ------------ ----------------------------
  DVPlogger -\> Server   `AUX1 <n>`   Select antenna for Radio 0
  DVPlogger -\> Server   `AUX2 <n>`   Select antenna for Radio 1

## 23. DVPlogger extension commands

  -----------------------------------------------------------------------
  Direction               Command                 Purpose
  ----------------------- ----------------------- -----------------------
  DVPlogger -\> Server    `?ANTLIST`              Request antenna
                                                  definitions / probe
                                                  extension support

  Server -\> DVPlogger    `ANTNAME <n> <name>`    Define physical antenna
                                                  name

  Server -\> DVPlogger    `ANTLIST END`           End antenna list and
                                                  advertise extension
                                                  support

  DVPlogger -\> Server    `RADIO0 FREQ <Hz>`      Radio 0 operating
                                                  frequency

  DVPlogger -\> Server    `RADIO1 FREQ <Hz>`      Radio 1 operating
                                                  frequency

  DVPlogger -\> Server    `RADIO0 MODE <mode>`    Radio 0 operating mode

  DVPlogger -\> Server    `RADIO1 MODE <mode>`    Radio 1 operating mode
  -----------------------------------------------------------------------

------------------------------------------------------------------------

# Part VIII --- Future extensions

The current syntax intentionally leaves room for additional
station-control metadata.

Possible future commands include:

``` text
RADIO0 PTT RX
RADIO0 PTT TX

RADIO0 BAND 14
RADIO0 POWER 100

ANTSTATUS 2 OK
TUNER0 STATE TUNED
```

These are **not defined by the current protocol** and must not be
assumed to be implemented.

In particular, band information does not need to be added merely for
antenna-server display because the receiving device can derive the band
from:

``` text
RADIOx FREQ <Hz>
```

Keeping frequency as the primary radio-state value also makes the
extension more useful to automatic antenna tuners and other
frequency-sensitive station equipment.

------------------------------------------------------------------------

## 24. Summary

The protocol deliberately separates three concepts:

``` text
Physical antenna identity
    -> owned by antenna server
    -> ANTNAME

Radio operating state
    -> owned by DVPlogger
    -> RADIOx FREQ / MODE

Antenna switching
    -> requested by DVPlogger
    -> standard OTRSP AUX1 / AUX2
```

This keeps the existing OTRSP antenna-switch implementation intact while
allowing richer station information to be exchanged when both endpoints
support the DVPlogger extension.

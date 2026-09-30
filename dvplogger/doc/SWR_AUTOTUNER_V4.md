# SWR / Auto Tuner (v4)

Add these optional tokens to each RIG specification.

- `T:0`: disabled; Alt-T keeps its former manual 8 W PTT toggle behavior.
- `T:1` / `T:2`: pulse KEY1 / KEY2.
- `T:3` / `T:4`: pulse USB DTR / RTS.
- `T:5`: start the rig's internal tuner through CAT/CI-V.
- `SWR:200`: automatically start tuning after two new readings above 2.00:1.
  Omit this token (or use zero) to disable automatic triggering.
- `TH:1500`: hold an external tuner contact for 1500 ms (500 to 3000 ms).

Examples:

```text
T:1,SWR:200,TH:1500
T:5,SWR:250
```

While transmitting, the logger asks the rig for SWR about five times per
second (CAT response scheduling may reduce the effective rate).
The right LCD's S-meter field changes to a compact SWR indication such as
`R1.5` while transmitting; it shows `R---` until a fresh value is received.
Automatic triggering has a 30-second re-trigger inhibit. Alt-T starts a tune
cycle when `T:` is configured; pressing Alt-T again cancels it. An external
tune cycle uses 8 W, releases the configured contact after `TH:`, and stops on
a satisfactory SWR or after 10 seconds. Rig-internal tuners control their own
carrier. QMX uses SWR Tune mode (`MD8`/`MD0`) and has no internal ATU.

CAT commands used are Icom CI-V `15 12`, Yaesu `RM6`, Kenwood `RM1` + `RM`,
and QMX `SW`. Internal tuner commands are Icom CI-V `1C 01 02`, Yaesu
`AC002`, and Kenwood `AC111`. Command availability varies by model and
firmware; test at minimum power into a dummy load before enabling `SWR:`.
